// WinShunArchive.exe: the winshun.archive plugin in the plugin library
// ("压缩包搜索"): finds files by name inside zip, 7z, rar, tar and other
// archives. Typing a name in Win顺's box also lists the members of archives
// that have it ("backup.zip › 合同\报价单.xlsx"); "zip 报价" lists only those.
//
// Which archives there are comes from Win顺's own index (list_files, the
// winshun:search permission): no disk is walked. Each archive is opened once,
// in the background at the lowest I/O priority, for its list of names only
// (Lister, on libarchive) and again when its size or time changed; the names
// stay in memory and in one file of the data folder (Index). Nothing is
// unpacked until the user asks for one member: its preview, or Ctrl+Enter.
//
// As a tool: WinShunArchive.exe --list <archive> [code page] prints what it
// reads there as JSON (the tests use it); --take <archive> <member> <index>
// <target> [code page] takes one member out.

#include "winshun.h"

#include "IdleTrim.h"
#include "Index.h"
#include "Lister.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

using winshun::Item;
using winshun::Items;
using winshun::Json;
using wsplugin::Working; // the names stay in memory: given back while idle (IdleTrim.h)
using namespace std::chrono_literals;

namespace wsarchive {

namespace {

constexpr auto kPassEvery = 20min; // looking for new and changed archives
constexpr auto kStaleAfter = 2min; // a query this long after the last pass starts one
constexpr auto kRetryIndex = 30s; // Win顺's own index is still being built
constexpr auto kSaveEvery = 30s;
constexpr auto kPublishEvery = 1s; // winshun/status while archives go through
constexpr int kPage = 2000; // of list_files
constexpr std::size_t kMaxNameBytes = std::size_t {128} << 20; // of all the archives together: what stays in memory
constexpr std::size_t kMaxRows = 200;
constexpr std::uint64_t kPreviewBytes = std::uint64_t {32} << 20; // a member shown in the content pane
constexpr std::uint64_t kOpenBytes = std::uint64_t {1} << 30; // a member opened with Ctrl+Enter
constexpr std::uint64_t kKeptBytes = std::uint64_t {256} << 20; // of members taken out for previews
constexpr std::size_t kFolderLines = 400; // of a folder's listing in the content pane

// Archives by their names. A .gz or .xz alone holds one file, not a list:
// only .tar.gz and .tar.xz count.
const char* const kExtensions[] = {"zip", "7z", "rar", "tar", "tgz", "txz", "cab", "iso"};
const char* const kTarOnly[] = {"gz", "xz"};
// What Win顺 does not open for a plugin without the "processes" permission,
// and this plugin does not ask for it: said here, not at the open.
const char* const kPrograms[] = {"exe", "com", "scr", "bat", "cmd", "ps1", "psm1", "vbs", "vbe", "js", "jse", "wsf", "wsh", "msi", "msp",
    "lnk", "url", "hta", "cpl", "reg", "jar", "pif", "appx", "msix", "appinstaller", "application", "gadget", "inf", "scf", "msc", "py",
    "pyw", "sh", "dll", "sys", "chm", "library-ms", "search-ms", "settingcontent-ms"};

std::string fileName(std::string_view path)
{
    const std::size_t slash = path.find_last_of('\\');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string folderOf(std::string_view path)
{
    const std::size_t slash = path.find_last_of('\\');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string extensionOf(std::string_view name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string_view::npos || dot == 0 || dot + 1 == name.size() ? std::string() : folded(name.substr(dot + 1));
}

bool endsWithFolded(std::string_view text, std::string_view end)
{
    return text.size() >= end.size() && folded(text.substr(text.size() - end.size())) == end;
}

std::string expand(const std::string& text)
{
    const std::wstring w = winshun::wide(text);
    wchar_t out[2048];
    const DWORD n = ::ExpandEnvironmentStringsW(w.c_str(), out, 2048);
    return n > 0 && n <= 2048 ? winshun::utf8(out) : text;
}

std::string thousands(std::size_t n)
{
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

std::string sizeText(std::uint64_t bytes)
{
    char buffer[32];
    if (bytes < 1024)
        std::snprintf(buffer, sizeof buffer, "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024)
        std::snprintf(buffer, sizeof buffer, "%.0f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(buffer, sizeof buffer, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(buffer, sizeof buffer, "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buffer;
}

std::string hex(std::uint64_t value)
{
    char buffer[20];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

// FNV-1a: a row's id, a folder's name in the data folder.
std::uint64_t hashOf(std::string_view text)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

template <std::size_t N> bool among(const char* const (&list)[N], std::string_view word)
{
    return std::ranges::any_of(list, [word](const char* item) { return word == item; });
}

// A member's own name as a file name of this computer's: its type kept, the
// rest cut to a length every folder takes.
std::string safeName(std::string_view name)
{
    constexpr std::size_t kMost = 100; // bytes of the name before its type
    std::string_view type;
    if (const std::size_t dot = name.find_last_of('.'); dot != std::string_view::npos && dot > 0 && name.size() - dot <= 17) {
        type = name.substr(dot);
        name = name.substr(0, dot);
    }
    std::size_t cut = std::min(name.size(), kMost);
    while (cut > 0 && cut < name.size() && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80)
        --cut; // not inside a UTF-8 character
    std::string out(name.substr(0, cut));
    out += type;
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || std::strchr("<>:\"/\\|?*", c))
            c = '_';
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    const std::string stem = folded(out.substr(0, out.find('.')));
    for (const char* reserved : {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "lpt1", "lpt2", "lpt3"}) {
        if (stem == reserved)
            return "_" + out;
    }
    return out.empty() ? "file" : out;
}

// ---- the plugin -------------------------------------------------------------------------

struct Config {
    std::vector<std::string> extensions; // asked of Win顺, lower case
    std::vector<std::string> folders; // only under these; empty: everywhere
    bool system = false; // also system and program folders

    friend bool operator==(const Config&, const Config&) = default;
};

Config configOf(const Json& settings)
{
    Config config;
    for (const char* ext : kExtensions)
        config.extensions.emplace_back(ext);
    for (const char* ext : kTarOnly)
        config.extensions.emplace_back(ext);
    std::string more = folded(settings["more"].toString());
    std::ranges::replace_if(more, [](char c) { return c == ',' || c == ';' || c == '\t'; }, ' ');
    for (std::string word : wordsOf(more)) {
        while (!word.empty() && (word.front() == '.' || word.front() == '*'))
            word.erase(0, 1);
        if (!word.empty() && word.size() <= 16 && std::ranges::find(config.extensions, word) == config.extensions.end())
            config.extensions.push_back(std::move(word));
    }
    std::string folders = settings["folders"].toString();
    std::size_t start = 0;
    while (start <= folders.size()) {
        const std::size_t end = std::min(folders.find(';', start), folders.size());
        std::string folder = expand(winshun::trim(folders.substr(start, end - start)));
        std::ranges::replace(folder, '/', '\\');
        while (folder.size() > 3 && folder.back() == '\\')
            folder.pop_back();
        if (!folder.empty())
            config.folders.push_back(std::move(folder));
        start = end + 1;
    }
    config.system = settings["system"].toBool();
    return config;
}

class Archives : public winshun::Plugin {
public:
    ~Archives() override { stopWorker(); }

protected:
    std::string name() const override { return "winshun.archive"; }

    void initialized() override
    {
        m_data = std::filesystem::path(winshun::wide(dataDir()));
        std::error_code ignored;
        // What was taken out for the last session's previews and opens.
        std::filesystem::remove_all(m_data / L"preview", ignored);
        std::filesystem::remove_all(m_data / L"opened", ignored);
        m_index.load(indexFile());
        // The archive being read when the process last ended, if it ended in
        // the middle of one (a crash in the reader, the memory limit): not
        // read again until it changes, or the plugin would end there at
        // every start and be turned off.
        if (const std::optional<Entry> fatal = takeMark()) {
            log("warn", "left out, the process ended while reading it: " + fatal->path);
            m_index.put(*fatal, Contents {});
            m_index.save(indexFile());
        }
        {
            const std::lock_guard lock(m_mutex);
            m_config = configOf(settings());
        }
        wsplugin::trimWhenIdle();
        m_worker = std::thread([this] {
            try {
                run();
            } catch (const std::exception& e) {
                winshun::debug(std::string("winshun.archive: the worker stopped: ") + e.what());
            } catch (...) {
                winshun::debug("winshun.archive: the worker stopped");
            }
        });
    }

    void shuttingDown() override { stopWorker(); }

    void settingsChanged(const Json& settings) override
    {
        Config config = configOf(settings);
        const std::lock_guard lock(m_mutex);
        if (config == m_config)
            return;
        m_config = std::move(config);
        ++m_generation; // the pass under way is for the old settings
        m_wanted = true;
        m_wake.notify_all();
    }

    Items query(const winshun::Query&) override { return {}; } // queryResult answers

    Json queryResult(const winshun::Query& q) override
    {
        const Working working;
        const bool typed = q.trigger != "content"; // the keyword: this plugin's own list, where it may speak
        const std::vector<std::string> words = wordsOf(q.text);
        const Progress progress = snapshot();
        wakeIfStale();
        Items items;
        // Typed among everything else (not after the keyword), a single
        // letter or digit is in nearly every archive: nothing to say to it.
        if (!typed && winshun::trim(q.text).size() < 2)
            return answer(items, false);
        if (words.empty()) {
            if (typed)
                items.push_back(Item("state", stateText(progress), zh("输入文件名的一部分，在压缩包里找", "Type part of a file name to find it inside archives")).invalid().withIcon("ph:file-zip"));
            return answer(items, typed && progress.reading);
        }
        Where where;
        for (const char* key : {"folder", "contentPick"}) {
            if (std::string folder = q.filters[key].toString(); !folder.empty()) {
                while (folder.size() > 3 && folder.back() == '\\')
                    folder.pop_back();
                where.folders.push_back(std::move(folder));
            }
        }
        for (const Json& ext : q.filters["extensions"].toArray()) {
            if (ext.isString())
                where.extensions.push_back(folded(ext.toString()));
        }
        // After the keyword one row is the plugin's own (how it stands, that there are more).
        const std::size_t limit = std::min<std::size_t>(q.limit > 0 ? static_cast<std::size_t>(q.limit) : kMaxRows, kMaxRows) - (typed ? 1 : 0);
        const Found found = m_index.search(words, limit, where);
        items.reserve(found.hits.size() + 1);
        for (const Hit& hit : found.hits)
            items.push_back(row(hit));
        if (typed) {
            if (found.hits.empty())
                items.push_back(Item("none", zh("压缩包里没有找到 ", "Nothing in the archives for ") + q.text, stateText(progress)).invalid().withIcon("ph:file-zip"));
            else if (progress.reading)
                items.push_back(Item("state", stateText(progress), zh("读完之前，结果可能不全", "Until it is done, some may be missing")).invalid().withIcon("ph:file-zip"));
            else if (found.more)
                items.push_back(Item("more", zh("还有更多，再输入几个字缩小范围", "There are more: type more of the name"), {}).invalid().withIcon("ph:file-zip"));
        }
        return answer(items, typed && progress.reading);
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        const Working working;
        if (a.trigger == "status")
            return statusAction(a.action);
        const Json& data = a.item["data"];
        const std::string archivePath = data["archive"].toString();
        const std::string member = data["member"].toString();
        if (archivePath.empty())
            return {winshun::effect::keep()};
        if (::GetFileAttributesW(winshun::wide(archivePath).c_str()) == INVALID_FILE_ATTRIBUTES) {
            wake(); // moved or deleted since it was read: the next pass forgets it
            return {winshun::effect::notify(zh("这个压缩包已经不在原来的位置了", "The archive is no longer where it was")), winshun::effect::keep()};
        }
        if (a.action == "folder")
            return {Json(Json::Object {{"type", "reveal"}, {"path", archivePath}}), winshun::effect::hide()};
        if (a.action != "extract")
            return {winshun::effect::openPath(archivePath), winshun::effect::hide()};

        if (data["folder"].toBool())
            return {winshun::effect::notify(zh("这是压缩包里的文件夹：按 Enter 打开压缩包查看", "A folder inside the archive: Enter opens the archive")), winshun::effect::keep()};
        const std::string own = fileName(member);
        if (among(kPrograms, extensionOf(own)))
            return {winshun::effect::notify(zh("程序和脚本不从这里直接运行：按 Enter 打开压缩包，解压后再用", "Programs and scripts are not run from here: Enter opens the archive")), winshun::effect::keep()};
        std::string problem;
        const auto target = takeOut(L"opened", archivePath, member, static_cast<std::size_t>(data["index"].toInt()), kOpenBytes, 8'000ms, problem);
        if (!target)
            return {winshun::effect::notify(problem), winshun::effect::keep()};
        return {winshun::effect::openPath(winshun::utf8(target->wstring())), winshun::effect::hide()};
    }

    // The content pane and the preview window on one of the rows: the member
    // itself, taken out to the data folder (a copy; the archive is not changed).
    std::optional<Json> details(const Json& item, const std::string&) override
    {
        const Working working;
        const Json& data = item["data"];
        const std::string archivePath = data["archive"].toString();
        const std::string member = data["member"].toString();
        if (archivePath.empty())
            return std::nullopt;
        const std::string head = archivePath + " › " + member + "\n";
        if (data["folder"].toBool())
            return Json(Json::Object {{"text", head + "\n" + folderListing(archivePath, member)}});
        const auto size = static_cast<std::uint64_t>(data["size"].toInt());
        if (data["locked"].toBool())
            return Json(Json::Object {{"text", head + zh("\n这个文件加了密码，看不了内容。按 Enter 打开压缩包。", "\nThis file needs a password. Enter opens the archive.")}});
        if (size > kPreviewBytes)
            return Json(Json::Object {{"text", head + zh("\n文件有 ", "\nThe file is ") + sizeText(size)
                    + zh("，不在这里预览。Ctrl+Enter 解压并打开，Enter 打开压缩包。", ": not previewed here. Ctrl+Enter unpacks and opens it, Enter opens the archive.")}});
        std::string problem;
        const auto target = takeOut(L"preview", archivePath, member, static_cast<std::size_t>(data["index"].toInt()), kPreviewBytes, 4'000ms, problem);
        if (!target)
            return Json(Json::Object {{"text", head + "\n" + problem}});
        return Json(Json::Object {{"file", winshun::utf8(target->wstring())}, {"text", head}});
    }

private:
    struct Progress {
        bool reading = false; // a pass is going through archives
        bool paused = false;
        bool waiting = false; // Win顺's index is not ready yet
        std::size_t done = 0;
        std::size_t total = 0;
        std::string problem; // why the last pass could not run
    };

    std::string zh(const char* cn, const char* en) const { return chinese() ? cn : en; }

    std::wstring indexFile() const { return (m_data / L"archives.bin").wstring(); }

    // Which archive is being read, kept in a file while it is: its size,
    // its time and its path, a line each.
    std::filesystem::path markFile() const { return m_data / L"reading.txt"; }
    void mark(const Entry& entry) const
    {
        std::ofstream out(markFile(), std::ios::binary | std::ios::trunc);
        out << entry.size << '\n' << entry.modified << '\n' << entry.path << '\n';
    }
    void unmark() const
    {
        std::error_code ignored;
        std::filesystem::remove(markFile(), ignored);
    }
    // The archive of a mark left behind, as one that cannot be read.
    std::optional<Entry> takeMark() const
    {
        std::optional<Entry> entry;
        {
            std::ifstream in(markFile(), std::ios::binary);
            Entry read;
            read.outcome = Outcome::Unreadable;
            if (in >> read.size >> read.modified && in.get() == '\n' && std::getline(in, read.path) && !read.path.empty())
                entry = std::move(read);
        }
        unmark();
        return entry;
    }

    Progress snapshot() const
    {
        const std::lock_guard lock(m_mutex);
        return m_progress;
    }

    Json answer(const Items& items, bool again) const
    {
        Json::Object out {{"items", winshun::itemsJson(items)}};
        if (again)
            out.emplace("rerun", 2); // the list fills while archives are read
        return Json(std::move(out));
    }

    Item row(const Hit& hit) const
    {
        const std::string own = fileName(hit.member);
        const std::string inner = folderOf(hit.member);
        // At the archive's top: its path alone (a bare "\" after it read as cut short).
        std::string subtitle = inner.empty() ? hit.archive : hit.archive + " › " + inner;
        if (hit.locked)
            subtitle += zh(" · 有密码", " · password");
        Item item("m" + hex(hashOf(hit.archive + "\n" + hit.member)), own, std::move(subtitle));
        const std::string type = extensionOf(own);
        item.withIcon(hit.folder ? "ph:folder" : type.empty() ? "ph:file" : "ext:" + type)
            .withCopy(hit.archive + "\\" + hit.member)
            .withDetails()
            .withData(Json::Object {{"archive", hit.archive}, {"member", hit.member}, {"index", static_cast<long long>(hit.index)},
                {"size", static_cast<long long>(hit.size)}, {"folder", hit.folder}, {"locked", hit.locked}});
        if (!hit.folder)
            item.withSize(static_cast<long long>(hit.size));
        if (hit.modified > 0)
            item.withModified(static_cast<long long>(hit.modified));
        return item;
    }

    // One line on how the index stands: the card's, and the keyword's first row.
    std::string stateText(const Progress& p) const
    {
        if (p.reading) {
            return (p.paused ? zh("已暂停：", "Paused: ") : zh("正在读压缩包：", "Reading archives: ")) + thousands(p.done) + " / " + thousands(p.total);
        }
        if (!p.problem.empty())
            return p.problem;
        const Totals t = m_index.totals();
        if (t.archives == 0)
            return p.waiting ? zh("等 Win顺 的文件索引建好", "Waiting for Win顺's file index") : zh("还没有找到压缩包", "No archives found yet");
        const std::size_t listed = t.byOutcome[static_cast<std::size_t>(Outcome::Listed)] + t.byOutcome[static_cast<std::size_t>(Outcome::Partial)];
        std::string text = zh("已收录 ", "") + thousands(listed) + zh(" 个压缩包里的 ", " archives, ") + thousands(t.members) + zh(" 个文件", " files inside");
        const auto also = [&](Outcome outcome, const char* cn, const char* en) {
            if (const std::size_t n = t.byOutcome[static_cast<std::size_t>(outcome)])
                text += zh("；", "; ") + thousands(n) + zh(cn, en);
        };
        also(Outcome::Partial, " 个只读到一部分", " read in part");
        also(Outcome::Locked, " 个加了密码看不到文件名", " with names behind a password");
        also(Outcome::Away, " 个在网盘上没下载", " online only");
        also(Outcome::Unreadable, " 个打不开", " could not be opened");
        also(Outcome::NotArchive, " 个不是压缩包或已损坏", " not archives, or damaged");
        return text;
    }

    void publish()
    {
        const Progress p = snapshot();
        using Buttons = std::vector<std::pair<std::string, std::string>>;
        if (p.reading) {
            const double share = p.total ? static_cast<double>(p.done) / static_cast<double>(p.total) : 0.0;
            status(stateText(p), share, p.paused ? "idle" : "busy",
                p.paused ? Buttons {{"resume", zh("继续", "Resume")}} : Buttons {{"pause", zh("暂停", "Pause")}});
        } else {
            const bool trouble = !p.problem.empty();
            status(stateText(p), -1, trouble ? "warn" : p.waiting ? "idle" : "ok", Buttons {{"refresh", zh("立即更新", "Update now")}});
        }
    }

    winshun::Effects statusAction(const std::string& what)
    {
        const bool pausing = what == "pause" || what == "resume";
        {
            const std::lock_guard lock(m_mutex);
            if (pausing)
                m_progress.paused = what == "pause";
            else
                m_wanted = true; // 立即更新: the pass says how it stands when it is through
            m_wake.notify_all();
        }
        if (pausing)
            publish();
        return {};
    }

    void wake()
    {
        const std::lock_guard lock(m_mutex);
        m_wanted = true;
        m_wake.notify_all();
    }

    void wakeIfStale()
    {
        const std::lock_guard lock(m_mutex);
        if (m_progress.reading || std::chrono::steady_clock::now() - m_lastPass < kStaleAfter)
            return;
        m_wanted = true;
        m_wake.notify_all();
    }

    void stopWorker()
    {
        {
            const std::lock_guard lock(m_mutex);
            m_stop = true;
            m_stopFlag = true;
            m_wake.notify_all();
        }
        if (m_worker.joinable())
            m_worker.join();
    }

    // ---- taking one member out ------------------------------------------------------

    std::optional<std::filesystem::path> takeOut(const wchar_t* kind, const std::string& archivePath, const std::string& member,
        std::size_t index, std::uint64_t maxBytes, std::chrono::milliseconds time, std::string& problem)
    {
        std::error_code failed;
        const std::filesystem::path folder = m_data / kind / winshun::wide(hex(hashOf(archivePath + "\n" + member)));
        const std::filesystem::path target = folder / winshun::wide(safeName(fileName(member)));
        // The same archive as when it was taken out before: that copy.
        const auto entry = m_index.find(archivePath);
        const std::string stamp = entry ? std::to_string(entry->size) + "," + std::to_string(entry->modified) : std::string();
        {
            const std::lock_guard lock(m_takenMutex);
            if (const auto it = m_taken.find(target.wstring()); it != m_taken.end() && it->second == stamp && std::filesystem::exists(target, failed))
                return target;
        }
        std::filesystem::create_directories(folder, failed);
        TakeLimits limits;
        limits.maxBytes = maxBytes;
        limits.time = time;
        limits.canceled = &m_stopFlag;
        std::string why;
        const Taken taken = take(winshun::wide(archivePath), member, index, target.wstring(), limits, &why);
        switch (taken) {
        case Taken::Done: {
            const std::lock_guard lock(m_takenMutex);
            m_taken[target.wstring()] = stamp;
            m_kept.push_back(target);
            m_keptBytes += std::filesystem::file_size(target, failed);
            // The oldest go once there are too many (their rows ask again when shown again).
            while (m_keptBytes > kKeptBytes && m_kept.size() > 1) {
                const std::filesystem::path old = m_kept.front();
                m_kept.pop_front();
                const std::uintmax_t size = std::filesystem::file_size(old, failed);
                if (!failed)
                    m_keptBytes -= std::min<std::uint64_t>(m_keptBytes, size);
                std::filesystem::remove(old, failed);
                m_taken.erase(old.wstring());
            }
            return target;
        }
        case Taken::NotFound:
            wake();
            problem = zh("压缩包变了或不在了，稍后再搜一次", "The archive changed or is gone: search again in a moment");
            break;
        case Taken::Locked:
            problem = zh("这个文件加了密码：按 Enter 打开压缩包，输入密码查看", "This file needs a password: Enter opens the archive");
            break;
        case Taken::TooLarge:
            problem = zh("文件超过 ", "The file is over ") + sizeText(maxBytes) + zh("：按 Enter 打开压缩包解压", ": Enter opens the archive");
            break;
        case Taken::TimedOut:
            problem = zh("这个文件在压缩包里解出来要很久：按 Enter 打开压缩包解压", "This file takes long to unpack: Enter opens the archive");
            break;
        case Taken::Failed:
            problem = zh("解不出这个文件（压缩方式不支持或压缩包损坏）：按 Enter 打开压缩包", "This file could not be unpacked: Enter opens the archive");
            break;
        }
        log("info", "take " + std::to_string(static_cast<int>(taken)) + ": " + why);
        return std::nullopt;
    }

    // What is in a folder of an archive, for the content pane.
    std::string folderListing(const std::string& archivePath, const std::string& folder) const
    {
        const auto entry = m_index.find(archivePath);
        if (!entry)
            return {};
        const std::string prefix = folded(folder) + "\\";
        std::string out;
        std::size_t lines = 0;
        std::size_t more = 0;
        std::string_view names = entry->contents->names;
        while (!names.empty()) {
            const std::size_t end = names.find('\n');
            const std::string_view line = names.substr(0, end);
            names.remove_prefix(end == std::string_view::npos ? names.size() : end + 1);
            if (line.size() <= prefix.size() || findFolded(line.substr(0, prefix.size()), prefix) != 0)
                continue;
            if (lines >= kFolderLines) {
                ++more;
                continue;
            }
            out.append(line.substr(prefix.size()));
            out += '\n';
            ++lines;
        }
        if (more)
            out += zh("……还有 ", "... and ") + thousands(more) + zh(" 项", " more") + "\n";
        return out;
    }

    // ---- the worker -------------------------------------------------------------------

    struct Listed {
        std::string path;
        std::uint64_t size = 0;
        std::int64_t modified = 0;
    };

    void run()
    {
        // Lowest I/O and CPU priority: reading archives never gets in the way.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto next = std::chrono::steady_clock::now();
        for (;;) {
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait_until(lock, next, [&] { return m_stop || m_wanted; });
                if (m_stop)
                    return;
                m_wanted = false;
            }
            const Working working;
            const bool ready = pass();
            {
                const std::lock_guard lock(m_mutex);
                m_progress.reading = false;
                m_progress.waiting = !ready && m_progress.problem.empty();
                m_lastPass = std::chrono::steady_clock::now();
                if (m_stop)
                    return;
            }
            if (m_index.changed())
                m_index.save(indexFile());
            publish();
            next = std::chrono::steady_clock::now() + (ready ? std::chrono::duration_cast<std::chrono::seconds>(kPassEvery) : kRetryIndex);
        }
    }

    bool stale(std::uint64_t generation)
    {
        const std::lock_guard lock(m_mutex);
        return m_stop || m_generation != generation;
    }

    // Waits while paused; false when the pass is to end.
    bool goOn(std::uint64_t generation)
    {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [&] { return m_stop || m_generation != generation || !m_progress.paused; });
        return !m_stop && m_generation == generation;
    }

    // One look at every archive Win顺 knows. False when its index was not
    // ready (asked again soon) or the list could not be had.
    bool pass()
    {
        Config config;
        std::uint64_t generation = 0;
        {
            const std::lock_guard lock(m_mutex);
            config = m_config;
            generation = m_generation;
            m_progress.problem.clear();
        }
        // 1. Which archives there are.
        std::vector<Listed> listed;
        bool building = false;
        long long cursor = 0;
        for (;;) {
            Json::Object args {{"limit", kPage}, {"cursor", cursor}, {"include_system", config.system}};
            Json::Array extensions;
            for (const std::string& ext : config.extensions)
                extensions.push_back(ext);
            args.emplace("extensions", Json(std::move(extensions)));
            if (!config.folders.empty()) {
                Json::Array folders;
                for (const std::string& folder : config.folders)
                    folders.push_back(folder);
                args.emplace("folders", Json(std::move(folders)));
            }
            const Json page = tool("list_files", Json(std::move(args)), &m_stopFlag);
            if (stale(generation))
                return true;
            if (page.has("error")) {
                const std::string why = page["error"]["message"].toString();
                log("warn", "list_files: " + why);
                const std::lock_guard lock(m_mutex);
                m_progress.problem = zh("拿不到压缩包的清单：", "The list of archives could not be had: ") + why;
                return false;
            }
            building = building || page["index_state"].isString();
            for (const Json& item : page["items"].toArray()) {
                Listed file {item["path"].toString(), static_cast<std::uint64_t>(item["size"].toInt()), item["modified"].toInt()};
                if (file.path.empty())
                    continue;
                const std::string type = extensionOf(fileName(file.path));
                if (among(kTarOnly, type) && !endsWithFolded(file.path, ".tar." + type))
                    continue;
                listed.push_back(std::move(file));
            }
            if (!page["next_cursor"].isNumber())
                break;
            cursor = page["next_cursor"].toInt();
        }

        // 2. Those new or changed since they were read.
        std::vector<const Listed*> todo;
        std::unordered_set<std::string> present;
        present.reserve(listed.size());
        for (const Listed& file : listed) {
            present.insert(file.path);
            const auto known = m_index.find(file.path);
            if (!known || known->size != file.size || known->modified != file.modified)
                todo.push_back(&file);
        }
        // Gone from Win顺's index: forgotten here too. Not while that index
        // is still filling: a drive not read yet would lose all it had here.
        if (!building)
            m_index.keepOnly(present);
        if (todo.empty())
            return !building;

        {
            const std::lock_guard lock(m_mutex);
            m_progress.reading = true;
            m_progress.done = 0;
            m_progress.total = todo.size();
        }
        publish();
        auto published = std::chrono::steady_clock::now();
        auto saved = published;
        std::size_t room = kMaxNameBytes - std::min(kMaxNameBytes, m_index.totals().nameBytes);
        for (const Listed* file : todo) {
            if (!goOn(generation))
                return true;
            Entry entry;
            entry.path = file->path;
            entry.size = file->size;
            entry.modified = file->modified;
            if (const auto same = m_index.copyOf(file->path, file->size, file->modified)) {
                // A copy of an archive already read (its name, size and time):
                // it holds what that one does, and is not opened for it.
                entry.outcome = Outcome::Listed;
                m_index.put(std::move(entry), same);
            } else {
                Contents contents;
                Limits limits;
                limits.canceled = &m_stopFlag;
                limits.maxNameBytes = std::min(limits.maxNameBytes, room);
                mark(entry);
                const Listing listing = room == 0 ? Listing {Outcome::Partial, {}, "no room"}
                                                  : list(winshun::wide(file->path), limits, [&](Member&& member) { contents.add(member); });
                unmark();
                if (m_stopFlag.load())
                    return true; // cut short by the shutdown: read again next time
                entry.outcome = listing.outcome;
                if (listing.outcome != Outcome::Listed)
                    log("info", std::string(outcomeName(listing.outcome)) + " (" + listing.problem + "): " + file->path);
                room -= std::min(room, m_index.put(std::move(entry), std::move(contents)));
            }
            const auto now = std::chrono::steady_clock::now();
            {
                const std::lock_guard lock(m_mutex);
                ++m_progress.done;
            }
            if (now - published >= kPublishEvery) {
                published = now;
                publish();
            }
            if (now - saved >= kSaveEvery) {
                saved = now;
                m_index.save(indexFile());
            }
        }
        if (room == 0)
            log("warn", "the names kept reached " + sizeText(kMaxNameBytes) + ": the archives read after that have none");
        return !building;
    }

    std::filesystem::path m_data;
    Index m_index;
    std::thread m_worker;
    mutable std::mutex m_mutex; // what follows, to m_lastPass
    std::condition_variable m_wake;
    Config m_config;
    Progress m_progress;
    std::uint64_t m_generation = 0; // of the settings
    bool m_wanted = true; // a pass: at the start, on a button, after a change
    bool m_stop = false;
    std::chrono::steady_clock::time_point m_lastPass = std::chrono::steady_clock::now();
    std::atomic_bool m_stopFlag = false; // m_stop, for what cannot take the lock (Lister, tool calls)
    std::mutex m_takenMutex; // the members taken out
    std::map<std::wstring, std::string> m_taken; // file -> the archive's stamp then
    std::deque<std::filesystem::path> m_kept;
    std::uint64_t m_keptBytes = 0;
};

// ---- as a tool ---------------------------------------------------------------------------

int listTool(const std::wstring& file, unsigned codePage)
{
    Json::Array members;
    Limits limits;
    limits.codePage = codePage;
    const Listing listing = list(file, limits, [&](Member&& m) {
        members.push_back(Json::Object {{"path", m.path}, {"size", static_cast<long long>(m.size)}, {"modified", static_cast<long long>(m.modified)},
            {"folder", m.folder}, {"locked", m.locked}});
    });
    const Json out = Json::Object {{"outcome", outcomeName(listing.outcome)}, {"format", listing.format}, {"problem", listing.problem},
        {"members", Json(std::move(members))}};
    winshun::detail::writeLine(out.dump());
    return 0;
}

int takeTool(const std::wstring& file, const std::wstring& member, const std::wstring& index, const std::wstring& target, unsigned codePage)
{
    static const char* const kNames[] = {"done", "not found", "locked", "too large", "timed out", "failed"};
    std::string problem;
    TakeLimits limits;
    limits.codePage = codePage;
    const Taken taken = take(file, winshun::utf8(member), static_cast<std::size_t>(std::wcstoull(index.c_str(), nullptr, 10)), target, limits, &problem);
    winshun::detail::writeLine(Json(Json::Object {{"taken", kNames[static_cast<std::size_t>(taken)]}, {"problem", problem}}).dump());
    return taken == Taken::Done ? 0 : 1;
}

} // namespace

} // namespace wsarchive

int wmain(int argc, wchar_t** argv)
{
    // The last argument of each, optional: the code page of names that do not say theirs.
    const auto codePage = [&](int at) { return argc > at ? static_cast<unsigned>(std::wcstoul(argv[at], nullptr, 10)) : 0u; };
    if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--list")
        return wsarchive::listTool(argv[2], codePage(3));
    if ((argc == 6 || argc == 7) && std::wstring_view(argv[1]) == L"--take")
        return wsarchive::takeTool(argv[2], argv[3], argv[4], argv[5], codePage(6));
    return wsarchive::Archives().serve();
}
