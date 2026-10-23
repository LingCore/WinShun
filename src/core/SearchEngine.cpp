#include "SearchEngine.h"

#include "AppCatalog.h"
#include "ContentIndex.h"
#include "ContentScanner.h"
#include "Crawler.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "IndexService.h"
#include "NameSearch.h"
#include "PathText.h"
#include "Query.h"
#include "SystemCatalog.h"
#include "SwitchCommand.h"
#include "ShareLister.h"
#include "TextUtil.h"
#include "Unc.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimeZone>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <functional>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>
#include <variant>

using namespace std::chrono_literals;

namespace ws {

// Whether recent items are still on disk, asked on a thread per drive. A drive
// can take long to answer: a disk spinning up, a USB stick in trouble, a
// network share whose server is gone (seconds, or minutes). A search waiting
// for it would leave the last query's rows on screen all that time, so a
// search waits a moment only. A drive still busy then answers in its own
// time; until it does, its items keep the answer from before, or are left
// out. The threads hold only the state they share: a drive that never answers
// holds up nothing else, quitting included.
class RecentOnDisk {
public:
    struct Item {
        bool isDir = false;
        std::uint32_t modified = 0; // seconds since 1970, UTC
        std::int64_t size = -1; // a file's bytes; -1 for a folder
    };

    RecentOnDisk() = default;
    RecentOnDisk(const RecentOnDisk&) = delete;
    RecentOnDisk& operator=(const RecentOnDisk&) = delete;
    ~RecentOnDisk()
    {
        std::lock_guard lock(m_state->mutex);
        m_state->quitting = true;
        m_state->work.notify_all();
    }

    // For each of `paths`, in its place: what is there, or nothing (not there,
    // or no answer in time and none from before). `known`, if given: for each,
    // whether the disk said (so nothing means not there): not when its server
    // did not answer, or it would not say (no access, no disk in the drive).
    std::vector<std::optional<Item>> look(const QStringList& paths, std::vector<bool>* known = nullptr)
    {
        std::vector<std::optional<Item>> found(static_cast<std::size_t>(paths.size()));
        if (known)
            known->assign(found.size(), false);
        if (paths.isEmpty())
            return found;
        State& s = *m_state;
        std::unique_lock lock(s.mutex);
        const auto now = Clock::now();
        std::vector<qsizetype> awaited;
        std::vector<quint64> since(found.size()); // the answer wanted: from a look begun at this one or later
        for (qsizetype i = 0; i < paths.size(); ++i) {
            Drive& drive = driveOf(paths[i]);
            if (drive.busySince && drive.busyPath == paths[i]) {
                since[static_cast<std::size_t>(i)] = drive.busyLook; // being looked at: that answer will do
            } else {
                since[static_cast<std::size_t>(i)] = s.looks + 1;
                if (std::find(drive.queue.cbegin(), drive.queue.cend(), paths[i]) == drive.queue.cend())
                    drive.queue.push_back(paths[i]);
            }
            // Slow today (still on an earlier one after the wait, or the last
            // one took longer): not waited for, until it answers in time again.
            if ((!drive.busySince || now - *drive.busySince < kWait) && drive.lastTook < kWait)
                awaited.push_back(i);
        }
        s.work.notify_all();
        const auto answered = [&](qsizetype i) {
            const auto it = s.answers.find(paths[i]);
            return it != s.answers.end() && it->second.look >= since[static_cast<std::size_t>(i)];
        };
        s.answered.wait_until(lock, now + kWait, [&] { return std::all_of(awaited.cbegin(), awaited.cend(), answered); });
        for (qsizetype i = 0; i < paths.size(); ++i) {
            if (const auto it = s.answers.find(paths[i]); it != s.answers.end()) {
                found[static_cast<std::size_t>(i)] = it->second.item;
                if (known)
                    (*known)[static_cast<std::size_t>(i)] = it->second.item || it->second.gone;
            }
        }
        return found;
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kWait = 100ms; // a drive that is awake answers in well under a millisecond

    struct Answer {
        quint64 look = 0; // State::looks when it was begun
        std::optional<Item> item; // nothing: not there, or could not be told
        bool gone = false; // nothing, and the disk said so (no such file or path)
    };
    struct Drive {
        std::deque<QString> queue;
        std::optional<Clock::time_point> busySince; // looking at busyPath
        QString busyPath;
        quint64 busyLook = 0;
        Clock::duration lastTook {}; // logged when it grows long
    };
    struct State {
        std::mutex mutex;
        std::condition_variable work; // for the drives' threads
        std::condition_variable answered; // for the searches
        std::unordered_map<QString, Drive> drives; // "C:", "\\server\share"; nodes stay put
        std::unordered_map<QString, Answer> answers; // by path
        quint64 looks = 0; // begun so far
        bool quitting = false;
    };

    // Caller holds the lock.
    Drive& driveOf(const QString& path)
    {
        const QString key = driveKey(path);
        const auto [it, added] = m_state->drives.try_emplace(key);
        if (added)
            std::thread(&RecentOnDisk::run, m_state, key).detach();
        return it->second;
    }

    static QString driveKey(const QString& path)
    {
        if (path.size() >= 2 && path[1] == u':')
            return path.left(2).toUpper();
        if (path.startsWith(u"\\\\")) {
            const qsizetype server = path.indexOf(u'\\', 2);
            const qsizetype share = server < 0 ? -1 : path.indexOf(u'\\', server + 1);
            return (share < 0 ? path : path.left(share)).toLower();
        }
        return {};
    }

    // With "\\?\" when longer than Windows takes otherwise.
    static std::wstring longPath(const QString& path)
    {
        if (path.size() < MAX_PATH - 12 || path.startsWith(u"\\\\?\\"))
            return path.toStdWString();
        if (path.startsWith(u"\\\\"))
            return L"\\\\?\\UNC\\" + path.mid(2).toStdWString();
        return L"\\\\?\\" + path.toStdWString();
    }

    static void run(std::shared_ptr<State> state, QString key)
    {
        State& s = *state;
        std::unique_lock lock(s.mutex);
        Drive& drive = s.drives[key];
        for (;;) {
            s.work.wait(lock, [&] { return s.quitting || !drive.queue.empty(); });
            if (s.quitting)
                return;
            const QString path = drive.queue.front();
            drive.queue.pop_front();
            const quint64 look = ++s.looks;
            const auto began = Clock::now();
            drive.busySince = began;
            drive.busyPath = path;
            drive.busyLook = look;
            lock.unlock();

            std::optional<Item> item;
            bool gone = false;
            WIN32_FILE_ATTRIBUTE_DATA data {};
            if (::GetFileAttributesExW(longPath(path).c_str(), GetFileExInfoStandard, &data)) {
                const auto written = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                const auto seconds = static_cast<qint64>(written / 10'000'000) - 11'644'473'600; // since 1601 → since 1970
                const bool isDir = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
                item = Item {isDir,
                    static_cast<std::uint32_t>(std::clamp<qint64>(seconds, 0, std::numeric_limits<std::uint32_t>::max())),
                    isDir ? -1 : static_cast<std::int64_t>((static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow)};
            } else {
                // Not "the server is not there" (ERROR_BAD_NETPATH and the like), "access denied", "no disk".
                const DWORD error = ::GetLastError();
                gone = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
            }
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began);

            lock.lock();
            drive.busySince.reset();
            drive.busyPath.clear();
            s.answers[path] = {look, item, gone};
            s.answered.notify_all();
            // Once while it stays slow: a share whose server is gone takes as long every time.
            if (took > 2s && drive.lastTook <= 2s) {
                qWarning("WinShun: drive %ls took %lld ms to say whether a recent item is still there",
                    qUtf16Printable(key), static_cast<long long>(took.count()));
            }
            drive.lastTook = took;
        }
    }

    std::shared_ptr<State> m_state = std::make_shared<State>();
};

namespace {

constexpr int kRecentOnEmptyQuery = 5; // listed with nothing typed (the history keeps more, for ranking)
// ...or, as asked, the files written last: a list to scroll through
// (Request::limit of them). From one folder and one tree of folders, a few
// more than in a short list, so that a clone or an unpacked archive still
// leaves room for the rest; the same however many are asked, so that a
// longer list starts as the shorter one did.
constexpr std::size_t kModifiedPerFolder = 10;
constexpr std::size_t kModifiedPerTree = 30;
constexpr int kRecentPromoted = 3;
// Places in 全部: the best few. Windows lists over a thousand, and words like
// 设置 or 显示 are in hundreds of their names.
constexpr std::size_t kPlacesShown = 5;
constexpr int kTypedKeywordScore = 1000; // a web keyword's row: above every app, and every plugin row
// Where a path typed from some folder along the way leads ("src/app/"): the
// places looked at, and the best few put first.
constexpr std::size_t kPathsSearched = 200;
constexpr std::size_t kPathsLed = 3;
constexpr std::size_t kFirstContentBatch = 8; // the most rows the launcher shows (maxRows in Main.qml)

// Request::extensions, as ParsedQuery has them: folded, without the dot.
std::vector<std::string> foldedExtensions(const QStringList& extensions)
{
    std::vector<std::string> folded;
    for (const QString& e : extensions) {
        if (!e.isEmpty())
            folded.push_back(text::foldAscii(wtf8::fromUtf16(wtf8::view(e))));
    }
    return folded;
}

// Whether the file at `path` (or so named) has one of `types` (folded): any
// file or folder when there are none, never a folder when there are.
// A file of one of `hidden` (the skipped types) is none.
bool ofType(const QString& path, bool isDir, const std::vector<std::string>& types,
    const std::vector<std::string>& hidden = {})
{
    if (types.empty() && hidden.empty())
        return true;
    const qsizetype dot = path.lastIndexOf(u'.');
    if (isDir || dot < 0 || dot + 1 >= path.size() || dot < path.lastIndexOf(u'\\') || dot < path.lastIndexOf(u'/'))
        return types.empty();
    const std::string ext = wtf8::fromUtf16(wtf8::view(path.sliced(dot + 1)));
    const auto is = [&](const std::string& t) { return text::equalsFolded(ext, t); };
    return (types.empty() || std::any_of(types.begin(), types.end(), is)) && std::none_of(hidden.begin(), hidden.end(), is);
}

// Request::excludedTypes, folded, less those asked for: by the type chosen
// (`types`) or ext: typed. Those are shown all the same.
std::vector<std::string> hiddenTypes(
    const SearchEngine::Request& request, const ParsedQuery& query, const std::vector<std::string>& types)
{
    std::vector<std::string> hidden = foldedExtensions(request.excludedTypes);
    const auto asked = [&](const std::string& h) {
        return std::ranges::find(types, h) != types.end() || std::ranges::find(query.extensions, h) != query.extensions.end();
    };
    std::erase_if(hidden, asked);
    return hidden;
}

// The query with the type chosen: its extensions, or with ext: typed too,
// those in both (none: an extension no name has, "/"); and the types skipped.
ParsedQuery withTypes(ParsedQuery query, const std::vector<std::string>& types, const std::vector<std::string>& hidden)
{
    query.excludedExtensions = hidden;
    if (types.empty())
        return query;
    if (query.extensions.empty()) {
        query.extensions = types;
    } else {
        std::erase_if(query.extensions, [&](const std::string& typed) {
            return std::none_of(types.begin(), types.end(), [&](const std::string& t) { return t == typed; });
        });
        if (query.extensions.empty())
            query.extensions.push_back("/");
    }
    return query;
}

// Whether a favorite is listed in the scope, and with a type chosen, is a file of it.
bool favoriteShown(Scope scope, const Favorite& f, const std::vector<std::string>& types)
{
    return scopeShows(scope, f.kind) && (types.empty() || (f.kind == Favorite::Kind::File && ofType(f.path, false, types)));
}

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
    r.switchId = place.switchId;
    return r;
}

// Its site, or with `words` a search on it.
SearchResult fromWeb(const WebShortcut& web, const QString& words)
{
    SearchResult r;
    r.name = web.name;
    r.path = webPath(web.keyword, words);
    r.app = AppKind::Web;
    r.target = words.isEmpty() ? web.homeUrl() : web.searchUrl(words);
    r.keyword = web.keyword;
    r.words = words;
    r.icon = web.iconRef(); // its site's icon (or its user's), whichever page it opens
    return r;
}

// A file row that only repeats an app row: the app's program, or its
// shortcut in the Start menu.
bool duplicatesApp(const SearchResult& file, const SearchResult& app)
{
    if (app.isWeb())
        return false; // a page on the web, not anything on this computer
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
    if (const FileSize size = e.isDir() ? kUnknownSize : index.size(id); size != kUnknownSize)
        r.size = static_cast<std::int64_t>(unpackSize(size));
    return r;
}

// A recent item that is not among the matches found in the index (or one
// listed with nothing typed).
SearchResult fromDisk(const QString& path, const RecentOnDisk::Item& item)
{
    SearchResult r;
    r.path = QDir::toNativeSeparators(path);
    r.name = r.path.mid(r.path.lastIndexOf(u'\\') + 1);
    if (r.name.isEmpty())
        r.name = r.path;
    r.isDir = item.isDir;
    r.recent = true;
    r.modified = item.modified;
    r.size = item.size;
    return r;
}

// A row as it is, or a path to look up on disk first (RecentOnDisk): the
// recent items are taken in rounds, the paths of a round looked up together.
using RecentItem = std::variant<SearchResult, QString>;

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

// The folders a content search looks in (ContentScope), and of those the
// system, program and tool folders, named all the same (their files are read).
struct ScopeFolders {
    std::vector<EntryId> folders; // sorted
    std::vector<EntryId> aside; // sorted
};

// Whether entries lie under one of some folders (ScopeFolders), and under
// one of the aside ones; cached by parent folder, as UnderFolder.
class UnderFolders {
public:
    enum class Where : std::uint8_t { Outside, Inside, InsideAside };

