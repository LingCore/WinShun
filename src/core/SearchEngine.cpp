#include "SearchEngine.h"

#include "AppCatalog.h"
#include "ContentIndex.h"
#include "ContentScanner.h"
#include "IndexService.h"
#include "NameSearch.h"
#include "Query.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>

using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr int kRecentOnEmptyQuery = 12;
constexpr int kRecentPromoted = 3;
constexpr int kRecentApps = 5; // 应用 with nothing typed: these first, then every app
constexpr qsizetype kAppsInAll = 3; // 全部: the best matching apps, above the files
constexpr int kMinAppScoreInAll = 20; // ... if they match at least this well

bool scopeAccepts(Scope scope, bool isDir) noexcept
{
    switch (scope) {
    case Scope::Apps:
        return false;
    case Scope::Files:
        return !isDir;
    case Scope::Folders:
        return isDir;
    default:
        return true;
    }
}

std::shared_ptr<const AppList> appsOf(const AppCatalog* catalog)
{
    static const auto kNone = std::make_shared<const AppList>();
    return catalog ? catalog->apps() : kNone;
}

const AppInfo* findApp(const AppList& apps, const QString& launchPath)
{
    const QString id = appIdOf(launchPath);
    if (id.isEmpty())
        return nullptr;
    const auto it = std::find_if(apps.begin(), apps.end(), [&](const AppInfo& a) { return a.id == id; });
    return it == apps.end() ? nullptr : &*it;
}

SearchResult fromApp(const AppInfo& app, bool recent)
{
    SearchResult r;
    r.name = app.name;
    r.path = app.launchPath();
    r.app = app.kind;
    r.elevatable = app.elevatable;
    r.target = app.target;
    r.recent = recent;
    return r;
}

// A file row that only repeats an app row: the app's program, or its
// shortcut in the Start menu.
bool duplicatesApp(const SearchResult& file, const SearchResult& app)
{
    if (app.app == AppKind::Desktop && !app.target.isEmpty() && file.path.compare(app.target, Qt::CaseInsensitive) == 0)
        return true;
    return file.name.size() == app.name.size() + 4 && file.name.endsWith(u".lnk", Qt::CaseInsensitive)
        && file.name.startsWith(app.name, Qt::CaseInsensitive)
        && file.path.contains(u"\\Start Menu\\Programs\\", Qt::CaseInsensitive);
}

SearchResult fromIndex(const FileIndex& index, EntryId id)
{
    const Entry& e = index.entry(id);
    SearchResult r;
    r.name = wtf8::toQString(index.name(e));
    r.path = index.path(id);
    r.isDir = e.isDir();
    return r;
}

SearchResult fromDisk(const QString& path, bool isDir)
{
    SearchResult r;
    r.path = QDir::toNativeSeparators(path);
    r.name = r.path.mid(r.path.lastIndexOf(u'\\') + 1);
    if (r.name.isEmpty())
        r.name = r.path;
    r.isDir = isDir;
    r.recent = true;
    return r;
}

// Whether entries lie under one folder, cached by parent folder (files next
// to each other share it).
class UnderFolder {
public:
    UnderFolder(const FileIndex& index, EntryId folder)
        : m_index(index)
        , m_folder(folder)
    {
        m_cache.fill({kNoEntry, false});
    }
    bool operator()(EntryId id)
    {
        const EntryId parent = m_index.entry(id).parent;
        if (m_folder == kNoEntry || parent == kNoEntry)
            return false;
        Slot& slot = m_cache[parent & (m_cache.size() - 1)];
        if (slot.parent != parent) {
            bool under = false;
            for (EntryId cur = parent; cur != kNoEntry && !under; cur = m_index.entry(cur).parent)
                under = cur == m_folder;
            slot = {parent, under};
        }
        return slot.under;
    }

private:
    struct Slot {
        EntryId parent;
        bool under;
    };
    const FileIndex& m_index;
    EntryId m_folder;
    std::array<Slot, 4096> m_cache {};
};

