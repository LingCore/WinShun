// WinShunCloudBackup.exe: the winshun.cloudbackup plugin ("云备份", GitHub
// #276) in the plugin library. Win顺's settings backup kept on a WebDAV server
// or in a private GitHub repository, either or both: uploaded by itself when
// the settings changed (a while after start, then every half hour), listed
// and restored from the launcher ("cloud").
//
// Win顺 makes the backup and restores it (the tools backup_export and
// backup_restore, the winshun:backup permission); this plugin only carries
// the file. Where it goes and how is in Cloud.cpp, the requests in Http.cpp.
// All of the network runs on one worker thread; the protocol thread answers
// queries from what the worker last learned.

#include "winshun.h"

#include "Cloud.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

using winshun::Effects;
using winshun::Item;
using winshun::Items;
using winshun::Json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kFirst = 2min; // after start: login settles, Win顺's own backup comes first
constexpr auto kEvery = 30min;
constexpr auto kAfterSettings = 8s; // an address or a password being typed makes one try
constexpr auto kListingFor = 60s; // how long a listing answers the launcher
constexpr std::size_t kMaxRows = 40;

// What is known of one place ("webdav", "github").
struct Place {
    std::string id; // Target::id() the rest belongs to
    std::string hash; // of the backup uploaded last (backup_export's)
    std::string name; // its file there
    std::string error; // why the last try failed
    std::vector<cb::Remote> backups; // newest first
    bool listed = false;
    Clock::time_point triedAt; // the last look there, whatever came of it
};

struct Wanted {
    std::string place;
    std::string name;
    std::string sha;
};

std::string readFile(const std::string& path)
{
    std::string bytes;
    FILE* f = nullptr;
    if (_wfopen_s(&f, winshun::wide(path).c_str(), L"rb") != 0 || !f)
        return bytes;
    char chunk[64 * 1024];
    for (std::size_t got; (got = std::fread(chunk, 1, sizeof chunk, f)) > 0;)
        bytes.append(chunk, got);
    std::fclose(f);
    return bytes;
}

bool writeFile(const std::string& path, const std::string& bytes)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, winshun::wide(path).c_str(), L"wb") != 0 || !f)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

void removeFile(const std::string& path)
{
    ::DeleteFileW(winshun::wide(path).c_str());
}

std::string computerName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    return ::GetComputerNameW(name, &size) ? winshun::utf8(std::wstring(name, size)) : std::string();
}

std::string sizeText(long long bytes)
{
    char text[32];
    if (bytes >= 1024 * 1024)
        std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(text, sizeof text, "%lld KB", std::max(1ll, bytes / 1024));
    return text;
}

class CloudBackup : public winshun::Plugin {
public:
    ~CloudBackup() override { stop(); }

protected:
    std::string name() const override { return "winshun.cloudbackup"; }

    void initialized() override
    {
        // Left by a run that Win顺 ended (a restore restarts it).
        removeFile(uploadFile());
        removeFile(restoreFile());
        {
            const std::lock_guard lock(m_mutex);
            m_config = cb::settingsFrom(settings());
            m_zh = chinese();
            loadState();
            m_nextAuto = Clock::now() + kFirst;
        }
        m_worker = std::thread([this] { work(); });
    }

    void settingsChanged(const Json& settings) override
    {
        const std::lock_guard lock(m_mutex);
        m_config = cb::settingsFrom(settings);
        m_zh = chinese();
        for (auto& [key, place] : m_places) {
            place.listed = false; // perhaps another place now: asked again
            place.triedAt = {};
        }
        m_nextAuto = Clock::now() + kAfterSettings;
        m_cv.notify_all();
    }

    void shuttingDown() override { stop(); }

    Items query(const winshun::Query& q) override
    {
        const std::lock_guard lock(m_mutex);
        return rows(q.text);
    }

    Json queryResult(const winshun::Query& q) override
    {
        const std::lock_guard lock(m_mutex);
        // A listing that is old (or none yet): read again, the rows follow.
        bool stale = false;
        for (const char* key : {"webdav", "github"}) {
            if (!configured(key))
                continue;
            const Place& place = m_places[key];
            stale = stale || Clock::now() - place.triedAt > kListingFor;
        }
        if (stale && !m_working) {
            m_wantList = true;
            m_cv.notify_all();
        }
        const bool busy = m_working || m_wantList || m_wantNow;
        return Json::Object {{"items", winshun::itemsJson(rows(q.text))}, {"rerun", busy ? 1 : 30}};
    }