    UnderFolders(const FileIndex& index, const ScopeFolders& scope)
        : m_index(index)
        , m_scope(scope)
    {
        m_cache.fill({kNoEntry, Where::Outside});
    }
    Where operator()(EntryId id)
    {
        const EntryId parent = m_index.entry(id).parent;
        if (parent == kNoEntry)
            return Where::Outside;
        Slot& slot = m_cache[parent & (m_cache.size() - 1)];
        if (slot.parent != parent) {
            Where where = Where::Outside;
            for (EntryId cur = parent; cur != kNoEntry; cur = m_index.entry(cur).parent) {
                if (std::ranges::binary_search(m_scope.folders, cur)) { // the innermost says
                    where = std::ranges::binary_search(m_scope.aside, cur) ? Where::InsideAside : Where::Inside;
                    break;
                }
            }
            slot = {parent, where};
        }
        return slot.where;
    }

private:
    struct Slot {
        EntryId parent;
        Where where;
    };
    const FileIndex& m_index;
    const ScopeFolders& m_scope;
    std::array<Slot, 4096> m_cache {};
};

// Folders one name may stand for: a name as common as "src" stands for
// thousands on a developer's PC, and all of them are looked in.
constexpr std::size_t kScopeFolders = 100'000;
constexpr std::size_t kScopeApproximate = 200; // ... when none is named just so

// What a folder named among the words, or given, stands for (ContentScope).
// Caller holds index.readLock().
ContentScope::Place resolvePlace(const FileIndex& index, const ContentFolderSpec& spec,
    const SearchEngine::Request& request, WorkerPool& pool, const std::function<bool()>& isCancelled,
    ScopeFolders& scope)
{
    using Status = ContentScope::Status;
    ContentScope::Place place;
    place.typed = spec.typed;
    std::vector<EntryId> ids;
    if (!spec.path.isEmpty()) {
        const EntryId id = index.findPath(spec.path.toStdWString());
        if (id == kNoEntry || index.entry(id).isDeleted() || !index.entry(id).isDir()) {
            place.status = Status::NotFound;
            place.folders = {spec.path};
            return place;
        }
        ids.push_back(id);
    } else {
        std::vector<std::string> names;
        for (const QString& name : spec.names)
            names.push_back(wtf8::fromUtf16(wtf8::view(name)));
        ids = findRelativePath(index, names, true, kScopeFolders, pool, isCancelled);
        if (ids.empty()) {
            // None named just so: those whose names hold it, or sound so
            // (pinyin), in folders whose names hold the rest.
            QueryTerm term;
            term.text = text::foldAscii(names.back());
            for (std::size_t i = 0; i + 1 < names.size(); ++i)
                term.ancestors.push_back(text::foldAscii(names[i]));
            ParsedQuery query;
            query.terms.push_back(std::move(term));
            const NameSearchOutput out = searchNames(index, NameMatcher(query),
                {.limit = kScopeApproximate, .foldersOnly = true}, pool, isCancelled);
            for (const NameHit& hit : out.hits)
                ids.push_back(hit.id);
            place.approximate = !ids.empty();
        }
        if (ids.empty()) {
            place.status = Status::Words;
            return place;
        }
        // Your own folders so named, not those in program folders and
        // node_modules alike, unless only those are (or those are asked for).
        const auto aside = [&](EntryId id) { return (index.entry(id).flags & EntryFlag::LowPriority) != 0; };
        if (!request.contentInLowPriority && !std::ranges::all_of(ids, aside))
            std::erase_if(ids, aside);
    }
    const auto remote = [&](EntryId id) { return (index.entry(id).flags & EntryFlag::Remote) != 0; };
    if (std::ranges::all_of(ids, remote)) {
        place.status = Status::OnShare;
        for (const EntryId id : ids | std::views::take(ContentScope::kListed))
            place.folders.append(QString::fromStdWString(index.wpath(id)));
        place.count = static_cast<int>(ids.size());
        return place;
    }
    std::erase_if(ids, remote);

    // Your own first, then elsewhere, hidden ones, system and program
    // folders; the shallowest first in each.
    const EntryId profile = index.findPath(win32::expandEnvironment(L"%USERPROFILE%"));
    UnderFolder inProfile(index, profile);
    const auto rank = [&](EntryId id) {
        const Entry& e = index.entry(id);
        const int kind = (e.flags & EntryFlag::LowPriority) ? 3
            : (e.flags & EntryFlag::Hidden)                  ? 2
            : (id == profile || inProfile(id))               ? 0
                                                             : 1;
        return std::tuple {kind, index.depth(id), id};
    };
    std::vector<std::tuple<int, int, EntryId>> ranked;
    ranked.reserve(ids.size());
    for (const EntryId id : ids)
        ranked.push_back(rank(id));
    std::ranges::sort(ranked);
    place.count = static_cast<int>(ranked.size());
    for (const auto& [kind, depth, id] : ranked | std::views::take(ContentScope::kListed))
        place.folders.append(QString::fromStdWString(index.wpath(id)));

    // One of them picked (Tab along them): that one alone.
    if (!request.contentPick.folder.isEmpty() && request.contentPick.typed == spec.typed) {
        const EntryId picked = index.findPath(request.contentPick.folder.toStdWString());
        if (std::ranges::find(ids, picked) != ids.end()) {
            ids = {picked};
            place.chosen = QString::fromStdWString(index.wpath(picked));
        }
    }
    for (const EntryId id : ids) {
        scope.folders.push_back(id);
        if (index.entry(id).flags & EntryFlag::LowPriority)
            scope.aside.push_back(id);
    }
    return place;
}

// Where a content search looks (ContentScope), and what for. The folders
// typed go before those given. Caller holds index.readLock().
ContentScope resolveScope(const FileIndex& index, const SearchEngine::Request& request, WorkerPool& pool,
    const std::function<bool()>& isCancelled, ScopeFolders& scope)
{
    ContentScope out;
    const ContentQuery typed = splitContentQuery(request.text);
    out.words = typed.words;
    for (const ContentFolderSpec& spec : typed.folders) {
        ContentScope::Place place = resolvePlace(index, spec, request, pool, isCancelled, scope);
        if (place.status == ContentScope::Status::Words) // "abc/" is what to find, then
            out.words = out.words.isEmpty() ? spec.typed : out.words + u' ' + spec.typed;
        out.places.push_back(std::move(place));
    }
    const bool typedSome = std::ranges::any_of(
        out.places, [](const ContentScope::Place& p) { return p.status != ContentScope::Status::Words; });
    if (!typedSome) {
        for (const QString& folder : request.contentFolders) {
            ContentFolderSpec given;
            given.path = pathtext::pathFromText(folder);
            if (given.path.isEmpty())
                given.path = folder;
            out.places.push_back(resolvePlace(index, given, request, pool, isCancelled, scope));
        }
    }
    std::ranges::sort(scope.folders);
    scope.folders.erase(std::ranges::unique(scope.folders).begin(), scope.folders.end());
    std::ranges::sort(scope.aside);
    scope.aside.erase(std::ranges::unique(scope.aside).begin(), scope.aside.end());
    return out;
}

// Recently opened items that match jump to the top (at most a few).
// Returns how many did.
qsizetype promoteHistory(
    SearchResults& results, const SearchEngine::Request& request, const NameMatcher& matcher, RecentOnDisk& onDisk)
{
    const bool foldersOnly = request.scope == Scope::Folders;
    // A path typed: only what lies in that folder. The folders typed match
    // names anywhere along an item's way, here as in the index (UnderFolder
    // keeps the index's to the folder): "D:\x\F1\" would bring up what was
    // opened in D:\x\F11 (#49: "search here" on F1 showed F11's files).
    QString within = pathtext::splitTyped(request.text).folder;
    if (!within.isEmpty() && !within.endsWith(u'\\'))
        within += u'\\'; // "D:\" has one
    SearchResults promoted;
    qsizetype next = 0;
    while (promoted.size() < kRecentPromoted && next < request.history.size()) {
        std::vector<RecentItem> round;
        QStringList paths;
        while (next < request.history.size() && promoted.size() + std::ssize(round) < kRecentPromoted) {
            const QString& path = request.history[next++];
            if (isAppLaunchPath(path) || isPlacePath(path))
                continue; // apps and places rank by their own rules (searchApps, searchPlaces)
            if (!within.isEmpty() && !path.startsWith(within, Qt::CaseInsensitive))
                continue;
            const auto it = std::find_if(results.begin(), results.end(),
                [&](const SearchResult& r) { return r.path.compare(path, Qt::CaseInsensitive) == 0; });
            if (it != results.end()) {
                SearchResult r = *it;
                r.recent = true;
                results.erase(it);
                round.emplace_back(std::move(r));
                continue;
            }
            if (matcher.matchPath(path, false) < 0 && matcher.matchPath(path, true) < 0)
                continue; // cheap name check before asking the disk
            round.emplace_back(path);
            paths.push_back(path);
        }
        const auto found = onDisk.look(paths);
        std::size_t k = 0;
        for (RecentItem& item : round) {
            if (auto* r = std::get_if<SearchResult>(&item)) {
                promoted.push_back(std::move(*r));
                continue;
            }
            const QString& path = std::get<QString>(item);
            const std::optional<RecentOnDisk::Item>& there = found[k++];
            if (there && (!foldersOnly || there->isDir) && matcher.matchPath(path, there->isDir) >= 0)
                promoted.push_back(fromDisk(path, *there));
        }
    }
    const qsizetype count = promoted.size();
    if (count > 0) {
        promoted.append(std::move(results));
        results = std::move(promoted);
    }
    return count;
}

// Ranked by time, the recent ones stay where they are, marked.
void markRecent(SearchResults& results, const QStringList& history)
{
    for (SearchResult& r : results) {
        r.recent = std::any_of(history.cbegin(), history.cend(),
            [&](const QString& path) { return r.path.compare(path, Qt::CaseInsensitive) == 0; });
    }
}

// Those of `rows` named just what was typed first, in their order; among
// them the files of the types listed first (Request::preferredTypes), so
// that "curl" lists curl.exe before the folder curl and curl.h, whichever
// of files and folders goes first after them (KindOrder). Returns where the
// others start.
SearchResults::iterator exactFirst(SearchResults::iterator begin, SearchResults::iterator end, const NameMatcher& matcher,
    const PreferredTypes& preferred)
{
    const auto exactEnd = std::stable_partition(begin, end,
        [&](const SearchResult& r) { return matcher.isExactName(wtf8::fromUtf16(wtf8::view(r.name)), r.isDir); });
    if (!preferred.empty()) {
        std::stable_partition(begin, exactEnd, [&](const SearchResult& r) {
            return !r.isDir && !r.isApp() && preferred.hasName(wtf8::fromUtf16(wtf8::view(r.name)));
        });
    }
    return exactEnd;
}

// The files and folders found, in their order: by match, a few opened
// lately first (promoteHistory), then those named just what was typed
// ("readme" finds README.md and the folder readme; programs and scripts
// first: exactFirst), then the files and the folders apart, as asked, each
// best first. By time, the two kinds apart, newest first.
void arrange(SearchResults& results, const SearchEngine::Request& request, const NameMatcher& matcher, RankBy rankBy,
    RecentOnDisk& onDisk)
{
    // The launcher sorted by name, folder, type or size (#44) is the same list
    // as by time: nothing put first, the kinds apart as asked. The panel's
    // column order is one list, as it came.
    const bool ordered = request.order.key != SortKey::Rank;
    if (ordered && request.browse) {
        markRecent(results, request.history);
        if (results.size() > request.limit)
            results.resize(request.limit);
        return;
    }
    // Recent items first, as rows of their own (they need not be in the
    // index): not into a list narrowed by when, how large or where, which
    // the history knows nothing of (a recent file would show through the filter).
    const bool filtered = request.filter.any() || !request.folders.isEmpty() || parseQuery(request.text).filter.any();
    const qsizetype promoted
        = rankBy == RankBy::Match && !ordered && !filtered ? promoteHistory(results, request, matcher, onDisk) : 0;
    auto rest = results.begin() + promoted; // after promoteHistory: it puts `results` together anew
    if (rankBy == RankBy::Match && !ordered) {
        const auto exactBegin = rest;
        rest = exactFirst(rest, results.end(), matcher, PreferredTypes(foldedExtensions(request.preferredTypes)));
        for (auto it = exactBegin; it != rest; ++it)
            it->exact = true; // a plugin's rows keep below them (plugin::Merge)
    } else {
        markRecent(results, request.history);
    }
    const bool foldersFirst = request.kindOrder == KindOrder::FoldersFirst;
    std::stable_partition(rest, results.end(), [&](const SearchResult& r) { return r.isDir == foldersFirst; });
    if (results.size() > request.limit)
        results.resize(request.limit);
}

// Whether the list is in an order the user chose: by time, or the launcher's
// sorts (#44). Else it is ranked, best match first.
bool sortedByChoice(const SearchEngine::Request& request)
{
    return request.rankBy != RankBy::Match || request.order.key != SortKey::Rank;
}

// SearchResults in the order the engine puts index hits in (RankBy, SortOrder),
// for rows that do not come from the index: a typed folder's contents, listed
// from disk (#44). The kinds apart as asked (arrange) unless the list is one
// (the panel, `browse`). A tie compares equal: a stable sort keeps what came.
class ResultOrder {
public:
    explicit ResultOrder(const SearchEngine::Request& request)
        : m_key(request.order.key == SortKey::Rank ? SortKey::Modified : request.order.key)
        , m_descending(request.order.key == SortKey::Rank ? request.rankBy == RankBy::Newest : request.order.descending)
        , m_apart(!request.browse)
        , m_foldersFirst(request.kindOrder == KindOrder::FoldersFirst)
    {
    }

