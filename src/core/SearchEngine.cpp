#include "SearchEngine.h"

#include "AppCatalog.h"
#include "ContentIndex.h"
#include "ContentScanner.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "IndexService.h"
#include "NameSearch.h"
#include "Query.h"
#include "SystemCatalog.h"
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
#include <cstring>
#include <optional>

using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr int kRecentOnEmptyQuery = 5; // listed with nothing typed (the history keeps more, for ranking)
constexpr int kRecentPromoted = 3;
// Places in 全部: the best few. Windows lists over a thousand, and words like
// 设置 or 显示 are in hundreds of their names.
constexpr std::size_t kPlacesShown = 5;
constexpr std::size_t kFirstContentBatch = 8; // the most rows the launcher shows (maxRows in Main.qml)

std::shared_ptr<const AppList> appsOf(const AppCatalog* catalog)
{
    static const auto kNone = std::make_shared<const AppList>();
    return catalog ? catalog->apps() : kNone;
}

std::shared_ptr<const PlaceList> placesOf(const SystemCatalog* catalog)
{
    static const auto kNone = std::make_shared<const PlaceList>();
    return catalog ? catalog->places() : kNone;
}

const AppInfo* findApp(const AppList& apps, const QString& launchPath)
{
    const QString id = appIdOf(launchPath);
    if (id.isEmpty())
        return nullptr;
    const auto it = std::find_if(apps.begin(), apps.end(), [&](const AppInfo& a) { return a.id == id; });
    return it == apps.end() ? nullptr : &*it;
}

const PlaceInfo* findPlace(const PlaceList& places, const QString& path)
{
    const QString key = placeKeyOf(path);
    if (key.isEmpty())
        return nullptr;
    const auto it = std::find_if(places.begin(), places.end(), [&](const PlaceInfo& p) { return p.key == key; });
    return it == places.end() ? nullptr : &*it;
}

// Found by one of its other names ("计算器" for Calculator): shown by that.
SearchResult fromApp(const AppInfo& app, bool recent, int otherName = -1)
{
    SearchResult r;
    r.name = otherName >= 0 && otherName < app.otherNames.size() ? app.otherNames[otherName] : app.name;
    r.path = app.launchPath();
    r.app = app.kind;
    r.elevatable = app.elevatable;
    r.target = app.target;
    r.recent = recent;
    return r;
}