    Effects action(const winshun::ActionRequest& a) override
    {
        const std::string what = a.item["data"]["do"].toString();
        const std::lock_guard lock(m_mutex);
        const bool zh = m_zh;
        if (what == "backup") {
            m_wantNow = true;
            m_cv.notify_all();
            return {winshun::effect::notify(zh ? "正在备份到云端…" : "Backing up to the cloud…"), winshun::effect::keep()};
        }
        if (what == "ask") {
            const Json& data = a.item["data"];
            const auto parsed = cb::parseName(data["name"].toString());
            const std::string when = parsed ? parsed->when : data["name"].toString();
            Json restore = data;
            restore.set("do", "restore");
            Items confirm;
            confirm.push_back(Item("confirm", zh ? "恢复 " + when + " 的备份？" : "Restore the backup of " + when + "?",
                zh ? "Enter 确认：现在的设置先在这台电脑上存一份，然后 Win顺 重启，换成这份备份"
                   : "Enter confirms: the settings of now are saved on this computer first, then Win顺 restarts with this backup")
                .withIcon("ph:clock-counter-clockwise").withData(restore));
            confirm.push_back(Item("back", zh ? "不恢复" : "Do not restore", zh ? "Enter 回到列表" : "Enter goes back to the list")
                .withIcon("ph:arrow-u-up-left").withData(Json::Object {{"do", "back"}}));
            return {winshun::effect::replace(confirm), winshun::effect::keep()};
        }
        if (what == "back")
            return {winshun::effect::replace(rows({})), winshun::effect::keep()};
        if (what == "restore") {
            const Json& data = a.item["data"];
            m_restore = Wanted {data["place"].toString(), data["name"].toString(), data["sha"].toString()};
            m_cv.notify_all();
            return {winshun::effect::notify(zh ? "正在下载备份，完成后 Win顺 会重启" : "Downloading the backup; Win顺 restarts when it is here"),
                winshun::effect::hide()};
        }
        return {winshun::effect::keep()};
    }

private:
    std::string uploadFile() const { return dataDir() + "\\upload.zip"; }
    std::string restoreFile() const { return dataDir() + "\\restore.zip"; }
    std::string stateFile() const { return dataDir() + "\\state.json"; }

    void stop()
    {
        m_stop = true;
        {
            const std::lock_guard lock(m_mutex);
            m_cv.notify_all();
        }
        if (m_worker.joinable())
            m_worker.join();
    }

    // ---- the rows (m_mutex held) ----------------------------------------------------

    bool configured(const std::string& key) const { return key == "webdav" ? m_config.webdav() : m_config.github(); }