    bool operator()(const SearchResult& a, const SearchResult& b) const
    {
        if (m_apart && a.isDir != b.isDir)
            return a.isDir == m_foldersFirst;
        switch (m_key) {
        case SortKey::Modified: // a time not known last either way
            if (a.modified != b.modified && (a.modified == 0 || b.modified == 0))
                return b.modified == 0;
            return a.modified != b.modified && (m_descending ? a.modified > b.modified : a.modified < b.modified);
        case SortKey::Size: // a size not known, and every folder, last either way
            if (a.size != b.size && (a.size < 0 || b.size < 0))
                return b.size < 0;
            return a.size != b.size && (m_descending ? a.size > b.size : a.size < b.size);
        case SortKey::Name:
            return directed(compareNames(a.name, b.name));
        case SortKey::Path: {
            const int c = compareNames(folderOf(a), folderOf(b));
            return directed(c != 0 ? c : compareNames(a.name, b.name));
        }
        case SortKey::Type: {
            const int c = compareNames(extensionOf(a), extensionOf(b));
            return directed(c != 0 ? c : compareNames(a.name, b.name));
        }
        case SortKey::Rank:
            break;
        }
        return false;
    }

private:
    bool directed(int c) const { return m_descending ? c > 0 : c < 0; }

    // As the index compares names: bytes of the WTF-8, ASCII case aside.
    static int compareNames(const QString& a, const QString& b)
    {
        return compareNames(wtf8::fromUtf16(wtf8::view(a)), wtf8::fromUtf16(wtf8::view(b)));
    }
    static int compareNames(const std::string& a, const std::string& b)
    {
        const std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned char x = text::fold(a[i]);
            const unsigned char y = text::fold(b[i]);
            if (x != y)
                return x < y ? -1 : 1;
        }
        return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
    }
    static QString folderOf(const SearchResult& r) { return r.path.left(std::max<qsizetype>(0, r.path.size() - r.name.size())); }
    static QString extensionOf(const SearchResult& r)
    {
        if (r.isDir)
            return {};
        const std::string name = wtf8::fromUtf16(wtf8::view(r.name));
        return QString::fromUtf8(name.substr(name.size() - extensionLength(name, false)));
    }

