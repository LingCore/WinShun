// WinShunSemantic.exe: the winshun.semantic plugin that comes with Win顺
// ("按意思找"): find files by what they are about, not by their words.
// "? 去年那份报销流程" finds 《费用管理办法.docx》 whose text says 差旅费用申请.
//
// It never reads a file itself: Win顺's content index already holds the
// text of documents and text files (list_documents, read_file_text; the
// winshun:content permission), so the disks are not read again. The text
// goes, in passages, to a model the user chose (Embedder: an OpenAI-
// compatible /embeddings endpoint, in the cloud or on this computer), and
// the vectors that come back stay here (Store). A query is turned into a
// vector the same way and compared with them all here.
//
// Off until the user turns it on and picks a service; nothing is sent
// before they agreed to what would be (the first item says how many files
// and about how many characters). Indexing runs on a thread of its own;
// with a model on this computer it gives way while the user is at it.

#include "winshun.h"

#include <psapi.h>

#include "Chunker.h"
#include "Embedder.h"
#include "Store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <list>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

using winshun::Item;
using winshun::Items;
using winshun::Json;
using namespace std::chrono_literals;

namespace semantic {

namespace {

constexpr int kToolTimeoutMs = 60000;
constexpr auto kDebounce = 250ms; // a newer query in that time: this one is not asked
constexpr auto kPassEvery = 10min; // looking for changed files
constexpr std::size_t kFlushEvery = 20; // files
constexpr auto kFlushAfter = 15s;
constexpr std::size_t kSample = 24; // files read for the estimate
constexpr int kReadChars = 60000; // of one file: more than its passages take
constexpr int kUserAwaySec = 60; // a local model works flat out after that much quiet
constexpr std::size_t kResults = 30;
// Idle trim (as the app's IdleTrim, which needs Qt): the vectors and what a pass
// listed stay in the working set long after the work; unused for a while and
// above this, the working set goes to Windows' standby list (soft faults bring
// back what the next query needs).
constexpr ULONGLONG kTrimAfterMs = 20'000;
constexpr auto kTrimRecheck = 30s;
constexpr SIZE_T kTrimAbove = 32 * 1024 * 1024; // private working set
// Rows shown however far below the best: the best one only. With 3, "猫打针的记录"
// showed a father's health check (2026-10-13).
constexpr std::size_t kAtLeast = 1;
constexpr float kCloseToBest = 0.85f; // the others: at least this share of the best score

const char* const kNoteExtensions[] = {"txt", "md", "markdown"};
const char* const kDocumentExtensions[] = {"doc", "docm", "docx", "dot", "dotm", "dotx", "dps", "dpt", "et", "ett", "pdf",
    "pot", "potm", "potx", "pps", "ppsm", "ppsx", "ppt", "pptm", "pptx", "wps", "wpt", "xls", "xlsm", "xlsx", "xlt", "xltm", "xltx"};

std::string fileName(const std::string& path)
{
    const std::size_t slash = path.find_last_of('\\');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string folderOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of('\\');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
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

// ---- giving memory back while idle ----------------------------------------------------

std::atomic<int> g_working {0}; // queries, actions and passes under way
std::atomic<ULONGLONG> g_lastUse {0}; // GetTickCount64 when the work last started or ended

struct Working {
    Working()
    {
        ++g_working;
        g_lastUse = ::GetTickCount64();
    }
    ~Working()
    {
        g_lastUse = ::GetTickCount64();
        --g_working;
    }
    Working(const Working&) = delete;
    Working& operator=(const Working&) = delete;
};

SIZE_T privateWorkingSet()
{
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return SIZE_MAX; // unknown: trim
    return counters.PrivateWorkingSetSize;
}

void trimWhenIdle()
{
    g_lastUse = ::GetTickCount64();
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(kTrimRecheck);
            if (g_working.load() > 0 || ::GetTickCount64() - g_lastUse.load() < kTrimAfterMs)
                continue;
            if (privateWorkingSet() > kTrimAbove)
                ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
        }
    }).detach(); // ends with the process
}

// ---- the wire: lines out, and Win顺's tools from any thread -------------------------

class Wire {
public:
    void send(Json message)
    {
        message.set("jsonrpc", "2.0");
        const std::lock_guard lock(m_out);
        winshun::detail::writeLine(message.dump());
    }