    Items rows(const std::string& text)
    {
        const bool zh = m_zh;
        Items items;
        if (!m_config.webdav() && !m_config.github()) {
            // "winshun:settings" is Win顺's own: its settings at this plugin's card.
            items.push_back(Item("setup", zh ? "还没有设置云备份" : "Cloud backup is not set up",
                zh ? "Enter 打开设置 → 插件 → 云备份：填 WebDAV 的地址和账号，或 GitHub 私有仓库和令牌；没填之前不联网"
                   : "Enter opens Settings → Plugins → Cloud Backup: a WebDAV address and account, or a private GitHub repository and a token; nothing is sent until then")
                .withIcon("ph:cloud-slash").withUrl("winshun:settings"));
            return items;
        }
        struct Row {
            std::string place;
            std::string label;
            cb::Remote backup;
        };
        std::vector<Row> backups;
        bool reading = false;
        for (const char* key : {"webdav", "github"}) {
            if (!configured(key))
                continue;
            const Place& place = m_places[key];
            const std::string label = std::string(key) == "webdav" ? "WebDAV" : "GitHub";
            const Json data = Json::Object {{"do", "backup"}};
            const std::string again = zh ? "Enter 立即备份" : "Enter backs up now";
            if (m_working && m_busyPlace == key) {
                items.push_back(Item(std::string("status:") + key, label + " · " + m_busyText).withIcon("ph:cloud-arrow-up").invalid());
            } else if (!place.error.empty()) {
                items.push_back(Item(std::string("status:") + key, label + (zh ? " · 没备份成功" : " · the backup failed"),
                    place.error + (zh ? " · Enter 再试一次" : " · Enter tries again")).withIcon("ph:cloud-warning").withData(data));
            } else if (const auto last = cb::parseName(place.name)) {
                items.push_back(Item(std::string("status:") + key, label + (zh ? " · 已备份（" + last->when + "）" : " · backed up (" + last->when + ")"),
                    again + (m_config.automatic ? (zh ? " · 设置变了会自动备份" : " · changed settings are backed up by themselves")
                                                : (zh ? " · 自动备份关着" : " · automatic backup is off")))
                    .withIcon("ph:cloud-check").withData(data));
            } else {
                items.push_back(Item(std::string("status:") + key, label + (zh ? " · 还没有备份过" : " · no backup yet"), again)
                    .withIcon("ph:cloud").withData(data));
            }
            reading = reading || (!place.listed && place.error.empty());
            for (const cb::Remote& backup : place.backups)
                backups.push_back({key, label, backup});
        }
        std::stable_sort(backups.begin(), backups.end(), [](const Row& a, const Row& b) { return a.backup.name > b.backup.name; });
        if (backups.size() > kMaxRows)
            backups.resize(kMaxRows);
        std::map<std::string, int> sameMinute;
        for (const Row& row : backups) {
            if (const auto parsed = cb::parseName(row.backup.name))
                ++sameMinute[row.place + parsed->when];
        }
        for (const Row& row : backups) {
            const auto parsed = cb::parseName(row.backup.name);
            if (!parsed)
                continue;
            std::string title = parsed->when;
            if (sameMinute[row.place + parsed->when] > 1)
                title += ":" + parsed->second;
            if (!parsed->computer.empty())
                title += " · " + parsed->computer;
            std::string subtitle = row.label;
            if (row.backup.size >= 0)
                subtitle += " · " + sizeText(row.backup.size);
            subtitle += zh ? " · Enter 恢复这份备份" : " · Enter restores this backup";
            if (!text.empty() && title.find(text) == std::string::npos && subtitle.find(text) == std::string::npos)
                continue;
            items.push_back(Item("backup:" + row.place + ":" + row.backup.name, title, subtitle)
                .withIcon("ph:clock-counter-clockwise")
                .withData(Json::Object {{"do", "ask"}, {"place", row.place}, {"name", row.backup.name}, {"sha", row.backup.sha}}));
        }
        if (reading && backups.empty())
            items.push_back(Item("reading", zh ? "正在读取云端的备份…" : "Reading the backups in the cloud…").withIcon("ph:cloud").invalid());
        return items;
    }

    // ---- the worker -------------------------------------------------------------------

    void work()
    {
        std::unique_lock lock(m_mutex);
        for (;;) {
            m_cv.wait_until(lock, m_nextAuto, [this] { return m_stop || m_restore || m_wantNow || m_wantList || Clock::now() >= m_nextAuto; });
            if (m_stop)
                return;
            const cb::Settings config = m_config;
            const bool zh = m_zh;
            if (m_restore) {
                const Wanted wanted = *m_restore;
                m_restore.reset();
                m_working = true;
                lock.unlock();
                restore(config, zh, wanted);
            } else if (m_wantNow || Clock::now() >= m_nextAuto) {
                const bool manual = m_wantNow;
                m_wantNow = false;
                m_nextAuto = Clock::now() + (config.automatic ? Clock::duration(kEvery) : Clock::duration(24h));
                if (!manual && !config.automatic)
                    continue;
                m_wantList = false; // a backup lists as well
                m_working = true;
                lock.unlock();
                sync(config, zh, manual);
            } else {
                m_wantList = false;
                m_working = true;
                lock.unlock();
                for (const auto& target : targets(config, zh)) {
                    std::vector<cb::Remote> backups;
                    busy(target->key(), zh ? "正在读取…" : "Reading…");
                    listed(*target, target->list(backups), backups);
                }
            }
            lock.lock();
            m_working = false;
            m_busyPlace.clear();
            lock.unlock();
            refresh(); // the rows changed: the launcher asks again
            lock.lock();
        }
    }

    std::vector<std::unique_ptr<cb::Target>> targets(const cb::Settings& config, bool zh) const
    {
        std::vector<std::unique_ptr<cb::Target>> all;
        if (auto webdav = cb::webdavTarget(config, zh, &m_stop))
            all.push_back(std::move(webdav));
        if (auto github = cb::githubTarget(config, zh, &m_stop))
            all.push_back(std::move(github));
        return all;
    }

    void busy(const std::string& place, const std::string& text)
    {
        {
            const std::lock_guard lock(m_mutex);
            m_busyPlace = place;
            m_busyText = text;
        }
        refresh();
    }