    SortKey m_key;
    bool m_descending;
    bool m_apart;
    bool m_foldersFirst;
};

constexpr qsizetype kFavoritesPromoted = 5; // of those that match, the first put first

void markFavorite(SearchResult& r, const Favorite& f)
{
    r.favorite = true;
    r.favoriteKeyword = f.keyword;
}

// Favorites' rows as things are now, in their order: their app or place, or
// what is at their path (asked of the disk as recent items are). One known
// to be gone is there all the same, `missing`; one that cannot be told yet
// (the apps or places not read yet) is left out. On a share that cannot be
// reached now: `offline`, the disk not asked.
SearchResults favoriteRows(const QList<const Favorite*>& favorites, const AppList& apps, const PlaceList& places,
    RecentOnDisk& onDisk, const IndexService* index, QStringView typed = {})
{
    std::vector<std::optional<SearchResult>> rows(static_cast<std::size_t>(favorites.size()));
    QStringList paths;
    std::vector<std::size_t> asked;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Favorite& f = *favorites[static_cast<qsizetype>(i)];
        SearchResult r;
        if (f.kind == Favorite::Kind::Command) {
            r.path = f.path;
            r.app = AppKind::Tool;
            r.target = f.command;
            r.workingDirectory = f.workingDirectory;
            r.words = commandWords(f, typed);
        } else if (f.kind == Favorite::Kind::App) {
            if (const AppInfo* app = findApp(apps, f.path))
                r = fromApp(*app, false);
            else if (apps.empty())
                continue;
            else
                r = {.path = f.path, .missing = true, .app = AppKind::Desktop};
        } else if (f.kind == Favorite::Kind::Place) {
            if (const PlaceInfo* place = findPlace(places, f.path))
                r = fromPlace(*place, false);
            else if (places.empty())
                continue;
            else
                r = {.path = f.path, .missing = true, .app = AppKind::Tool};
        } else {
            r.path = f.path;
            r.isDir = f.kind == Favorite::Kind::Folder;
            if (index && r.path.startsWith(u"\\\\") && index->shares().offline(r.path.toStdWString())) {
                r.offline = true;
            } else {
                paths.push_back(f.path);
                asked.push_back(i);
            }
        }
        r.name = f.name;
        markFavorite(r, f);
        rows[i] = std::move(r);
    }
    std::vector<bool> known;
    const auto found = onDisk.look(paths, &known);
    for (std::size_t k = 0; k < asked.size(); ++k) {
        SearchResult& r = *rows[asked[k]];
        if (found[k]) {
            r.isDir = found[k]->isDir;
            r.modified = found[k]->modified;
        } else if (known[k]) {
            r.missing = true;
        }
    }
    SearchResults out;
    for (std::optional<SearchResult>& r : rows) {
        if (r)
            out.push_back(std::move(*r));
    }
    return out;
}

// Favorites among the matches: marked; one whose keyword is all that was
// typed on top (below a web shortcut's typed keyword); with `promote`, the
// first few that match (in their order) put first, those the index does not
// hold (a folder left out of it) looked for on disk; with `types` (a type
// chosen), only files of it. Adds to `total` what it adds.
void promoteFavorites(SearchResults& results, qint64& total, const SearchEngine::Request& request,
    const NameMatcher& matcher, bool promote, const AppList& apps, const PlaceList& places, RecentOnDisk& onDisk,
    const IndexService* index, const std::vector<std::string>& types)
{
    const FavoriteList& favorites = request.favorites;
    if (favorites.isEmpty())
        return;
    const auto nameMatches = [&](const QString& name) { return matcher.matchName(wtf8::fromUtf16(wtf8::view(name))) >= 0; };
    for (SearchResult& r : results) {
        const qsizetype i = r.isWeb() || r.isPlugin() ? -1 : indexOfFavorite(favorites, r.path);
        if (i < 0)
            continue;
        markFavorite(r, favorites[i]);
        if (nameMatches(favorites[i].name))
            r.name = favorites[i].name; // its user's name for it, when that is what was typed
    }

    QList<const Favorite*> wanted;
    const qsizetype command = typedCommandKeyword(favorites, request.text);
    const qsizetype keyword = command >= 0 ? command : typedFavoriteKeyword(favorites, request.text);
    if (keyword >= 0 && favoriteShown(request.scope, favorites[keyword], types))
        wanted.push_back(&favorites[keyword]);
    const bool keyed = !wanted.isEmpty();
    for (qsizetype i = 0; promote && i < favorites.size() && wanted.size() < kFavoritesPromoted + (keyed ? 1 : 0); ++i) {
        const Favorite& f = favorites[i];
        if (i == keyword || !favoriteShown(request.scope, f, types))
            continue;
        const bool found = std::any_of(results.cbegin(), results.cend(),
            [&](const SearchResult& r) { return r.favorite && r.path.compare(f.path, Qt::CaseInsensitive) == 0; });
        if (found || nameMatches(f.name)
            || (f.isFileOrFolder() && matcher.matchPath(f.path, f.kind == Favorite::Kind::Folder) >= 0)) {
            wanted.push_back(&f);
        }
    }
    if (wanted.isEmpty())
        return;

    // Those found taken out as they are, the others asked of the disk.
    SearchResults rows;
    QList<const Favorite*> unfound;
    std::vector<qsizetype> unfoundAt;
    for (const Favorite* f : std::as_const(wanted)) {
        const auto it = std::find_if(results.begin(), results.end(),
            [&](const SearchResult& r) { return r.favorite && r.path.compare(f->path, Qt::CaseInsensitive) == 0; });
        if (it != results.end()) {
            rows.push_back(std::move(*it));
            results.erase(it);
            --total;
        } else {
            unfound.push_back(f);
            unfoundAt.push_back(rows.size());
            rows.push_back({});
        }
    }
    const SearchResults looked = favoriteRows(unfound, apps, places, onDisk, index, request.text);
    SearchResults put;
    for (qsizetype i = 0, k = 0; i < rows.size(); ++i) {
        if (k < std::ssize(unfoundAt) && unfoundAt[static_cast<std::size_t>(k)] == i) {
            const QString& path = unfound[k++]->path;
            const auto it = std::find_if(looked.cbegin(), looked.cend(), [&](const SearchResult& r) { return r.path == path; });
            if (it != looked.cend())
                put.push_back(*it);
            continue;
        }
        put.push_back(std::move(rows[i]));
    }
    if (keyed && !put.isEmpty() && put.first().path.compare(favorites[keyword].path, Qt::CaseInsensitive) == 0) {
        put.first().score = kTypedKeywordScore; // as a web keyword's row: nothing a plugin pushes goes above it
        put.first().exact = true;
    }
    total += put.size();
    // Below a web shortcut's keyword typed (the same word cannot be both; one typed with words can).
    const qsizetype at = !results.isEmpty() && results.first().isWeb() && results.first().score == kTypedKeywordScore ? 1 : 0;
    for (qsizetype i = 0; i < put.size(); ++i)
        results.insert(at + i, std::move(put[i]));
    if (results.size() > request.limit)
        results.resize(request.limit);
}

} // namespace

bool rankedBefore(const SearchResult& a, const SearchResult& b, RankBy by) noexcept
{
    if (by != RankBy::Match) {
        // Dated more than a day ahead: ordered as not known (sortTime).
        const FileTime until = trustedUntil();
        const FileTime ta = sortTime(a.modified, until);
        const FileTime tb = sortTime(b.modified, until);
        if (ta != tb) {
            if (ta == 0 || tb == 0)
                return tb == 0;
            return by == RankBy::Newest ? ta > tb : ta < tb;
        }
    }
    return a.relevance > b.relevance;
}