SearchResult fromPlace(const PlaceInfo& place, bool recent)
{
    SearchResult r;
    r.name = place.name;
    r.path = place.path();
    r.app = place.kind;
    r.target = place.command;
    r.icon = place.icon;
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
        if (isAppLaunchPath(path) || isPlacePath(path))
            continue; // apps and places rank by their own rules (searchApps, searchPlaces)
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
        if (!info.exists() || matcher.matchPath(path, info.isDir()) < 0)
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

SearchEngine::SearchEngine(IndexService* index, AppCatalog* apps, SystemCatalog* places, QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_documents(std::make_unique<DocExtractor>(4, DocExtractor::Priority::Normal))
    , m_apps(apps)
    , m_places(places)
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
    const auto places = placesOf(m_places);

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
            if (isPlacePath(path)) {
                const PlaceInfo* place = request.scope == Scope::All ? findPlace(*places, path) : nullptr;
                if (place)
                    results.push_back(fromPlace(*place, true));
                continue;
            }
            const QFileInfo info(path);
            if (info.exists())
                results.push_back(fromDisk(path, info.isDir()));
        }
        total = results.size();
    } else {
        const NameMatcher matcher(query);
        const auto index = m_index->index();
        {
            const auto lock = index->readLock();
            const NameSearchOutput out = searchNames(
                *index, matcher, static_cast<std::size_t>(request.limit), m_pool, [&] { return isStale(job.id); });
            if (out.cancelled || isStale(job.id))
                return;
            total = static_cast<qint64>(out.totalMatches);
            results.reserve(static_cast<qsizetype>(out.hits.size()));
            for (const NameHit& hit : out.hits)
                results.push_back(fromIndex(*index, hit.id));
        }
        promoteHistory(results, request, matcher);
        // Files before folders, each in the order they ranked (recent ones first).
        std::stable_partition(results.begin(), results.end(), [](const SearchResult& r) { return !r.isDir; });

        // 全部: every app found and the best few places come first, best
        // first, then the files and folders.
        if (request.scope == Scope::All) {
            struct Ranked {
                int score;
                SearchResult result;
            };
            std::vector<Ranked> ranked;
            for (const AppHit& hit : searchApps(*apps, query, matcher, request.history))
                ranked.push_back({hit.score, fromApp((*apps)[hit.index], false, hit.otherName)});
            const std::size_t appCount = ranked.size();
            for (const PlaceHit& hit : searchPlaces(*places, query, matcher, request.history)) {
                if (ranked.size() - appCount >= kPlacesShown)
                    break;
                const PlaceInfo& place = (*places)[hit.index];
                // An app of that name (控制面板) is the same thing.
                const bool isApp = std::any_of(ranked.cbegin(), ranked.cbegin() + appCount,
                    [&](const Ranked& r) { return r.result.name.compare(place.name, Qt::CaseInsensitive) == 0; });
                if (!isApp)
                    ranked.push_back({hit.score, fromPlace(place, false)});
            }
            // Equal scores: apps first.
            std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
            total += static_cast<qint64>(ranked.size());
            SearchResults top;
            for (Ranked& r : ranked) {
                if (top.size() >= request.limit)
                    break;
                top.push_back(std::move(r.result));
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
    // before the rest. Those are read whole and handed to the content index
    // (ContentIndexer::Intake): the next search knows them.
    struct Candidate {
        EntryId id;
        std::uint8_t order; // lower first
        bool indexed; // the content index knows it
    };
    const ContentFilter filter {ExtensionFilter(request.contentDocuments
                                                    ? request.contentExtensions + documentExtensions()
                                                    : request.contentExtensions),
        request.contentInLowPriority};
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
        const EntryId profile = index->findPath(win32::expandEnvironment(L"%USERPROFILE%"));
        std::vector<std::vector<Candidate>> found(index->chunkCount());
        std::vector<int> ruledOutOf(index->chunkCount(), 0);
        m_pool.parallelFor(index->chunkCount(), [&](std::size_t c) {
            if (isStale(job.id))
                return;
            UnderFolder inProfile(*index, profile);
            const auto first = static_cast<EntryId>(c << FileIndex::kChunkBits);
            auto k = static_cast<std::size_t>(std::lower_bound(lookup.known.begin(), lookup.known.end(), first,
                [](std::uint32_t known, EntryId id) { return (known & ~kMatch) < id; }) - lookup.known.begin());
            const auto entries = index->chunk(c);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const Entry& e = entries[i];
                if (!filter.accepts(*index, e))
                    continue;
                const auto id = static_cast<EntryId>(first + i);
                bool indexed = false;
                bool likely = false;
                while (k < lookup.known.size() && (lookup.known[k] & ~kMatch) < id)
                    ++k;
                if (k < lookup.known.size() && (lookup.known[k] & ~kMatch) == id) {
                    indexed = true;
                    if (lookup.usable) {
                        if (!(lookup.known[k] & kMatch)) {
                            ++ruledOutOf[c];
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
                found[c].push_back({id, static_cast<std::uint8_t>(likely ? priority : 4 + priority), indexed});
            }
        });
        if (isStale(job.id))
            return;
        std::size_t count = 0;
        for (const auto& f : found)
            count += f.size();
        candidates.reserve(count);
        for (std::size_t c = 0; c < found.size(); ++c) {
            candidates.insert(candidates.end(), found[c].begin(), found[c].end());
            ruledOut += ruledOutOf[c];
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.order < b.order; });

    const ContentScanner scanner(needle);
    ContentIndexer::Intake intake(m_index->contentIndexer()); // before the reads, gone once they are done
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
                    const Candidate& candidate = candidates[i];
                    bool gone = false;
                    {
                        const auto lock = index->readLock();
                        gone = index->entry(candidate.id).isDeleted();
                        if (!gone)
                            path = index->wpath(candidate.id); // as it is now: it may have moved
                    }
                    if (gone) {
                        scanned.fetch_add(1);
                        continue;
                    }
                    const std::int64_t maxBytes = request.contentSizeLimits.of(path);
                    std::optional<ContentMatch> match;
                    doctext::Location location;
                    const bool document = isDocumentPath(path);
                    if (document) {
                        // Its text as the index keeps it; else read now (not
                        // indexed yet, changed since, or on a volume not
                        // followed), and kept for next time if it can be.
                        std::optional<doctext::DocText> text
                            = candidate.indexed ? content->textOf(candidate.id) : std::nullopt;
                        if (!text && intake.wants(path)) {
                            const std::uint64_t since = intake.since();
                            ContentIndexer::FileText read
                                = ContentIndexer::readFile(path, maxBytes, stale, m_documents.get());
                            intake.take(candidate.id, read, since);
                            text = std::move(read.document);
                        } else if (!text) {
                            DocExtractor::Result read = m_documents->extractFile(path, maxBytes, stale);
                            if (read.status == extractproto::Status::Ok)
                                text = std::move(read.text);
                        }
                        if (text) {
                            std::size_t at = 0;
                            const std::string& s = text->text;
                            match = scanner.scan(
                                [&](char* buffer, std::size_t capacity) {
                                    const std::size_t n = std::min(capacity, s.size() - at);
                                    std::memcpy(buffer, s.data() + at, n);
                                    at += n;
                                    return n;
                                },
                                ContentScanner::kChunkBytes, stale);
                            if (match)
                                location = doctext::locate(*text, static_cast<std::uint32_t>(match->line));
                        }
                    } else if (!candidate.indexed && intake.wants(path)) {
                        const std::uint64_t since = intake.since();
                        ContentIndexer::Scanned read = ContentIndexer::scanFile(path, maxBytes, scanner, stale);
                        intake.take(candidate.id, read.text, since);
                        match = std::move(read.match);
                    } else {
                        match = scanner.scanFile(path, maxBytes, stale);
                    }
                    scanned.fetch_add(1);
                    if (!match)
                        continue;
                    SearchResult r;
                    r.path = QString::fromStdWString(path);
                    r.name = r.path.mid(r.path.lastIndexOf(u'\\') + 1);
                    r.line = match->line;
                    if (document) {
                        switch (location.kind) {
                        case doctext::PlaceKind::Page:
                            r.where = SearchResult::Where::Page;
                            break;
                        case doctext::PlaceKind::Slide:
                            r.where = SearchResult::Where::Slide;
                            break;
                        case doctext::PlaceKind::Row:
                            r.where = SearchResult::Where::Row;
                            r.sheet = QString::fromUtf8(location.sheet);
                            break;
                        case doctext::PlaceKind::None:
                            r.where = SearchResult::Where::Document;
                            break;
                        }
                        r.placeNumber = static_cast<int>(location.number);
                    }
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

        // The first matches go out once they fill the window or 50 ms have
        // passed (a new query's rows replace the last one's with them: one
        // row alone would flash), the rest every 100 ms.
        const auto start = std::chrono::steady_clock::now();
        bool first = true;
        for (;;) {
            std::unique_lock lock(doneMutex);
            if (doneCv.wait_for(lock, first ? 5ms : 100ms, [&] { return running == 0; }))
                break;
            lock.unlock();
            if (first) {
                std::size_t ready = 0;
                {
                    std::lock_guard batchLock(batchMutex);
                    ready = static_cast<std::size_t>(batch.size());
                }
                if (ready < kFirstContentBatch && std::chrono::steady_clock::now() - start < 50ms)
                    continue;
                first = false;
            }
            flush(false);
        }
    }
    flush(true);
    candidates = decltype(candidates)(); // free them before compacting (`= {}` keeps the capacity)
    ::HeapCompact(::GetProcessHeap(), 0); // return the scan's scratch memory to the OS
}

} // namespace ws