// Recently opened items that match jump to the top (at most a few).
void promoteHistory(SearchResults& results, const SearchEngine::Request& request, const NameMatcher& matcher)
{
    SearchResults promoted;
    for (const QString& path : request.history) {
        if (promoted.size() >= kRecentPromoted)
            break;
        if (isAppLaunchPath(path))
            continue; // apps rank by their own rules (searchApps)
        const auto it = std::find_if(results.begin(), results.end(),
            [&](const SearchResult& r) { return r.path.compare(path, Qt::CaseInsensitive) == 0; });
        if (it != results.end()) {
            SearchResult r = *it;
            r.recent = true;
            results.erase(it);
            promoted.push_back(std::move(r));
            continue;
        }
        if (matcher.matchPath(path, false) < 0 && matcher.matchPath(path, true) < 0)
            continue; // cheap name check before touching the disk
        const QFileInfo info(path);
        if (!info.exists() || !scopeAccepts(request.scope, info.isDir()) || matcher.matchPath(path, info.isDir()) < 0)
            continue;
        promoted.push_back(fromDisk(path, info.isDir()));
    }
    if (promoted.isEmpty())
        return;
    promoted.append(results);
    if (promoted.size() > request.limit)
        promoted.resize(request.limit);
    results = std::move(promoted);
}

} // namespace

SearchEngine::SearchEngine(IndexService* index, AppCatalog* apps, QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_apps(apps)
{
    qRegisterMetaType<ws::SearchResults>();
    m_worker = std::jthread([this](std::stop_token stop) { workerLoop(stop); });
}

SearchEngine::~SearchEngine()
{
    {
        std::lock_guard lock(m_mutex);
        m_pending.reset();
        m_latest.store(~quint64 {0}); // marks any running job stale
    }
    m_worker.request_stop();
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

quint64 SearchEngine::submit(Request request)
{
    std::lock_guard lock(m_mutex);
    const quint64 id = ++m_counter;
    m_latest.store(id);
    m_pending = Job {id, std::move(request)};
    m_cv.notify_one();
    return id;
}

void SearchEngine::cancel()
{
    std::lock_guard lock(m_mutex);
    m_latest.store(++m_counter);
    m_pending.reset();
}

void SearchEngine::workerLoop(std::stop_token stop)
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, stop, [this] { return m_pending.has_value(); }))
                return;
            job = std::move(*m_pending);
            m_pending.reset();
        }
        if (isStale(job.id))
            continue;
        if (job.request.scope == Scope::Content)
            runContentSearch(job);
        else if (job.request.scope == Scope::Apps)
            runAppSearch(job);
        else
            runNameSearch(job);
    }
}

void SearchEngine::runNameSearch(const Job& job)
{
    QElapsedTimer timer;
    timer.start();
    const Request& request = job.request;
    const ParsedQuery query = parseQuery(request.text);
    SearchResults results;
    qint64 total = 0;
    const auto apps = appsOf(m_apps);

    if (query.isEmpty()) {
        for (const QString& path : request.history) {
            if (results.size() >= kRecentOnEmptyQuery || isStale(job.id))
                break;
            if (isAppLaunchPath(path)) {
                const AppInfo* app = request.scope == Scope::All ? findApp(*apps, path) : nullptr;
                if (app)
                    results.push_back(fromApp(*app, true));
                continue;
            }
            const QFileInfo info(path);
            if (info.exists() && scopeAccepts(request.scope, info.isDir()))
                results.push_back(fromDisk(path, info.isDir()));
        }
        total = results.size();
    } else {
        const NameMatcher matcher(query);
        const auto index = m_index->index();
        {
            const auto lock = index->readLock();
            const NameSearchOutput out = searchNames(*index, matcher, request.scope,
                static_cast<std::size_t>(request.limit), m_pool, [&] { return isStale(job.id); });
            if (out.cancelled || isStale(job.id))
                return;
            total = static_cast<qint64>(out.totalMatches);
            results.reserve(static_cast<qsizetype>(out.hits.size()));
            for (const NameHit& hit : out.hits)
                results.push_back(fromIndex(*index, hit.id));
        }
        promoteHistory(results, request, matcher);

        // 全部: the apps that match well come first, like the Start menu's best match.
        if (request.scope == Scope::All) {
            SearchResults top;
            for (const AppHit& hit : searchApps(*apps, query, matcher, request.history)) {
                if (hit.score < kMinAppScoreInAll)
                    break; // best first: the rest match worse
                ++total;
                if (top.size() < kAppsInAll)
                    top.push_back(fromApp((*apps)[hit.index], false));
            }
            if (!top.isEmpty()) {
                total -= results.removeIf([&](const SearchResult& file) {
                    return std::any_of(top.cbegin(), top.cend(), [&](const SearchResult& app) { return duplicatesApp(file, app); });
                });
                top.append(std::move(results));
                if (top.size() > request.limit)
                    top.resize(request.limit);
                results = std::move(top);
            }
        }
    }

    if (!isStale(job.id))
        emit resultsReady(job.id, results, total, timer.nsecsElapsed() / 1000);
}