    // One of Win顺's tools; an object with "error" when it failed.
    Json tool(const std::string& name, Json arguments)
    {
        const long long id = m_nextId++;
        {
            const std::lock_guard lock(m_mutex);
            if (m_closed)
                return gone();
            m_waiting[id];
        }
        send(Json::Object {{"id", id}, {"method", "winshun/tool"}, {"params", Json::Object {{"name", name}, {"arguments", std::move(arguments)}}}});
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(kToolTimeoutMs), [&] { return m_closed || m_waiting[id].has_value(); });
        std::optional<Json> answer = std::move(m_waiting[id]);
        m_waiting.erase(id);
        if (!answer)
            return gone();
        const Json& m = *answer;
        return m.has("error") ? Json(Json::Object {{"error", m["error"]}}) : m["result"];
    }

    void deliver(long long id, Json message)
    {
        const std::lock_guard lock(m_mutex);
        if (const auto it = m_waiting.find(id); it != m_waiting.end()) {
            it->second = std::move(message);
            m_cv.notify_all();
        }
    }

    void close()
    {
        const std::lock_guard lock(m_mutex);
        m_closed = true;
        m_cv.notify_all();
    }

private:
    static Json gone() { return Json::Object {{"error", Json::Object {{"code", -32000}, {"message", "Win顺 went away"}}}}; }

    std::mutex m_out;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<long long, std::optional<Json>> m_waiting;
    std::atomic<long long> m_nextId {1};
    bool m_closed = false;
};

// What came from Win顺 for the main thread: requests and notifications.
class Inbox {
public:
    void push(Json message)
    {
        const std::lock_guard lock(m_mutex);
        m_messages.push_back(std::move(message));
        m_cv.notify_all();
    }
    void close()
    {
        const std::lock_guard lock(m_mutex);
        m_closed = true;
        m_cv.notify_all();
    }
    std::optional<Json> pop()
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [&] { return m_closed || !m_messages.empty(); });
        if (m_messages.empty())
            return std::nullopt;
        Json m = std::move(m_messages.front());
        m_messages.pop_front();
        return m;
    }
    // Waits up to `time` for another query to come after this one.
    bool newerQuery(std::chrono::milliseconds time)
    {
        std::unique_lock lock(m_mutex);
        return m_cv.wait_for(lock, time, [&] {
            return m_closed || std::any_of(m_messages.begin(), m_messages.end(), [](const Json& m) { return m["method"].toString() == "query"; });
        });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Json> m_messages;
    bool m_closed = false;
};

// ---- the plugin ------------------------------------------------------------------

class Semantic {
public:
    int run()
    {
        std::thread reader([this] { read(); });
        trimWhenIdle();
        while (const auto message = m_inbox.pop()) {
            if (!handle(*message))
                break;
        }
        stopIndexer();
        reader.detach(); // blocked on stdin; the process ends now
        return 0;
    }

private:
    enum class Phase { Starting, NoService, NoContentIndex, Counting, WaitingForYes, Indexing, Ready, Failed };

    struct Status {
        Phase phase = Phase::Starting;
        std::size_t done = 0; // this pass
        std::size_t total = 0;
        std::size_t toSend = 0; // WaitingForYes: files…
        std::size_t chars = 0; // … and their characters, estimated
        std::string error;
        bool contentStillReading = false;
    };

    // ---- the lines -------------------------------------------------------------

    void read()
    {
        winshun::detail::LineReader reader(::GetStdHandle(STD_INPUT_HANDLE));
        std::string line;
        while (reader.next(line)) {
            const auto message = Json::parse(line);
            if (!message || !message->isObject())
                continue;
            if (!message->has("method") && (*message)["id"].isNumber())
                m_wire.deliver((*message)["id"].toInt(), *message);
            else
                m_inbox.push(*message);
        }
        m_wire.close();
        m_inbox.close();
    }

    void respond(const Json& id, Json result) { m_wire.send(Json::Object {{"id", id}, {"result", std::move(result)}}); }