SearchEngine::SearchEngine(IndexService* index, AppCatalog* apps, SystemCatalog* places, QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_documents(std::make_unique<DocExtractor>(4, DocExtractor::Priority::Normal))
    , m_apps(apps)
    , m_places(places)
    , m_onDisk(std::make_unique<RecentOnDisk>())
{
    qRegisterMetaType<ws::SearchResults>();
    qRegisterMetaType<ws::ContentLeftOut>();
    qRegisterMetaType<ws::ContentScope>();
    qRegisterMetaType<ws::RemovableDrives>();
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
    m_pending = Job {id, std::move(request), std::chrono::steady_clock::now()};
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
        else if (!job.request.walkFolder.isEmpty())
            runDriveWalk(job);
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

    // For the log, when it takes long.
    using Clock = std::chrono::steady_clock;
    const auto began = Clock::now();
    Clock::duration lockWait {};
    Clock::duration names {};

    const bool foldersOnly = request.scope == Scope::Folders;
    // 文件 with a type chosen: only its files, whatever is typed or listed.
    const std::vector<std::string> types = foldersOnly ? std::vector<std::string>() : foldedExtensions(request.extensions);
    const std::vector<std::string> hidden = hiddenTypes(request, query, types); // skipped, and not asked for
    // Narrowed by when, how large or where (the panel's filters, size: and
    // dm: typed): files and folders alone, in the index's own terms. Nothing
    // typed in the panel lists them all the same (browse).
    const bool filtered = request.filter.any() || !request.folders.isEmpty() || query.filter.any();
    const bool listing = filtered || (request.browse && query.isEmpty());
    if (query.isEmpty() && request.startList == StartList::Modified && !listing) {
        // The files written last (in 文件夹, the folders they are in), newest first.
        // A day ahead at most: later is a clock gone wrong, not new work.
        const NewestOptions options {.limit = static_cast<std::size_t>(request.limit),
            .folders = foldersOnly,
            .perFolder = kModifiedPerFolder,
            .perTree = kModifiedPerTree,
            .notAfter = trustedUntil(),
            .extensions = types,
            .excludedExtensions = hidden};
        const auto index = m_index->index();
        const auto lock = index->readLock();
        lockWait = Clock::now() - began;
        const NewestOutput out = newestFiles(*index, options, m_pool, [&] { return isStale(job.id); });
        if (out.cancelled || isStale(job.id))
            return;
        for (const NameHit& hit : out.hits) {
            SearchResult r = fromIndex(*index, hit.id);
            r.modified = hit.modified;
            results.push_back(std::move(r));
        }
        markRecent(results, request.history);
        total = results.size();
    } else if (query.isEmpty() && !listing) {
        qsizetype next = 0;
        while (results.size() < kRecentOnEmptyQuery && next < request.history.size() && !isStale(job.id)) {
            std::vector<RecentItem> round;
            QStringList paths;
            while (next < request.history.size() && results.size() + std::ssize(round) < kRecentOnEmptyQuery) {
                const QString& path = request.history[next++];
                if (indexOfFavorite(request.favorites, path) >= 0)
                    continue; // listed above them
                if (isAppLaunchPath(path)) {
                    const AppInfo* app = request.scope == Scope::All ? findApp(*apps, path) : nullptr;
                    if (app)
                        round.emplace_back(fromApp(*app, true));
                    continue;
                }
                if (isPlacePath(path)) {
                    const PlaceInfo* place = request.scope == Scope::All ? findPlace(*places, path) : nullptr;
                    // A switch is never listed here: Enter on the first row as
                    // the window opens would flip it back (pinnedPlaces).
                    if (place && place->switchId.isEmpty())
                        round.emplace_back(fromPlace(*place, true));
                    continue;
                }
                if (!ofType(path, false, types, hidden))
                    continue; // asked of the disk only when it may be listed
                round.emplace_back(path);
                paths.push_back(path);
            }
            const auto found = m_onDisk->look(paths);
            std::size_t k = 0;
            for (RecentItem& item : round) {
                if (auto* r = std::get_if<SearchResult>(&item)) {
                    results.push_back(std::move(*r));
                    continue;
                }
                const std::optional<RecentOnDisk::Item>& there = found[k++];
                if (there && (!foldersOnly || there->isDir) && (types.empty() || !there->isDir))
                    results.push_back(fromDisk(std::get<QString>(item), *there));
            }
        }
        total = results.size();
    } else {
        const NameMatcher matcher(withTypes(query, types, hidden));
        const PreferredTypes preferred(foldedExtensions(request.preferredTypes)); // searchNames: by match only
        const auto index = m_index->index();
        const RankBy rankBy = request.rankBy;
        // A path typed or pasted ("C:\Users\me\Tencent Files\x"): what that
        // folder holds and is so named, from the disk as Explorer lists it,
        // then what lies deeper in it and is so named, from the index.
        const pathtext::TypedPath typedPath = pathtext::splitTyped(request.text);
        EntryFilter filter = request.filter;
        filter.merge(query.filter);
        {
            const auto lock = index->readLock();
            lockWait = Clock::now() - began;
            // The folders to look under, as the index has them (one not in
            // it finds nothing: a folder of its own that is not there).
            for (const QString& folder : request.folders) {
                const EntryId id = index->findPath(folder.toStdWString());
                filter.folders.push_back(id == kNoEntry ? kNoEntry - 1 : id);
            }
            std::ranges::sort(filter.folders);
            const NameSearchOptions options {.limit = static_cast<std::size_t>(request.limit),
                .foldersOnly = foldersOnly,
                .rankBy = rankBy,
                .kindsApart = !request.browse || request.order.key == SortKey::Rank,
                .filter = filter,
                .order = request.order,
                .trustedUntil = trustedUntil(),
                .preferred = preferred};
            const NameSearchOutput out
                = searchNames(*index, matcher, options, m_pool, [&] { return isStale(job.id); });
            names = Clock::now() - began - lockWait;
            if (out.cancelled || isStale(job.id))
                return;
            total = static_cast<qint64>(out.totalMatches);
            results.reserve(static_cast<qsizetype>(out.hits.size()));
            // The folders of a typed path match names anywhere along the way:
            // only what really lies under the folder typed. One the index does
            // not have holds nothing (#49: "F1\" found the files of "F11"
            // and of every folder with "f1" in its name); the disk listing
            // below has what it holds.
            const EntryId folder = typedPath.folder.isEmpty() ? kNoEntry : index->findPath(typedPath.folder.toStdWString());
            UnderFolder under(*index, folder);
            for (const NameHit& hit : out.hits) {
                if (!typedPath.folder.isEmpty() && !under(hit.id)) {
                    --total;
                    continue;
                }
                SearchResult r = fromIndex(*index, hit.id);
                r.modified = knownTime(index->modified(hit.id)); // its own, though ordered as unknown when ahead
                results.push_back(std::move(r));
            }
        }
        arrange(results, request, matcher, rankBy, *m_onDisk);

        // A network folder is listed only when its server answers: one that
        // is not there would hold the search up for the half minute Windows
        // waits. Known to be away (a share searched), or not answering now.
        const bool listable = !unc::isUnc(typedPath.folder.toStdWString())
            || (!m_index->shares().offline(typedPath.folder.toStdWString())
                && share::reachable(unc::host(typedPath.folder.toStdWString())));
        if (!typedPath.folder.isEmpty() && listable && !filtered) {
            // The ones named just so first; with nothing more typed than the
            // folder, the folder itself (listTyped puts it first).
            // In an order the user chose (time, or the launcher's sorts #44) the
            // folder's contents are sorted with the other matches, so every one
            // is listed, not the first `limit` of listTyped's own order.
            const bool byChoice = sortedByChoice(request);
            SearchResults rows
                = pathtext::listTyped(typedPath, foldersOnly, byChoice ? std::numeric_limits<int>::max() : static_cast<int>(request.limit));
            rows.removeIf([&](const SearchResult& r) { return !ofType(r.path, r.isDir, types, hidden); });
            if (!byChoice)
                exactFirst(rows.begin(), rows.end(), matcher, PreferredTypes(foldedExtensions(request.preferredTypes)));
            total -= results.removeIf([&](const SearchResult& deeper) {
                return std::any_of(rows.cbegin(), rows.cend(),
                    [&](const SearchResult& r) { return r.path.compare(deeper.path, Qt::CaseInsensitive) == 0; });
            });
            total += rows.size();
            if (byChoice) {
                // The folder itself (nothing typed after it) stays on top as
                // the current place; the rest, rows and matches, in the order.
                const bool self = typedPath.name.trimmed().isEmpty() && !rows.isEmpty()
                    && rows.front().path.compare(typedPath.folder, Qt::CaseInsensitive) == 0;
                SearchResults merged = std::move(results); // the index's first: a tie keeps them ahead
                for (auto it = rows.begin() + (self ? 1 : 0); it != rows.end(); ++it)
                    merged.push_back(std::move(*it));
                std::stable_sort(merged.begin(), merged.end(), ResultOrder(request));
                if (self)
                    merged.prepend(std::move(rows.front()));
                results = std::move(merged);
                if (results.size() > request.limit)
                    results.resize(request.limit);
            } else {
                rows.append(std::move(results));
                if (rows.size() > request.limit)
                    rows.resize(request.limit);
                results = std::move(rows);
            }
        }
        // On a share that cannot be reached now: shown all the same, dimmed.
        for (SearchResult& r : results) {
            if (r.path.startsWith(u"\\\\"))
                r.offline = m_index->shares().offline(r.path.toStdWString());
        }

        // 全部: a web shortcut's keyword typed first ("gh", "gh WinShun")
        // puts its site or search on top. Then every app found, the best few
        // places and the web shortcuts by name, best first; then the files
        // and folders.
        if (request.scope == Scope::All && typedPath.folder.isEmpty() && !listing) {
            struct Ranked {
                int score;
                SearchResult result;
                bool guessedSwitch = false; // a switch found by a guess (PlaceHit::weak)
            };
            std::vector<Ranked> ranked;
            for (const AppHit& hit : searchApps(*apps, query, matcher, request.history)) {
                ranked.push_back({hit.score, fromApp((*apps)[hit.index], false, hit.otherName)});
                ranked.back().result.exact = hit.exact; // by name, another name or an abbreviation ("ps")
            }
            const std::size_t appCount = ranked.size();
            std::size_t shown = 0; // places other than switches (those are few, and each one a concept)
            for (const PlaceHit& hit : searchPlaces(*places, query, matcher, request.history)) {
                const PlaceInfo& place = (*places)[hit.index];
                const bool isSwitch = !place.switchId.isEmpty();
                if (!isSwitch && shown >= kPlacesShown)
                    continue;
                // An app of that name (控制面板) is the same thing.
                const bool isApp = std::any_of(ranked.cbegin(), ranked.cbegin() + appCount,
                    [&](const Ranked& r) { return r.result.name.compare(place.name, Qt::CaseInsensitive) == 0; });
                if (isApp)
                    continue;
                ranked.push_back({hit.score, fromPlace(place, false), isSwitch && hit.weak});
                shown += isSwitch ? 0 : 1;
            }
            // A wrong Enter on an app opens it; on a switch it changes the
            // system. So a switch found by a guess ("ly") goes behind every
            // app and page found as well, and first only where there are none.
            int behind = std::numeric_limits<int>::max();
            for (const Ranked& r : ranked) {
                if (r.result.switchId.isEmpty())
                    behind = std::min(behind, r.score);
            }
            for (Ranked& r : ranked) {
                if (r.guessedSwitch)
                    r.score = std::min(r.score, behind - 1);
            }
            // "关蓝牙", "音量 30", "蓝牙设置": the switch with what was asked of
            // it, above everything but a typed keyword; the plain row of the
            // same switch and the page that opens the same place give way.
            // A state word alone ("深色") only tells the plain row what Enter
            // then does.
            std::optional<SearchResult> commandRow;
            if (const std::optional<SwitchCommand> command = parseSwitchCommand(request.text, *places)) {
                const PlaceInfo& target = (*places)[command->place];
                if (command->onTop) {
                    commandRow = fromPlace(target, false);
                    commandRow->intent = command->intent;
                    commandRow->score = kTypedKeywordScore;
                    std::erase_if(ranked, [&](const Ranked& r) {
                        return r.result.switchId == target.switchId
                            || (r.result.switchId.isEmpty() && r.result.isPlace()
                                && r.result.target.compare(target.command, Qt::CaseInsensitive) == 0);
                    });
                } else {
                    for (Ranked& r : ranked) {
                        if (r.result.switchId == target.switchId)
                            r.result.intent = command->intent;
                    }
                }
            }
            const std::optional<TypedKeyword> typed = typedKeyword(request.web, request.text);
            for (const WebHit& hit : searchWebShortcuts(request.web, matcher)) {
                if (!typed || hit.index != typed->index) // else on top already
                    ranked.push_back({hit.score, fromWeb(request.web[hit.index], {})});
            }
            // Equal scores: apps first.
            std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
            SearchResults top;
            if (typed) {
                top.push_back(fromWeb(request.web[typed->index], typed->words));
                top.back().score = kTypedKeywordScore; // nothing a plugin pushes goes above it
                top.back().exact = true;
            }
            if (commandRow)
                top.push_back(std::move(*commandRow));
            total += static_cast<qint64>(ranked.size()) + top.size();
            for (Ranked& r : ranked) {
                if (top.size() >= request.limit)
                    break;
                r.result.score = r.score;
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

        // A path typed from some folder along the way ("tools/out/covers/",
        // "src\app\App.cpp"): where it leads first, so Enter goes there, not
        // to the best of what lies in it or is named like it. Several such
        // places: the one opened last, then your own, then the shallowest.
        if (const pathtext::RelativePath typed = typedPath.folder.isEmpty() && !filtered
                ? pathtext::splitRelative(request.text)
                : pathtext::RelativePath();
            !typed.names.isEmpty()) {
            std::vector<std::string> steps;
            for (const QString& name : typed.names)
                steps.push_back(wtf8::fromUtf16(wtf8::view(name)));
            struct Led {
                SearchResult row;
                qsizetype opened; // in the history; its size when not there
                bool aside; // under system, program or tool folders, or hidden
                int depth;
            };
            std::vector<Led> led;
            {
                const auto lock = index->readLock();
                const std::vector<EntryId> ids = findRelativePath(*index, steps, foldersOnly || typed.folder,
                    kPathsSearched, m_pool, [&] { return isStale(job.id); });
                for (const EntryId id : ids) {
                    SearchResult row = fromIndex(*index, id);
                    if (!ofType(row.path, row.isDir, types, hidden))
                        continue;
                    row.modified = index->modified(id);
                    const qsizetype opened = request.history.indexOf(row.path, 0, Qt::CaseInsensitive);
                    const bool aside = index->entry(id).flags & (EntryFlag::LowPriority | EntryFlag::Hidden);
                    led.push_back({std::move(row), opened < 0 ? request.history.size() : opened, aside, index->depth(id)});
                }
            }
            if (isStale(job.id))
                return;
            const auto keep = std::min(led.size(), kPathsLed);
            const FileTime until = trustedUntil();
            std::ranges::partial_sort(led, led.begin() + static_cast<std::ptrdiff_t>(keep), [until](const Led& a, const Led& b) {
                if (a.opened != b.opened)
                    return a.opened < b.opened;
                if (a.aside != b.aside)
                    return !a.aside;
                if (a.depth != b.depth)
                    return a.depth < b.depth;
                return sortTime(a.row.modified, until) > sortTime(b.row.modified, until);
            });
            led.resize(keep);
            if (!led.empty()) {
                total -= results.removeIf([&](const SearchResult& r) {
                    return std::ranges::any_of(
                        led, [&](const Led& l) { return l.row.path.compare(r.path, Qt::CaseInsensitive) == 0; });
                });
                SearchResults rows;
                for (Led& l : led)
                    rows.push_back(std::move(l.row));
                total += rows.size();
                rows.append(std::move(results));
                if (rows.size() > request.limit)
                    rows.resize(request.limit);
                results = std::move(rows);
            }
        }

        // Favorites first, but not before what a typed path leads to, nor
        // among rows ranked by time (marked there only).
        const bool promote = rankBy == RankBy::Match && typedPath.folder.isEmpty() && !listing
            && pathtext::splitRelative(request.text).names.isEmpty();
        promoteFavorites(results, total, request, matcher, promote, *apps, *places, *m_onDisk, m_index, types);

        // Nothing at all is named as typed (no file or folder, no app, place
        // or favorite either): the query read aloud, for the names that sound
        // the same (年度报告 for 年度报高), listed under a heading that says
        // so (approximate). Only then: a query that finds anything costs
        // nothing more. Each reading (行: xing, hang) is a search of its own;
        // an entry found by several keeps its best score.
        if (results.isEmpty() && typedPath.folder.isEmpty() && pathtext::splitRelative(request.text).names.isEmpty()
            && !isStale(job.id)) {
            std::vector<ParsedQuery> readings = homophoneQueries(query);
            for (ParsedQuery& reading : readings)
                reading.filter = query.filter; // the bounds stay as typed
            std::unordered_map<EntryId, NameHit> best;
            for (const ParsedQuery& reading : readings) {
                const NameMatcher spoken(withTypes(reading, types, hidden));
                const auto lock = index->readLock();
                const NameSearchOptions options {.limit = static_cast<std::size_t>(request.limit),
                    .foldersOnly = foldersOnly,
                    .rankBy = rankBy,
                    .kindsApart = !request.browse || request.order.key == SortKey::Rank,
                    .filter = filter,
                    .order = request.order,
                    .trustedUntil = trustedUntil(),
                    .preferred = preferred};
                const NameSearchOutput out = searchNames(*index, spoken, options, m_pool, [&] { return isStale(job.id); });
                if (out.cancelled || isStale(job.id))
                    return;
                for (const NameHit& hit : out.hits) {
                    const auto [it, added] = best.try_emplace(hit.id, hit);
                    if (!added && hit.score > it->second.score)
                        it->second = hit;
                }
            }
            if (!best.empty()) {
                // In a name search's order: by match, a tie to the one written
                // last; by time as asked, one not known last.
                std::vector<NameHit> hits;
                hits.reserve(best.size());
                for (const auto& [id, hit] : best)
                    hits.push_back(hit);
                std::ranges::sort(hits, [rankBy](const NameHit& a, const NameHit& b) {
                    if (rankBy != RankBy::Match && a.modified != b.modified) {
                        if (a.modified == 0 || b.modified == 0)
                            return b.modified == 0;
                        return rankBy == RankBy::Newest ? a.modified > b.modified : a.modified < b.modified;
                    }
                    if (a.score != b.score)
                        return a.score > b.score;
                    if (a.modified != b.modified)
                        return a.modified > b.modified;
                    return a.id < b.id;
                });
                if (std::ssize(hits) > request.limit)
                    hits.resize(static_cast<std::size_t>(request.limit));
                {
                    const auto lock = index->readLock();
                    for (const NameHit& hit : hits) {
                        SearchResult r = fromIndex(*index, hit.id);
                        r.modified = knownTime(index->modified(hit.id));
                        results.push_back(std::move(r));
                    }
                }
                total = static_cast<qint64>(best.size());
                arrange(results, request, NameMatcher(withTypes(readings.front(), types, hidden)), rankBy, *m_onDisk);
                for (SearchResult& r : results)
                    r.approximate = true; // the recent ones arrange() puts first too
            }
        }
    }

    // Nothing typed: every favorite first, in its user's order; the recent
    // items after them (not twice).
    if (query.isEmpty() && !listing && !request.favorites.isEmpty() && !isStale(job.id)) {
        QList<const Favorite*> shown;
        for (const Favorite& f : request.favorites) {
            if (favoriteShown(request.scope, f, types))
                shown.push_back(&f);
        }
        SearchResults rows = favoriteRows(shown, *apps, *places, *m_onDisk, m_index);
        results.removeIf([&](const SearchResult& r) { return indexOfFavorite(request.favorites, r.path) >= 0; });
        total = rows.size() + results.size();
        rows.append(std::move(results));
        results = std::move(rows);
    }

    // Nothing typed, 全部: the switches to keep in view (a keep-awake that is
    // on) above everything else, even the favorites.
    if (query.isEmpty() && !listing && request.scope == Scope::All && !request.pinnedPlaces.isEmpty() && !isStale(job.id)) {
        SearchResults rows;
        for (const QString& path : request.pinnedPlaces) {
            const PlaceInfo* place = findPlace(*places, path);
            if (!place || place->switchId.isEmpty())
                continue;
            if (std::any_of(rows.cbegin(), rows.cend(), [&](const SearchResult& r) { return r.path == path; }))
                continue;
            rows.push_back(fromPlace(*place, false));
            rows.back().statusRow = true;
        }
        total += rows.size();
        rows.append(std::move(results));
        results = std::move(rows);
    }

    // The rows of the query before stay on screen (dimmed) until these come.
    const auto ms = [](Clock::duration d) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    };
    const auto end = Clock::now();
    if (end - job.submitted > 2s) {
        qWarning("WinShun: a search took %lld ms (%lld waiting for the search before, %lld for the index, "
                 "%lld finding names, %lld the rest)",
            ms(end - job.submitted), ms(began - job.submitted), ms(lockWait), ms(names), ms(end - began - lockWait - names));
    }

    if (isStale(job.id))
        return;
    emit resultsReady(job.id, results, total, timer.nsecsElapsed() / 1000);
    // Something typed, and the list is all there is: whether a drive the
    // index leaves out could hold what was not found (the launcher offers to
    // walk it). Not while the list goes on past what was asked for: there,
    // the end of the list is a page more, not the end of what there is.
    if (!query.isEmpty() && results.size() < request.limit) {
        const RemovableDrives drives = unindexedRemovableDrives(); // told even when there are none: one unplugged goes
        if (!isStale(job.id))
            emit removableDrives(job.id, drives);
    }
}

RemovableDrives SearchEngine::unindexedRemovableDrives()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - m_removableAt < 3s)
        return m_removable; // as a moment ago: the drives are asked about with every keystroke
    m_removableAt = now;
    m_removable.clear();
    const std::vector<std::wstring> indexed = m_index->volumeRoots();
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 2; i < 26; ++i) { // not A: and B:, floppy drives that take seconds to say they are empty
        if (!(mask & (1u << i)))
            continue;
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        // External hard disks say DRIVE_FIXED and are indexed as such: only
        // what Windows calls removable (USB sticks, memory cards), and of
        // those only the ones the index leaves out (Settings).
        if (::GetDriveTypeW(root) != DRIVE_REMOVABLE)
            continue;
        const std::wstring_view letter(root, 2);
        if (std::any_of(indexed.begin(), indexed.end(), [&](const std::wstring& r) { return win32::equalsIgnoreCase(r, letter); }))
            continue;
        wchar_t label[MAX_PATH + 1] = {};
        if (!::GetVolumeInformationW(root, label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0))
            continue; // no media (an empty card reader), or not ready
        m_removable.push_back({QString::fromWCharArray(root, 2), QString::fromWCharArray(label)});
    }
    return m_removable;
}