void SearchEngine::runAppSearch(const Job& job)
{
    QElapsedTimer timer;
    timer.start();
    const Request& request = job.request;
    const ParsedQuery query = parseQuery(request.text);
    const auto apps = appsOf(m_apps);
    SearchResults results;
    qint64 total = 0;

    if (query.isEmpty()) {
        // Like the Start menu's list: recently opened apps, then all of them by name.
        for (const QString& path : request.history) {
            if (results.size() >= kRecentApps)
                break;
            if (const AppInfo* app = findApp(*apps, path))
                results.push_back(fromApp(*app, true));
        }
        const qsizetype recent = results.size();
        for (const AppInfo& app : *apps) {
            if (results.size() >= request.limit)
                break;
            const QString path = app.launchPath();
            if (std::none_of(results.cbegin(), results.cbegin() + recent, [&](const SearchResult& r) { return r.path == path; }))
                results.push_back(fromApp(app, false));
        }
        total = static_cast<qint64>(apps->size());
    } else {
        const NameMatcher matcher(query);
        const std::vector<AppHit> hits = searchApps(*apps, query, matcher, request.history);
        total = static_cast<qint64>(hits.size());
        for (const AppHit& hit : hits) {
            if (results.size() >= request.limit)
                break;
            results.push_back(fromApp((*apps)[hit.index], false));
        }
    }

    if (!isStale(job.id))
        emit resultsReady(job.id, results, total, timer.nsecsElapsed() / 1000);
}