    // False when the loop is to end.
    bool handle(const Json& m)
    {
        const Working working; // a query or action pages the vectors in; not trimmed from under it
        const std::string method = m["method"].toString();
        const Json& id = m["id"];
        const Json& params = m["params"];
        if (method == "initialize") {
            m_locale = params["locale"].toString();
            m_dataDir = params["dataDir"].toString();
            applySettings(params["settings"]);
            respond(id, Json::Object {{"protocolVersion", "1"}, {"name", "winshun.semantic"}});
            m_store = std::make_unique<Store>(folder());
            m_indexer = std::thread([this] {
                indexLoop();
                m_indexerDone = true;
            });
        } else if (method == "query") {
            // Typing goes on: only the last of a quick run is asked.
            if (m_inbox.newerQuery(kDebounce)) {
                respond(id, Json::Object {{"items", Json::Array {}}});
                return true;
            }
            respond(id, Json::Object {{"items", winshun::itemsJson(query(params))}});
        } else if (method == "action") {
            respond(id, Json::Object {{"effects", action(params)}});
        } else if (method == "settingsChanged") {
            applySettings(params["settings"]);
        } else if (method == "shutdown") {
            respond(id, Json::Object {});
            return false;
        } else if (!id.isNull() && method != "cancel") {
            m_wire.send(Json::Object {{"id", id}, {"error", Json::Object {{"code", -32601}, {"message", "Method not found: " + method}}}});
        }
        return true;
    }

    bool chinese() const { return m_locale != "en"; }
    std::string zh(const std::string& z, const std::string& e) const { return chinese() ? z : e; }

    // ---- settings ----------------------------------------------------------------

    void applySettings(const Json& settings)
    {
        const std::lock_guard lock(m_mutex);
        m_provider = providerFrom(settings, chinese());
        m_folders.clear();
        std::string list = settings["folders"].toString();
        std::replace(list.begin(), list.end(), '\n', ';');
        for (std::size_t at = 0; at <= list.size();) {
            std::size_t end = list.find(';', at);
            if (end == std::string::npos)
                end = list.size();
            std::string folder = winshun::trim(expand(list.substr(at, end - at)));
            while (folder.size() > 3 && (folder.back() == '\\' || folder.back() == '/'))
                folder.pop_back();
            if (!folder.empty())
                m_folders.push_back(folder);
            at = end + 1;
        }
        m_notes = settings["types"].toString() == "notes";
        ++m_generation;
        m_wake.notify_all();
    }

    // ---- what the user sees ------------------------------------------------------------

    Status status() const
    {
        const std::lock_guard lock(m_mutex);
        return m_status;
    }

    std::string whereText(const std::string& mark) const
    {
        // "[page 3]" → "第 3 页"
        const std::size_t space = mark.find(' ');
        if (space == std::string::npos || mark.size() < 3)
            return {};
        const std::string kind = mark.substr(1, space - 1);
        const std::string what = mark.substr(space + 1, mark.size() - space - 2);
        if (kind == "page")
            return zh("第 " + what + " 页", "page " + what);
        if (kind == "slide")
            return zh("第 " + what + " 张幻灯片", "slide " + what);
        if (kind == "sheet")
            return zh("工作表 " + what, "sheet " + what);
        return {};
    }