// A removable drive the index leaves out, or any folder it does not hold
// (only the folders chosen are indexed: GitHub #52), walked on the disk for
// the names (Request::walkFolder): each folder read once, in the order they are met,
// one at a time (a USB stick serves one request at a time), each file and
// folder matched as a path from the history is (NameMatcher::matchPath:
// pinyin, folders, ext: and exclusions as for the index). The rows go out
// every so often, each batch in the order asked (best match first, or by
// time); a folder's handle is closed before the next is opened, so the drive
// can be ejected between folders. Stops at Request::limit rows: more would be
// a list nobody reads through.
void SearchEngine::runDriveWalk(const Job& job)
{
    const Request& request = job.request;
    const ParsedQuery query = parseQuery(request.text);
    const bool foldersOnly = request.scope == Scope::Folders;
    const std::vector<std::string> types = foldersOnly ? std::vector<std::string>() : foldedExtensions(request.extensions);
    const std::vector<std::string> hidden = hiddenTypes(request, query, types);
    const NameMatcher matcher(withTypes(query, types, hidden));
    const RankBy rankBy = request.rankBy;
    const auto stale = [&] { return isStale(job.id); };
    std::optional<PreferredTypes> preferred;
    if (rankBy == RankBy::Match && !request.preferredTypes.isEmpty())
        preferred.emplace(foldedExtensions(request.preferredTypes));

    struct Hit {
        SearchResult row;
        int score;
    };
    std::vector<Hit> batch;
    int seen = 0;
    int found = 0;
    auto lastSent = std::chrono::steady_clock::now();
    const auto send = [&](bool finished) {
        if (stale())
            return;
        const FileTime until = trustedUntil();
        std::ranges::stable_sort(batch, [rankBy, until](const Hit& a, const Hit& b) {
            const FileTime ta = sortTime(a.row.modified, until);
            const FileTime tb = sortTime(b.row.modified, until);
            if (rankBy != RankBy::Match && ta != tb) {
                if (ta == 0 || tb == 0)
                    return tb == 0;
                return rankBy == RankBy::Newest ? ta > tb : ta < tb;
            }
            return a.score > b.score;
        });
        SearchResults rows;
        for (Hit& hit : batch)
            rows.push_back(std::move(hit.row));
        batch.clear();
        if (!rows.isEmpty())
            emit driveResults(job.id, rows);
        emit driveProgress(job.id, seen, finished);
        lastSent = std::chrono::steady_clock::now();
    };

    // "E:", "D:\work": no backslash at the end ("E:\" is "E:", the drive).
    std::wstring start = request.walkFolder.toStdWString();
    while (!start.empty() && start.back() == L'\\')
        start.pop_back();
    const int baseDepth = static_cast<int>(std::count(start.begin(), start.end(), L'\\'));
    const bool driveRoot = start.size() == 2 && start[1] == L':';
    std::deque<std::wstring> folders {std::move(start)};
    while (!folders.empty() && !stale() && found < request.limit) {
        const std::wstring folder = std::move(folders.front());
        folders.pop_front();
        const int depth = static_cast<int>(std::count(folder.begin(), folder.end(), L'\\')) - baseDepth;
        WIN32_FIND_DATAW fd;
        const win32::UniqueFind find(::FindFirstFileExW(win32::longPath(folder + L"\\*").c_str(), FindExInfoBasic, &fd,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (!find.valid())
            continue; // not readable (or gone): the index would have left it out too
        do {
            const std::wstring_view name(fd.cFileName);
            if (name == L"." || name == L"..")
                continue;
            const bool isDir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            // Windows' own folders at the root hold nothing of the user's.
            if (isDir && driveRoot && depth == 0 && (name == L"System Volume Information" || name == L"$RECYCLE.BIN"))
                continue;
            ++seen;
            const std::wstring path = folder + L'\\' + std::wstring(name);
            if (isDir && Crawler::shouldDescend(fd.dwFileAttributes, fd.dwReserved0))
                folders.push_back(path);
            if (foldersOnly && !isDir)
                continue;
            const QString qpath = QString::fromStdWString(path);
            if (!hidden.empty() && !ofType(qpath, isDir, {}, hidden))
                continue;
            int score = matcher.matchPath(qpath, isDir);
            if (score < 0)
                continue;
            score -= std::min(depth + 1, 12); // as a name search ranks depth
            SearchResult r;
            r.path = qpath;
            r.name = QString::fromWCharArray(fd.cFileName);
            if (preferred && !isDir && preferred->hasName(wtf8::fromUtf16(wtf8::view(r.name))))
                score += PreferredTypes::kBonus; // as a name search ranks the types listed first
            r.isDir = isDir;
            r.modified = knownTime(fileTimeOf(win32::ticks(fd.ftLastWriteTime)));
            batch.push_back({std::move(r), score});
            if (++found >= request.limit)
                break;
        } while (::FindNextFileW(find.get(), &fd));
        if (std::chrono::steady_clock::now() - lastSent > 200ms)
            send(false);
    }
    send(true);
}

void SearchEngine::runContentSearch(const Job& job)
{
    QElapsedTimer timer;
    timer.start();
    const Request& request = job.request;
    const bool byTime = request.rankBy != RankBy::Match;
    const FileTime until = trustedUntil(); // candidates by time: one dated ahead as not known
    emit resultsReady(job.id, {}, 0, 0);
    if (request.text.trimmed().isEmpty()) {
        emit contentProgress(job.id, 0, 0, true);
        return;
    }
    const auto index = m_index->index();
    // Where it looks, and what for: "年度报告 abc/" in the folders named abc.
    // Named, and none found there (a path not in the index, one on a
    // share): nowhere, not everywhere.
    ScopeFolders scope;
    ContentScope where;
    {
        const auto lock = index->readLock();
        where = resolveScope(*index, request, m_pool, [&] { return isStale(job.id); }, scope);
    }
    if (isStale(job.id))
        return;
    emit contentScope(job.id, where);
    // A name no folder has is among the words (Status::Words): no scope of its own.
    const bool scoped = request.contentSet != ContentSet::Given
        && std::ranges::any_of(where.places, [](const ContentScope::Place& p) { return p.status != ContentScope::Status::Words; });
    const QStringList terms = contentTerms(where.words);
    if (terms.isEmpty() || (scoped && scope.folders.empty())) {
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
    // (ContentIndexer::Intake): the next search knows them. Ranked by time,
    // the newest (or oldest) first within those two, so that the matches
    // kept when there are too many are the ones listed first.
    struct Candidate {
        EntryId id;
        std::uint8_t order; // lower first
        bool indexed; // the content index knows it
        FileTime modified; // knownTime: 0 when not known
    };
    using ContentSet = SearchEngine::ContentSet;
    const ContentSet set = request.contentSet;
    const bool oneOff = set != ContentSet::Listed; // a part passed over, read this once (see ContentSet)
    // The kinds of text the list leaves out: counted as the candidates are
    // picked, or (ContentSet::OtherTypes) the candidates themselves.
    const OtherTextTypes otherTypes(request.contentExtensions);
    const QStringList listed
        = request.contentDocuments ? request.contentExtensions + documentExtensions() : request.contentExtensions;
    const ContentFilter filter {ExtensionFilter(set == ContentSet::OtherTypes ? otherTypes.extensions() : listed),
        request.contentInLowPriority || set == ContentSet::LowPriority};
    const ExtensionFilter skipped(request.excludedTypes); // packed once, asked of each candidate
    const auto content = m_index->contentIndex();
    const ContentIndex::SearchGuard searching(content.get()); // the content indexer waits meanwhile
    std::vector<Candidate> candidates;
    int ruledOut = 0;
    // What the scan passes over, told once it has looked at every candidate
    // (contentLeftOut), so that each part can be asked for after all. The
    // files over the size limit by path: the content index knows some as
    // such, the rest are seen when opened (largeMutex: by the readers).
    ContentLeftOut leftOut;
    std::mutex largeMutex;
    FileIndex::IdPin pin;
    {
        const auto lock = index->readLock();
        pin = index->pinIds(); // from here on the index is not compacted: the content index's ids stay its own
    }
    // Not under the lock: the content index may have to be read from disk
    // first (on a hard disk, one that may first have to spin up), and the
    // index would wait for that, and name searches behind it.
    QElapsedTimer lookupTimer;
    lookupTimer.start();
    const ContentIndex::Lookup lookup = content && !oneOff ? content->lookup(terms) : ContentIndex::Lookup();
    if (lookupTimer.elapsed() > 50) // a few milliseconds from the cache: it was read off the disk
        m_index->prewarmSoon(std::chrono::seconds(5));
    if (isStale(job.id))
        return;
    // Where a file is, for ranking (contentRelevance): your own folders
    // first, then elsewhere, hidden ones, system and program folders.
    const auto priorityOf = [](const Entry& e, EntryId id, UnderFolder& inProfile) -> std::uint8_t {
        if (e.flags & EntryFlag::LowPriority)
            return 3;
        if (e.flags & EntryFlag::Hidden)
            return 2;
        return inProfile(id) ? 0 : 1;
    };
    if (set == ContentSet::Given) {
        // The files asked for, as they are now; none as the content index
        // knows it (as too large, or as it was when it fit the limit).
        const auto lock = index->readLock();
        UnderFolder inProfile(*index, index->findPath(win32::expandEnvironment(L"%USERPROFILE%")));
        for (const QString& file : request.contentOnly) {
            const EntryId id = index->findPath(file.toStdWString());
            if (id == kNoEntry || index->entry(id).isDeleted())
                continue;
            candidates.push_back({id, priorityOf(index->entry(id), id, inProfile), false,
                byTime ? sortTime(index->modified(id), until) : FileTime {0}});
        }
    } else {
        // Every entry is looked at: a chunk of them per worker (on one
        // thread, 3 million take some 15 ms). On the way, what is passed
        // over is counted (ContentLeftOut): the files of listed types in
        // system and program folders, and the files of text types not listed,
        // each kind of those on its own (the hint names the most numerous).
        const auto lock = index->readLock();
        constexpr std::uint32_t kMatch = ContentIndex::Lookup::kMatch;
        const EntryId profile = index->findPath(win32::expandEnvironment(L"%USERPROFILE%"));
        std::vector<std::vector<Candidate>> found(index->chunkCount());
        std::vector<int> ruledOutOf(index->chunkCount(), 0);
        std::vector<std::vector<EntryId>> largeOf(index->chunkCount());
        std::vector<int> lowPriorityOf(index->chunkCount(), 0);
        std::vector<std::vector<int>> otherTypesOf(index->chunkCount()); // per kind (otherTypes.count()), made when first met
        const bool bounded = request.filter.byTime() || request.filter.bySize(); // the panel's: when written, how large
        m_pool.parallelFor(index->chunkCount(), [&](std::size_t c) {
            if (isStale(job.id))
                return;
            UnderFolder inProfile(*index, profile);
            UnderFolders inScope(*index, scope);
            const auto first = static_cast<EntryId>(c << FileIndex::kChunkBits);
            auto k = static_cast<std::size_t>(std::lower_bound(lookup.known.begin(), lookup.known.end(), first,
                [](std::uint32_t known, EntryId id) { return (known & ~kMatch) < id; }) - lookup.known.begin());
            const auto entries = index->chunk(c);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const Entry& e = entries[i];
                // Outside the folders named: neither read nor counted as passed over.
                const auto within = scoped ? inScope(static_cast<EntryId>(first + i)) : UnderFolders::Where::Inside;
                if (within == UnderFolders::Where::Outside)
                    continue;
                const bool aside = within == UnderFolders::Where::InsideAside; // a system folder named: read
                if (set == ContentSet::LowPriority && (aside || !(e.flags & EntryFlag::LowPriority)))
                    continue; // only those: the rest were read
                std::string_view name;
                std::size_t extLength = 0;
                switch (filter.classify(*index, e, name, extLength)) {
                case ContentVerdict::Never:
                    continue;
                case ContentVerdict::LowPriority:
                    if (aside)
                        break;
                    ++lowPriorityOf[c];
                    continue;
                case ContentVerdict::OtherType:
                    // Of a kind the one-off over them would read: where this scan looks.
                    if (set == ContentSet::Listed && (filter.includeLowPriority || aside || !(e.flags & EntryFlag::LowPriority))) {
                        if (const int t = otherTypes.indexOf(name, extLength); t >= 0) {
                            if (otherTypesOf[c].empty())
                                otherTypesOf[c].assign(otherTypes.count(), 0);
                            ++otherTypesOf[c][static_cast<std::size_t>(t)];
                        }
                    }
                    continue;
                case ContentVerdict::Read:
                    break;
                }
                // A skipped type (Settings): neither read nor counted as passed over.
                if (!skipped.empty() && skipped.matches(name, extLength))
                    continue;
                const auto id = static_cast<EntryId>(first + i);
                if (bounded && !request.filter.fits(index->modified(id), index->size(id), false))
                    continue; // neither read nor counted as passed over
                bool indexed = false;
                bool likely = false;
                while (k < lookup.known.size() && (lookup.known[k] & ~kMatch) < id)
                    ++k;
                if (k < lookup.known.size() && (lookup.known[k] & ~kMatch) == id) {
                    indexed = true;
                    // Known to be over the size limit: left out whatever the
                    // phrase (opening it would only find that out again).
                    if (!lookup.large.empty() && std::ranges::binary_search(lookup.large, id)) {
                        largeOf[c].push_back(id);
                        ++ruledOutOf[c];
                        continue;
                    }
                    if (lookup.usable) {
                        if (!(lookup.known[k] & kMatch)) {
                            ++ruledOutOf[c];
                            continue;
                        }
                        likely = true;
                    }
                }
                const std::uint8_t priority = priorityOf(e, id, inProfile);
                found[c].push_back({id, static_cast<std::uint8_t>(likely ? priority : 4 + priority), indexed,
                    byTime ? sortTime(index->modified(id), until) : FileTime {0}});
            }
        });
        if (isStale(job.id))
            return;
        std::size_t count = 0;
        for (const auto& f : found)
            count += f.size();
        candidates.reserve(count);
        std::vector<int> otherCounts(otherTypes.count(), 0);
        for (std::size_t c = 0; c < found.size(); ++c) {
            candidates.insert(candidates.end(), found[c].begin(), found[c].end());
            ruledOut += ruledOutOf[c];
            for (const EntryId id : largeOf[c])
                leftOut.large.append(QString::fromStdWString(index->wpath(id)));
            leftOut.lowPriority += lowPriorityOf[c];
            for (std::size_t t = 0; t < otherTypesOf[c].size(); ++t)
                otherCounts[t] += otherTypesOf[c][t];
        }
        // The kinds not listed, the most numerous first; the hint names a few.
        std::vector<std::size_t> kinds;
        for (std::size_t t = 0; t < otherCounts.size(); ++t) {
            if (otherCounts[t] > 0)
                kinds.push_back(t);
        }
        std::ranges::stable_sort(kinds, [&](std::size_t a, std::size_t b) { return otherCounts[a] > otherCounts[b]; });
        for (const std::size_t t : kinds) {
            leftOut.otherTypes += otherCounts[t];
            if (leftOut.otherExtensions.size() < 3)
                leftOut.otherExtensions.append(otherTypes.extension(t));
        }
    }
    if (byTime) {
        const bool newest = request.rankBy == RankBy::Newest;
        std::stable_sort(candidates.begin(), candidates.end(), [newest](const Candidate& a, const Candidate& b) {
            if ((a.order < 4) != (b.order < 4))
                return a.order < 4;
            if (a.modified != b.modified) {
                if (a.modified == 0 || b.modified == 0)
                    return b.modified == 0;
                return newest ? a.modified > b.modified : a.modified < b.modified;
            }
            return a.order < b.order;
        });
    } else {
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.order < b.order; });
    }

    const ContentScanner scanner(terms);
    const std::int64_t now = QDateTime::currentSecsSinceEpoch();
    // Before the reads, gone once they are done. Not for a part read this
    // once: the index is to stay as it is about those files (see ContentSet).
    ContentIndexer::Intake intake(oneOff ? nullptr : m_index->contentIndexer());
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
        // In the list's order (the launcher places them among the rows it has).
        std::ranges::stable_sort(
            out, [&](const SearchResult& a, const SearchResult& b) { return rankedBefore(a, b, request.rankBy); });
        if (!out.isEmpty())
            emit contentResults(job.id, out);
        if (finished && !oneOff && scanned.load() == total) {
            // Every candidate was looked at (the scan did not stop at its
            // limit): the files left out for their size are all known.
            std::lock_guard lock(largeMutex);
            emit contentLeftOut(job.id, leftOut);
        }
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
                    FileTime modified = 0;
                    {
                        const auto lock = index->readLock();
                        gone = index->entry(candidate.id).isDeleted();
                        if (!gone) {
                            path = index->wpath(candidate.id); // as it is now: it may have moved
                            modified = index->modified(candidate.id);
                        }
                    }
                    if (gone) {
                        scanned.fetch_add(1);
                        continue;
                    }
                    const std::int64_t maxBytes = request.contentSizeLimits.of(path);
                    std::optional<ContentMatch> match;
                    doctext::Location location;
                    bool overLimit = false; // more than maxBytes: left unread
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
                            overLimit = read.large;
                        } else if (!text) {
                            DocExtractor::Result read = m_documents->extractFile(path, maxBytes, stale);
                            if (read.status == extractproto::Status::Ok)
                                text = std::move(read.text);
                            overLimit = read.large;
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
                        overLimit = read.text.large;
                    } else {
                        match = scanner.scanFile(path, maxBytes, stale, &overLimit);
                    }
                    if (overLimit) {
                        std::lock_guard lock(largeMutex);
                        leftOut.large.append(QString::fromStdWString(path));
                    }
                    scanned.fetch_add(1);
                    if (!match)
                        continue;
                    SearchResult r;
                    r.path = QString::fromStdWString(path);
                    r.name = r.path.mid(r.path.lastIndexOf(u'\\') + 1);
                    r.modified = modified;
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
                    r.snippetMatches = std::move(match->matches);
                    r.relevance = contentRelevance(*match, terms, r.name, candidate.order & 3,
                        modified ? now - static_cast<std::int64_t>(modified) : -1);
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
        // row alone would flash), or, later than that, as soon as there is
        // one (not at the next 100 ms tick); the rest every 100 ms.
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
                if (ready == 0 || (ready < kFirstContentBatch && std::chrono::steady_clock::now() - start < 50ms))
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