void SearchEngine::runContentSearch(const Job& job)
{
    QElapsedTimer timer;
    timer.start();
    const Request& request = job.request;
    const QString needle = request.text.trimmed();
    emit resultsReady(job.id, {}, 0, 0);
    if (needle.isEmpty()) {
        emit contentProgress(job.id, 0, 0, true);
        return;
    }

    // Candidates: the files of the chosen types, not cloud placeholders. Only
    // their ids are kept (there can be millions: every .js under
    // node_modules); a file's path is looked up when it is read, and the ids
    // are pinned until then. The content index answers for the files it
    // knows without opening them: those it says match are read first (for
    // the line and snippet), then the ones it does not know, your own files
    // before the rest.
    struct Candidate {
        EntryId id;
        std::uint8_t order; // lower first
    };
    const ContentFilter filter {ExtensionFilter(request.contentExtensions), request.contentInLowPriority};
    const auto index = m_index->index();
    const auto content = m_index->contentIndex();
    const ContentIndex::SearchGuard searching(content.get()); // the content indexer waits meanwhile
    std::vector<Candidate> candidates;
    int ruledOut = 0;
    FileIndex::IdPin pin;
    {
        const auto lock = index->readLock();
        pin = index->pinIds();
        const ContentIndex::Lookup lookup = content ? content->lookup(needle) : ContentIndex::Lookup();
        constexpr std::uint32_t kMatch = ContentIndex::Lookup::kMatch;
        UnderFolder inProfile(*index, index->findPath(win32::expandEnvironment(L"%USERPROFILE%")));
        std::size_t k = 0;
        for (std::size_t c = 0; c < index->chunkCount(); ++c) {
            if (isStale(job.id))
                return;
            const auto entries = index->chunk(c);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const Entry& e = entries[i];
                if (!filter.accepts(*index, e))
                    continue;
                const auto id = static_cast<EntryId>((c << FileIndex::kChunkBits) + i);
                bool likely = false;
                if (lookup.usable) {
                    while (k < lookup.known.size() && (lookup.known[k] & ~kMatch) < id)
                        ++k;
                    if (k < lookup.known.size() && (lookup.known[k] & ~kMatch) == id) {
                        if (!(lookup.known[k] & kMatch)) {
                            ++ruledOut;
                            continue;
                        }
                        likely = true;
                    }
                }
                std::uint8_t priority = 1;
                if (e.flags & EntryFlag::LowPriority)
                    priority = 3;
                else if (e.flags & EntryFlag::Hidden)
                    priority = 2;
                else if (inProfile(id))
                    priority = 0;
                candidates.push_back({id, static_cast<std::uint8_t>(likely ? priority : 4 + priority)});
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.order < b.order; });

    const ContentScanner scanner(needle);
    const int total = static_cast<int>(candidates.size()) + ruledOut;
    std::atomic<std::size_t> next {0};
    std::atomic<int> scanned {ruledOut};
    std::atomic<int> found {0};
    std::mutex batchMutex;
    SearchResults batch;
    std::mutex doneMutex;
    std::condition_variable doneCv;
    const int threadCount = static_cast<int>(
        std::clamp<std::size_t>(candidates.size(), 1, static_cast<std::size_t>(std::clamp(request.contentThreads, 1, 64))));
    int running = threadCount;

    const auto stale = [&] { return isStale(job.id); };
    const auto flush = [&](bool finished) {
        SearchResults out;
        {
            std::lock_guard lock(batchMutex);
            out.swap(batch);
        }
        if (stale())
            return;
        if (!out.isEmpty())
            emit contentResults(job.id, out);
        emit contentProgress(job.id, scanned.load(), total, finished);
    };

    {
        std::vector<std::jthread> workers;
        workers.reserve(threadCount);
        for (int t = 0; t < threadCount; ++t) {
            workers.emplace_back([&] {
                ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                std::wstring path;
                while (!stale() && found.load() < request.maxContentResults) {
                    const std::size_t i = next.fetch_add(1);
                    if (i >= candidates.size())
                        break;
                    bool gone = false;
                    {
                        const auto lock = index->readLock();
                        gone = index->entry(candidates[i].id).isDeleted();
                        if (!gone)
                            path = index->wpath(candidates[i].id); // as it is now: it may have moved
                    }
                    if (gone) {
                        scanned.fetch_add(1);
                        continue;
                    }
                    const auto match = scanner.scanFile(path, request.maxContentFileBytes, stale);
                    scanned.fetch_add(1);
                    if (!match)
                        continue;
                    SearchResult r;
                    r.path = QString::fromStdWString(path);
                    r.name = r.path.mid(r.path.lastIndexOf(u'\\') + 1);
                    r.line = match->line;
                    r.snippet = match->snippet;
                    r.snippetMatchStart = match->matchStart;
                    r.snippetMatchLength = match->matchLength;
                    std::lock_guard lock(batchMutex);
                    if (found.load() < request.maxContentResults) {
                        batch.push_back(std::move(r));
                        found.fetch_add(1);
                    }
                }
                {
                    std::lock_guard lock(doneMutex);
                    --running;
                }
                doneCv.notify_all();
            });
        }

        for (;;) {
            std::unique_lock lock(doneMutex);
            if (doneCv.wait_for(lock, 100ms, [&] { return running == 0; }))
                break;
            lock.unlock();
            flush(false);
        }
    }
    flush(true);
    candidates = decltype(candidates)(); // free them before compacting (`= {}` keeps the capacity)
    ::HeapCompact(::GetProcessHeap(), 0); // return the scan's scratch memory to the OS
}

} // namespace ws