    Items statusItems(bool alone) const
    {
        const Status s = status();
        Provider p;
        {
            const std::lock_guard lock(m_mutex);
            p = m_provider;
        }
        const std::string icon = "glyph:U+E721"; // Search
        Items items;
        switch (s.phase) {
        case Phase::Starting:
        case Phase::Counting:
            items.push_back(Item("status", zh("正在清点要建立索引的文件…", "Counting the files to index…")).withIcon(icon).invalid());
            break;
        case Phase::NoService:
            items.push_back(Item("status", zh("先选一个模型服务", "Pick a model service first"),
                zh("设置 → 插件 → 按意思找：本机的 Ollama，或者填一个云服务的密钥", "Settings → Plugins → Search by meaning: Ollama on this PC, or a cloud service's key"))
                    .withIcon(icon).invalid());
            break;
        case Phase::NoContentIndex:
            items.push_back(Item("status", zh("要先打开文件内容搜索", "Turn on file content search first"),
                zh("按意思找用的是 Win顺 已经读出来的文档文字：设置 → 文件内容搜索", "It uses the text Win顺 already read out of documents: Settings → File content search"))
                    .withIcon(icon).invalid());
            break;
        case Phase::WaitingForYes: {
            if (s.toSend == 0) { // nothing to send: nothing to agree to
                items.push_back(Item("status", zh("没有要建索引的文件", "No files to index"),
                    zh("看看设置里的“只看这些文件夹”和“文件类型”；新放进去的文件，内容搜索读完后会自己加进来",
                        "Check \"Only these folders\" and \"File types\" in the settings; new files join once content search has read them"))
                        .withIcon(icon).invalid());
                break;
            }
            const std::string files = thousands(s.toSend);
            const std::string chars = s.chars >= 10000 ? thousands(s.chars / 10000) + zh(" 万字", "0k characters") : thousands(s.chars) + zh(" 字", " characters");
            std::string subtitle = p.local
                ? zh("约 " + files + " 个文件（约 " + chars + "），用本机的 " + p.name + " 计算，不上传。回车开始",
                      "About " + files + " files (about " + chars + "), computed by " + p.name + " on this PC; nothing is uploaded. Enter starts")
                : zh("约 " + files + " 个文件（约 " + chars + "）的文字会发给 " + p.name + " 计算，按它的价格计费。回车开始",
                      "The text of about " + files + " files (about " + chars + ") goes to " + p.name + ", at its prices. Enter starts");
            items.push_back(Item("start", zh("开始建立语义索引", "Build the semantic index"), subtitle)
                                .withIcon("glyph:U+E768") // Play
                                .withActions({{"start", zh("开始", "Start"), "Enter"}}));
            break;
        }
        case Phase::Indexing:
            items.push_back(Item("status", zh("正在建立语义索引：" + thousands(s.done) + " / " + thousands(s.total) + " 个文件", "Building the semantic index: " + thousands(s.done) + " / " + thousands(s.total) + " files"),
                p.name + " · " + p.model).withIcon(icon).invalid());
            break;
        case Phase::Ready:
            if (alone)
                items.push_back(Item("status", zh("语义索引已就绪：" + thousands(m_store ? m_store->files() : 0) + " 个文件", "The semantic index is ready: " + thousands(m_store ? m_store->files() : 0) + " files"),
                    zh("输入一句话，描述你要找的文件", "Describe the file you are looking for")).withIcon(icon).invalid());
            break;
        case Phase::Failed:
            items.push_back(Item("retry", zh("语义索引停下了：", "The semantic index stopped: ") + s.error, zh("回车重试", "Enter tries again"))
                                .withIcon("glyph:U+E7BA") // Warning
                                .withActions({{"retry", zh("重试", "Try again"), "Enter"}}));
            break;
        }
        return items;
    }

    Items query(const Json& params)
    {
        const std::string text = winshun::trim(params["text"].toString());
        const Status s = status();
        if (text.empty() || s.phase == Phase::NoService || s.phase == Phase::NoContentIndex || s.phase == Phase::WaitingForYes
            || (s.phase != Phase::Ready && (!m_store || m_store->files() == 0)))
            return statusItems(true);
        Provider p;
        {
            const std::lock_guard lock(m_mutex);
            p = m_provider;
        }
        std::vector<float> vector;
        if (const auto it = m_queryCache.find(text); it != m_queryCache.end()) {
            vector = it->second;
        } else {
            const Embedded e = embed(p, {text}, true, chinese());
            if (!e.error.empty() || e.vectors.empty())
                return {Item("error", e.error.empty() ? zh("模型没有回答", "No answer from the model") : e.error).withIcon("glyph:U+E7BA").invalid()};
            vector = e.vectors.front();
            if (m_queryCache.size() > 64)
                m_queryCache.clear();
            m_queryCache.emplace(text, vector);
        }
        const std::size_t limit = std::min<std::size_t>(kResults, static_cast<std::size_t>(std::max<long long>(1, params["limit"].toInt(kResults))));
        Items items;
        int score = 99;
        // Copies of one file (a download saved twice, "(1)") have the same
        // best passage: one row, saying how many more. Their scores differ a
        // little, as each passage was sent with its file's name.
        struct Copies {
            std::size_t row;
            float score;
            int more = 0;
        };
        std::map<std::string, Copies> copies; // where + passage -> its row
        const std::vector<Hit> hits = m_store->search(vector, limit * 2);
        // Files about something else still get a score, well below the
        // best (measured 2026-10-13: the file looked for was never more
        // than 0.05 under the best, unrelated ones 15 % and more).
        float best = 0;
        for (const Hit& hit : hits)
            best = std::max(best, hit.score);
        for (const Hit& hit : hits) {
            if (items.size() >= kAtLeast && hit.score < best * kCloseToBest)
                continue;
            const std::string same = hit.where + '\n' + hit.snippet;
            if (const auto it = copies.find(same); it != copies.end() && it->second.score - hit.score < 0.02f) {
                ++it->second.more;
                continue;
            }
            if (items.size() >= limit)
                break;
            const std::string where = whereText(hit.where);
            std::string subtitle = (where.empty() ? std::string() : where + " · ") + hit.snippet;
            Item item(hit.path, fileName(hit.path), subtitle);
            item.withPath(hit.path).withIcon("file:" + hit.path).withScore(std::max(score--, 1)).withCopy(hit.path);
            item.withPreview(folderOf(hit.path) + "\n\n" + hit.snippet);
            copies.try_emplace(same, Copies {items.size(), hit.score});
            items.push_back(std::move(item));
        }
        for (const auto& [same, c] : copies) {
            if (c.more > 0)
                items[c.row].title += zh("（另有 " + std::to_string(c.more) + " 个副本）", " (" + std::to_string(c.more) + " more copies)");
        }
        if (items.empty())
            items.push_back(Item("none", zh("没有找到意思相近的文件", "No file is about that")).withIcon("glyph:U+E721").invalid());
        if (s.phase != Phase::Ready) {
            Items more = statusItems(false);
            items.insert(items.end(), more.begin(), more.end());
        }
        return items;
    }