    // A listing's outcome into the place; the place as it is then.
    Place listed(const cb::Target& target, const cb::Outcome& outcome, const std::vector<cb::Remote>& backups)
    {
        const std::lock_guard lock(m_mutex);
        Place& place = m_places[target.key()];
        if (place.id != target.id()) {
            place = {};
            place.id = target.id();
        }
        if (m_stop)
            return place;
        place.error = outcome.error;
        place.triedAt = Clock::now();
        if (outcome.ok()) {
            place.backups = backups;
            place.listed = true;
        }
        return place;
    }

    void failed(const cb::Target& target, const std::string& error)
    {
        const std::lock_guard lock(m_mutex);
        m_places[target.key()].error = error;
    }

    void sync(const cb::Settings& config, bool zh, bool manual)
    {
        const auto all = targets(config, zh);
        if (all.empty())
            return;
        struct Live {
            cb::Target* target;
            bool need; // nothing of ours there (or what was uploaded is gone)
            std::vector<cb::Remote> backups;
        };
        std::vector<Live> live;
        std::vector<std::string> errors;
        std::string common;
        bool same = true;
        for (const auto& target : all) {
            busy(target->key(), zh ? "正在连接…" : "Connecting…");
            std::vector<cb::Remote> backups;
            const cb::Outcome outcome = target->list(backups);
            if (m_stop)
                return;
            const Place place = listed(*target, outcome, backups);
            if (!outcome.ok()) {
                errors.push_back(target->label() + (zh ? "：" : ": ") + outcome.error);
                continue;
            }
            const bool there = std::any_of(backups.begin(), backups.end(), [&](const cb::Remote& b) { return b.name == place.name; });
            const bool need = place.hash.empty() || !there;
            if (need || (!live.empty() && common != place.hash))
                same = false;
            common = place.hash;
            live.push_back({target.get(), need, backups});
        }
        int uploaded = 0;
        if (!live.empty()) {
            // Win顺 packs what there is now; not written when it is what every
            // place has already.
            const Json packed = tool("backup_export",
                Json::Object {{"file", uploadFile()}, {"unless", same ? common : std::string()}}, &m_stop);
            if (m_stop)
                return;
            if (packed.has("error")) {
                const std::string why = (zh ? "Win顺 没能做出备份：" : "Win顺 could not make the backup: ") + packed["error"]["message"].toString();
                for (const Live& l : live)
                    failed(*l.target, why);
                errors.push_back(why);
                live.clear();
            }
            const std::string hash = packed["hash"].toString();
            const std::string bytes = packed["written"].toBool() ? readFile(uploadFile()) : std::string();
            removeFile(uploadFile());
            SYSTEMTIME now {};
            ::GetLocalTime(&now);
            std::string name = cb::backupName(now, computerName());
            // Two in the same second: the next free one, as Win顺's own folder does.
            const auto taken = [&live](const std::string& wanted) {
                for (const Live& l : live) {
                    if (std::any_of(l.backups.begin(), l.backups.end(), [&](const cb::Remote& b) { return b.name == wanted; }))
                        return true;
                }
                return false;
            };
            while (taken(name)) {
                FILETIME ticks {};
                ::SystemTimeToFileTime(&now, &ticks);
                ULARGE_INTEGER t {{ticks.dwLowDateTime, ticks.dwHighDateTime}};
                t.QuadPart += 10'000'000;
                ticks = {t.LowPart, t.HighPart};
                ::FileTimeToSystemTime(&ticks, &now);
                name = cb::backupName(now, computerName());
            }
            for (Live& l : live) {
                {
                    const std::lock_guard lock(m_mutex);
                    if (!l.need && m_places[l.target->key()].hash == hash)
                        continue; // nothing changed since the one there
                }
                if (bytes.empty()) {
                    const std::string why = zh ? "备份文件读不出来" : "The backup file could not be read";
                    failed(*l.target, why);
                    errors.push_back(l.target->label() + (zh ? "：" : ": ") + why);
                    continue;
                }
                busy(l.target->key(), zh ? "正在上传…" : "Uploading…");
                const cb::Outcome outcome = l.target->upload(name, bytes);
                if (m_stop)
                    return;
                if (!outcome.ok()) {
                    failed(*l.target, outcome.error);
                    errors.push_back(l.target->label() + (zh ? "：" : ": ") + outcome.error);
                    continue;
                }
                ++uploaded;
                // Only the newest kept, as in a backup folder on this computer.
                std::vector<cb::Remote> backups;
                if (l.target->list(backups).ok()) {
                    for (std::size_t i = static_cast<std::size_t>(config.keep); i < backups.size(); ++i) {
                        if (backups[i].name != name && !m_stop)
                            l.target->remove(backups[i]);
                    }
                    if (backups.size() > static_cast<std::size_t>(config.keep))
                        backups.resize(static_cast<std::size_t>(config.keep));
                } else {
                    backups = l.backups;
                    backups.insert(backups.begin(), cb::Remote {name, static_cast<long long>(bytes.size()), {}});
                }
                const std::lock_guard lock(m_mutex);
                Place& place = m_places[l.target->key()];
                place.hash = hash;
                place.name = name;
                place.error.clear();
                place.backups = backups;
                place.listed = true;
                place.triedAt = Clock::now();
            }
            const std::lock_guard lock(m_mutex);
            saveState();
        }
        report(zh, manual, uploaded, errors);
    }