    Json action(const Json& params)
    {
        const std::string what = params["action"].toString();
        const std::string item = params["item"]["id"].toString();
        if (item == "start" || what == "start") {
            {
                const std::lock_guard lock(m_mutex);
                std::ofstream(consentPath(), std::ios::binary | std::ios::trunc) << consentFor(m_provider);
                m_status.phase = Phase::Indexing;
                m_status.done = 0;
                m_status.total = m_status.toSend;
                ++m_generation;
            }
            m_wake.notify_all();
            return Json::Array {winshun::effect::notify(zh("开始建立语义索引", "Building the semantic index")), winshun::effect::keep(),
                winshun::effect::replace(statusItems(true))};
        }
        if (item == "retry" || what == "retry") {
            {
                const std::lock_guard lock(m_mutex);
                m_status.phase = Phase::Starting;
                m_status.error.clear();
                ++m_generation;
            }
            m_wake.notify_all();
            return Json::Array {winshun::effect::keep(), winshun::effect::replace(statusItems(true))};
        }
        return Json::Array {winshun::effect::hide()};
    }

    // ---- indexing ------------------------------------------------------------------

    std::wstring folder() const { return winshun::wide(m_dataDir) + L"\\semantic"; }
    std::wstring consentPath() const { return winshun::wide(m_dataDir) + L"\\consent.txt"; }

    // What the user said yes to: this model, and these files. Other folders
    // or types ask again (a try on one folder is no yes for the whole disk).
    // m_mutex held.
    std::string consentFor(const Provider& p) const
    {
        std::string said = p.signature() + "\nnotes=" + (m_notes ? "1" : "0");
        for (const std::string& f : m_folders)
            said += "\nfolder=" + f;
        return said;
    }

    bool consented(const Provider& p) const
    {
        std::ifstream in(consentPath(), std::ios::binary);
        const std::string said((std::istreambuf_iterator<char>(in)), {});
        const std::lock_guard lock(m_mutex);
        return said == consentFor(p);
    }

    void setPhase(Phase phase, std::string error = {})
    {
        const std::lock_guard lock(m_mutex);
        m_status.phase = phase;
        m_status.error = std::move(error);
    }

    // Sleeps until woken (settings, the user's yes) or `time` is up.
    void rest(std::chrono::steady_clock::duration time)
    {
        std::unique_lock lock(m_mutex);
        const std::uint64_t generation = m_generation;
        m_wake.wait_for(lock, time, [&] { return m_stop || m_generation != generation; });
    }

    void stopIndexer()
    {
        {
            const std::lock_guard lock(m_mutex);
            m_stop = true;
            m_wake.notify_all();
        }
        m_wire.close(); // a tool call under way gives up
        // A request to the model may take a while to give up: what was
        // written is on disk, what was not is read again next time.
        for (int i = 0; i < 40 && m_indexer.joinable() && !m_indexerDone; ++i)
            std::this_thread::sleep_for(50ms);
        if (m_indexer.joinable()) {
            if (m_indexerDone)
                m_indexer.join();
            else
                ::ExitProcess(0);
        }
    }

    void indexLoop()
    {
        std::string openedFor;
        int failures = 0;
        while (true) {
            Provider p;
            std::uint64_t generation = 0;
            {
                const std::lock_guard lock(m_mutex);
                if (m_stop)
                    return;
                p = m_provider;
                generation = m_generation;
            }
            if (!p.configured()) {
                setPhase(Phase::NoService);
                rest(24h);
                continue;
            }
            if (openedFor != p.signature()) {
                if (!m_store->open(p.signature())) {
                    setPhase(Phase::Failed, zh("数据文件夹不能用", "The data folder cannot be used"));
                    rest(24h);
                    continue;
                }
                openedFor = p.signature();
            }
            std::string failed;
            {
                const Working working; // listing, reading, embedding: touches pages all the time
                failed = pass(p, generation);
            }
            if (failed.empty()) {
                failures = 0;
                rest(kPassEvery);
            } else if (failed == "-") {
                // Waiting for the user's yes, or for a service or the content
                // index to be set up: until something changes. (A pass the
                // settings cut short starts again at once.)
                if (!changed(generation))
                    rest(kPassEvery);
            } else {
                // Away or busy: again in a while, a longer one each time.
                ++failures;
                setPhase(Phase::Failed, failed);
                rest(std::chrono::minutes(std::min(30, 1 << std::min(failures, 5))));
            }
        }
    }

    bool changed(std::uint64_t generation) const
    {
        const std::lock_guard lock(m_mutex);
        return m_stop || m_generation != generation;
    }

    struct Listed {
        std::string path;
        std::uint32_t stamp;
    };

    // The files to have, from Win顺's content index; nullopt with the phase set when there are none to be had.
    std::optional<std::vector<Listed>> list(std::uint64_t generation)
    {
        Json::Array folders;
        Json::Array extensions;
        {
            const std::lock_guard lock(m_mutex);
            for (const std::string& f : m_folders)
                folders.push_back(f);
            for (const char* e : kDocumentExtensions)
                extensions.push_back(e);
            if (m_notes) {
                for (const char* e : kNoteExtensions)
                    extensions.push_back(e);
            }
        }
        std::vector<Listed> out;
        long long cursor = 0;
        bool reading = false;
        while (!changed(generation)) {
            Json args = Json::Object {{"extensions", Json(extensions)}, {"cursor", cursor}, {"limit", 5000}};
            if (!folders.empty())
                args.set("folders", Json(folders));
            const Json page = m_wire.tool("list_documents", std::move(args));
            if (page.has("error")) {
                const std::string message = page["error"]["message"].toString();
                setPhase(message.find("content index is off") != std::string::npos ? Phase::NoContentIndex : Phase::Failed, message);
                return std::nullopt;
            }
            for (const Json& item : page["items"].toArray())
                out.push_back({item["path"].toString(), static_cast<std::uint32_t>(item["stamp"].toInt())});
            reading = reading || page["reading"].toBool();
            if (!page["next_cursor"].isNumber())
                break;
            cursor = page["next_cursor"].toInt();
        }
        {
            const std::lock_guard lock(m_mutex);
            m_status.contentStillReading = reading;
        }
        return out;
    }

    std::optional<std::string> textOf(const std::string& path)
    {
        const Json r = m_wire.tool("read_file_text", Json::Object {{"path", path}, {"max_chars", kReadChars}});
        if (r.has("error"))
            return std::nullopt;
        return r["text"].toString();
    }

    // How long the user has been away from the keyboard and mouse.
    long long userIdleSeconds()
    {
        const Json r = m_wire.tool("index_status", Json::Object {});
        return r["user_idle_seconds"].toInt(3600);
    }