    // What a backup came to: asked for, always a line in the launcher; by
    // itself, only a failure, and the same one once (not every half hour).
    void report(bool zh, bool manual, int uploaded, const std::vector<std::string>& errors)
    {
        if (errors.empty()) {
            m_toasted.clear();
            if (manual)
                notifyUser(uploaded > 0 ? (zh ? "已备份到云端" : "Backed up to the cloud") : (zh ? "云端已经是最新的备份" : "The cloud has the newest backup already"));
            return;
        }
        if (manual) {
            notifyUser(errors.front());
            return;
        }
        if (errors.front() == m_toasted)
            return;
        m_toasted = errors.front();
        toast(zh ? "云备份没成功" : "The cloud backup failed", errors.front(), {winshun::effect::setQuery("cloud ")});
    }

    void restore(const cb::Settings& config, bool zh, const Wanted& wanted)
    {
        const auto tell = [&](const std::string& why) {
            notifyUser(why);
            toast(zh ? "备份没能恢复" : "The backup was not restored", why, {winshun::effect::setQuery("cloud ")});
        };
        for (const auto& target : targets(config, zh)) {
            if (target->key() != wanted.place)
                continue;
            busy(target->key(), zh ? "正在下载备份…" : "Downloading the backup…");
            std::string bytes;
            const cb::Outcome outcome = target->download({wanted.name, -1, wanted.sha}, bytes);
            if (m_stop)
                return;
            if (!outcome.ok())
                return tell(outcome.error);
            if (!writeFile(restoreFile(), bytes))
                return tell(zh ? "备份存不到这台电脑上" : "The backup could not be saved on this computer");
            // Win顺 checks it, keeps what there is now and restarts with it.
            const Json answer = tool("backup_restore", Json::Object {{"file", restoreFile()}}, &m_stop);
            removeFile(restoreFile());
            if (answer.has("error") && !m_stop)
                tell(answer["error"]["message"].toString());
            return;
        }
        tell(zh ? "这个备份所在的地方已经不在设置里了" : "The place of this backup is no longer in the settings");
    }

    // ---- what was uploaded, kept between runs (m_mutex held) ---------------------------

    void loadState()
    {
        const auto state = Json::parse(readFile(stateFile()));
        if (!state || !state->isObject())
            return;
        for (const char* key : {"webdav", "github"}) {
            const Json& saved = (*state)[key];
            if (!saved.isObject())
                continue;
            Place& place = m_places[key];
            place.id = saved["id"].toString();
            place.hash = saved["hash"].toString();
            place.name = saved["name"].toString();
        }
    }

    void saveState()
    {
        Json::Object state;
        for (const auto& [key, place] : m_places)
            state[key] = Json::Object {{"id", place.id}, {"hash", place.hash}, {"name", place.name}};
        writeFile(stateFile(), Json(std::move(state)).dump());
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    mutable std::atomic_bool m_stop = false;
    cb::Settings m_config;
    bool m_zh = true;
    std::map<std::string, Place> m_places;
    Clock::time_point m_nextAuto = Clock::now() + kFirst;
    bool m_wantNow = false; // the user asked for a backup
    bool m_wantList = false; // the launcher shows a listing that is old
    std::optional<Wanted> m_restore;
    bool m_working = false;
    std::string m_busyPlace; // what the worker is at, for the rows
    std::string m_busyText;
    std::string m_toasted; // worker only: the failure a notification told of
};

} // namespace

int main()
{
    return CloudBackup().serve();
}