    // One pass: what changed since the last is read and sent. "" when it
    // went through, "-" when it stopped for a reason already shown (or
    // the settings changed), else what went wrong.
    std::string pass(const Provider& p, std::uint64_t generation)
    {
        {
            const std::lock_guard lock(m_mutex);
            if (m_status.phase == Phase::Starting || m_status.phase == Phase::Failed || m_status.phase == Phase::NoService
                || m_status.phase == Phase::NoContentIndex)
                m_status.phase = Phase::Counting;
        }
        const auto listed = list(generation);
        if (!listed)
            return "-";
        if (changed(generation))
            return "-";
        const auto have = m_store->stamps();
        std::vector<Listed> todo;
        std::set<std::string> present;
        for (const Listed& l : *listed) {
            present.insert(l.path);
            const auto it = have.find(l.path);
            if (it == have.end() || it->second != l.stamp)
                todo.push_back(l);
        }

        // Nothing new to send (fewer folders than agreed to): no need to ask.
        if (listed->empty() || (!todo.empty() && !consented(p))) {
            // What would be sent, roughly: a sample of the files' texts.
            std::size_t chars = 0;
            std::size_t sampled = 0;
            const std::size_t step = std::max<std::size_t>(1, todo.size() / kSample);
            for (std::size_t i = 0; i < todo.size() && sampled < kSample && !changed(generation); i += step) {
                if (const auto text = textOf(todo[i].path)) {
                    chars += std::min(codePoints(*text), kMostPassages * kTarget);
                    ++sampled;
                }
            }
            const std::lock_guard lock(m_mutex);
            if (m_generation != generation)
                return "-";
            m_status.phase = Phase::WaitingForYes;
            m_status.toSend = todo.size();
            m_status.chars = sampled > 0 ? chars * todo.size() / sampled : 0;
            return "-";
        }

        for (const auto& [path, stamp] : have) {
            if (!present.contains(path))
                m_store->remove(path);
        }
        {
            const std::lock_guard lock(m_mutex);
            m_status.done = 0;
            m_status.total = todo.size();
            m_status.phase = todo.empty() ? Phase::Ready : Phase::Indexing;
            m_status.error.clear();
        }
        auto lastFlush = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < todo.size(); ++i) {
            if (changed(generation)) {
                m_store->flush();
                return "-";
            }
            const auto started = std::chrono::steady_clock::now();
            NewFile file;
            file.path = todo[i].path;
            file.stamp = todo[i].stamp;
            if (const auto text = textOf(todo[i].path)) {
                const std::vector<Passage> parts = passages(*text);
                if (!parts.empty()) {
                    // The file's name goes with each passage: it says much about what the file is.
                    const std::string name = fileName(file.path);
                    std::vector<std::string> inputs;
                    for (const Passage& part : parts)
                        inputs.push_back(name + "\n" + part.text);
                    const Embedded e = embed(p, inputs, false, chinese());
                    if (!e.error.empty() && !e.badInput) {
                        m_store->flush();
                        return e.error;
                    }
                    for (std::size_t k = 0; k < e.vectors.size() && k < parts.size(); ++k)
                        file.passages.push_back({parts[k].where, snippet(parts[k].text, 160), e.vectors[k]});
                }
            }
            // Gone, unreadable or refused: kept without passages, so it is not tried again until it changes.
            m_store->add(std::move(file));
            {
                const std::lock_guard lock(m_mutex);
                m_status.done = i + 1;
            }
            const auto now = std::chrono::steady_clock::now();
            if (m_store->pending() >= kFlushEvery || now - lastFlush > kFlushAfter) {
                m_store->flush();
                lastFlush = now;
            }
            // A model on this computer takes the CPU (or the GPU): while the
            // user is at it, it works a third of the time.
            if (p.local && userIdleSeconds() < kUserAwaySec)
                rest(2 * (std::chrono::steady_clock::now() - started));
        }
        m_store->flush();
        m_store->compactIfWorth();
        setPhase(Phase::Ready);
        return {};
    }

    Wire m_wire;
    Inbox m_inbox;
    std::string m_locale = "zh";
    std::string m_dataDir;
    std::unique_ptr<Store> m_store; // made and opened by the indexer; searched by the main thread once it has files
    std::unordered_map<std::string, std::vector<float>> m_queryCache;
    std::thread m_indexer;
    std::atomic<bool> m_indexerDone {false};

    mutable std::mutex m_mutex; // what follows
    std::condition_variable m_wake;
    Provider m_provider;
    std::vector<std::string> m_folders;
    bool m_notes = false;
    std::uint64_t m_generation = 0;
    bool m_stop = false;
    Status m_status;
};

} // namespace

} // namespace semantic

int main()
{
    return semantic::Semantic().run();
}
