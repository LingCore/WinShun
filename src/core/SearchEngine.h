#pragma once

#include "ContentIndex.h"
#include "Favorites.h"
#include "SearchTypes.h"
#include "WebShortcut.h"
#include "WorkerPool.h"

#include <QObject>
#include <QStringList>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ws {

class AppCatalog;
class DocExtractor;
class IndexService;
class RecentOnDisk;
class SystemCatalog;

// Runs searches on worker threads (name searches apart from content scans and
// drive walks). Only the newest request matters: a new submit() cancels
// whatever is running, so typing never queues up work.
// Name searches also go through the installed apps, the places in Windows
// (`apps` and `places` may be null) and the web shortcuts.
// Signals are emitted from the worker threads (queued to the receivers).
// Whether content match `a` goes before `b`: the better match, or by when
// last written (a time not known last either way, the better match on a tie).
bool rankedBefore(const SearchResult& a, const SearchResult& b, RankBy by) noexcept;

// What a content scan passed over, told once it has looked at every
// candidate (SearchEngine::contentLeftOut): the launcher offers each part
// after the last row, to be searched this once (Request::contentSet).
struct ContentLeftOut {
    QStringList large; // over the size limit, by path (the content index knew some, the rest were opened)
    int lowPriority = 0; // of a listed type, in system, program and tool folders (not asked for: contentInLowPriority)
    int otherTypes = 0; // text of a kind not listed (OtherTextTypes)
    QStringList otherExtensions; // ... the most numerous few of those kinds, "vue", "kt"
    // Of a listed type, on a share or in WSL, the content index not having
    // read them: their share's contents are not indexed, or not yet (GitHub #114).
    int remote = 0;
    friend bool operator==(const ContentLeftOut&, const ContentLeftOut&) = default;
};

// Where a content search looks, as it made out the folders named among its
// words (ContentFolderSpec: "年度报告 abc/") or given (Request::contentFolders),
// told before it reads anything (SearchEngine::contentScope). No places:
// everywhere the settings say.
struct ContentScope {
    enum class Status : std::uint8_t {
        Searched, // its folders are looked in
        NotFound, // a path that is not in the index (not there, left out, a drive not indexed)
        Words, // no folder so named: what was typed is among the words instead
    };
    struct Place {
        QString typed; // as typed ("abc/", "D:\My Files\"); empty for one given
        Status status = Status::Searched;
        // The folders it stands for, your own first, then the shallowest (the
        // first kListed of `count`); the one chosen (Request::contentPick), if
        // one was, alone.
        QStringList folders;
        int count = 0;
        QString chosen; // the folder looked in when one of several was picked; empty: all of them
        bool approximate = false; // none named just so: those whose names hold it (or sound so)
        friend bool operator==(const Place&, const Place&) = default;
    };
    static constexpr qsizetype kListed = 50;
    std::vector<Place> places;
    QString words; // what it looks for, after the folders came out
    friend bool operator==(const ContentScope&, const ContentScope&) = default;
};

// A removable drive (a USB stick, a memory card) with media in it that the
// index leaves out (Settings: includeRemovableDrives), as a name search
// found it (SearchEngine::removableDrives). The launcher offers to walk it
// for the query (Request::walkFolder).
struct RemovableDrive {
    QString root; // "E:"
    QString label; // its volume label; may be empty
    friend bool operator==(const RemovableDrive&, const RemovableDrive&) = default;
};
using RemovableDrives = QList<RemovableDrive>;

class SearchEngine : public QObject {
    Q_OBJECT

public:
    // Which files a content search looks in (its candidates): those the
    // settings say, or, asked for after a scan said how many it passed over
    // (ContentLeftOut), one of those parts alone. Those are read this once:
    // the content index is neither asked about them nor given what is read
    // (its own pass keeps to the settings and would drop them again; and a
    // file over the limit has nearly every gram, so it would be read for
    // nearly every phrase).
    enum class ContentSet : std::uint8_t {
        Listed, // the extensions set, and documents if asked; not system folders unless asked
        LowPriority, // only the files of listed types in system, program and tool folders
        OtherTypes, // only the files of text types not listed (OtherTextTypes), where the scan looked
        Given, // only the files named in contentOnly, as they are now
    };

    struct Request {
        QString text;
        Scope scope = Scope::All;
        // 文件 with a type chosen (图片…): only files with one of these
        // extensions (without the dot, any case), typed or not; with ext:
        // typed too, those of both. Empty: any file or folder.
        QStringList extensions;
        // Settings' skipped file types (without the dot, any case): files with
        // these never come up, names or content, unless asked for by that
        // extension (in `extensions`, or ext: typed).
        QStringList excludedTypes;
        // And the skipped file names (GitHub #132: "~$*", Office's and WPS's
        // lock files): wildcards against a file's whole name, any case;
        // shown when a word typed is one of them ("~$报告").
        QStringList excludedFileNames;
        // The search panel's filters: when written and how large (with
        // size: and dm: typed, the narrower of the two), and the folders to
        // look under (full paths; empty: anywhere). Names only; 内容 takes
        // its folders as contentFolders.
        EntryFilter filter;
        QStringList folders;
        // Typed in a folder's window (Explorer, Directory Opus: GitHub #110):
        // that folder, and `text` is what was typed after it. What it finds
        // in the folder comes first (its own files and folders from the
        // disk, as a typed path lists them, then what lies deeper), then
        // what it finds elsewhere, marked (SearchResult::elsewhere), as
        // Listary lists it; with `currentFolderOnly`, nothing elsewhere.
        // Names only: 内容 is given the folder in its text, as typed.
        QString currentFolder;
        bool currentFolderOnly = false;
        // The panel's column order; Rank: rankBy.
        SortOrder order;
        // The panel: with nothing typed, every file and folder the filters
        // let through (in `order`), not the start list.
        bool browse = false;
        // Set: not a name search but a walk of this folder on the disk ("E:",
        // a drive the index leaves out: RemovableDrive; or any folder it does
        // not hold: "D:\work", GitHub #52), for the files and folders named as
        // `text` says (scope, extensions and rankBy as for names). The rows
        // come in batches (driveResults), with how far it is (driveProgress);
        // a new request stops it, as any search.
        QString walkFolder;
        KindOrder kindOrder = KindOrder::FilesFirst; // 全部, 文件
        // Files and folders, and content matches (全部: its apps, places and
        // web shortcuts stay first, by match).
        RankBy rankBy = RankBy::Match;
        // Ranked by match: files of these types (extensions without the dot,
        // any case) before others that match about as well, and first among
        // those named just what was typed ("curl": curl.exe before the folder
        // curl and curl.h); see PreferredTypes. Empty: none.
        QStringList preferredTypes;
        StartList startList = StartList::Opened; // nothing typed: what was opened, or written, last
        // Names; with nothing typed, the files written last. Asked again with
        // a higher one (the launcher, as its user scrolls down), the list
        // starts as the shorter one did, more after it; so do content matches.
        int limit = 100;
        QStringList history; // newest first
        QStringList contentExtensions; // e.g. {"txt"}
        ContentSizeLimits contentSizeLimits; // by kind of file
        bool contentDocuments = true; // and Word, Excel, PowerPoint, PDF (documentExtensions())
        int maxContentResults = 300;
        // Also inside system, program and tool folders (EntryFlag::LowPriority).
        bool contentInLowPriority = false;
        // Files read at once (each open waits for the antivirus, so more
        // than processors). Asked for (内容): many; 全部's own: a few.
        int contentThreads = 4;
        ContentSet contentSet = ContentSet::Listed;
        // ContentSet::Given: the files, by path (the ones a scan before left
        // out for being over the size limit, ContentLeftOut::large, asked for
        // after all). The limits given still apply: the caller raises them.
        QStringList contentOnly;
        // Content: only in these folders (full paths) and the folders in them,
        // unless the text names folders of its own (ContentFolderSpec), which
        // go first. A folder named so is looked in even if it is a system,
        // program or tool folder.
        QStringList contentFolders;
        // A folder named as typed (ContentFolderSpec::typed: "abc/") that
        // stands for several, and the one of them to look in alone.
        struct ContentPick {
            QString typed;
            QString folder;
        } contentPick;
        // 全部: a keyword typed first puts its page or search on top; names find them too.
        WebShortcuts web;
        // Listed first with nothing typed, the first few that match first
        // when typed, a favorite's keyword alone on top (after a web one).
        FavoriteList favorites;
        // 全部, nothing typed: these places (as History has them,
        // placePath) listed first, above the favorites, marked statusRow:
        // a switch that is on and must be seen to be ended (keep awake).
        // Others in the start list: none, a switch is never listed there.
        QStringList pinnedPlaces;
    };

    explicit SearchEngine(
        IndexService* index, AppCatalog* apps = nullptr, SystemCatalog* places = nullptr, QObject* parent = nullptr);
    ~SearchEngine() override;

    quint64 submit(Request request);
    void cancel();

signals:
    // Name search, or the (empty) start of a content search.
    void resultsReady(quint64 requestId, const ws::SearchResults& results, qint64 totalMatches, qint64 elapsedUs);
    // Content search delivers matches incrementally.
    void contentResults(quint64 requestId, const ws::SearchResults& results);
    void contentProgress(quint64 requestId, int scanned, int total, bool finished);
    // Once a content scan of ContentSet::Listed has looked at every candidate
    // (not when it stopped at maxContentResults): what it passed over, before
    // the contentProgress that says it finished. The launcher offers each
    // part (Request::contentSet).
    void contentLeftOut(quint64 requestId, const ws::ContentLeftOut& leftOut);
    // A content scan, before it reads anything: where it looks.
    void contentScope(quint64 requestId, const ws::ContentScope& scope);
    // After a name search's resultsReady, when something was typed and the
    // list is all there is (fewer than Request::limit): the removable drives
    // the index leaves out (none: an empty list). The launcher offers to
    // walk one (Request::walkFolder).
    void removableDrives(quint64 requestId, const ws::RemovableDrives& drives);
    // A walk (Request::walkFolder): its rows as they are found, best of each
    // batch first, and how many files and folders it has looked at.
    void driveResults(quint64 requestId, const ws::SearchResults& results);
    void driveProgress(quint64 requestId, int seen, bool finished);

private:
    struct Job {
        quint64 id = 0;
        Request request;
        std::chrono::steady_clock::time_point submitted;
    };

    // Each request runs on one of two threads (GitHub #115): name searches on
    // one, content scans and drive walks on the other. A scan stops at its own
    // pace (the file in hand: an antivirus checking it, a hard disk spinning
    // up; the content index read off the disk first), and the name search
    // typed meanwhile does not wait for it. Only the newest request runs at
    // all (m_latest): the other thread's, if any, is stopping.
    enum class Lane : std::uint8_t { Names, Scans };
    static Lane laneOf(const Request& request) noexcept
    {
        return request.scope == Scope::Content || !request.walkFolder.isEmpty() ? Lane::Scans : Lane::Names;
    }

    void workerLoop(std::stop_token stop, Lane lane);
    void drivesLoop(std::stop_token stop);
    void leaveBehind(std::chrono::steady_clock::time_point now); // m_mutex held, before m_latest moves on
    void runNameSearch(const Job& job);
    void runContentSearch(const Job& job);
    void runDriveWalk(const Job& job);
    RemovableDrives unindexedRemovableDrives(); // the drives thread: as of a moment ago (m_removable)
    bool isStale(quint64 id) const noexcept { return m_latest.load(std::memory_order_relaxed) != id; }

    IndexService* m_index;
    std::unique_ptr<DocExtractor> m_documents; // reads documents the content index has no text of
    AppCatalog* m_apps;
    SystemCatalog* m_places;
    std::unique_ptr<RecentOnDisk> m_onDisk; // whether recent items are still there
    // For testing (GitHub #115): WINSHUN_SLOW_READS=<ms> set as the engine is
    // made, each file a content scan reads keeps it that much longer, a newer
    // search notwithstanding, as a hard disk spinning up or an antivirus
    // holding the file would.
    const std::chrono::milliseconds m_slowReads;
    // The removable drives as last looked at (unindexedRemovableDrives), and
    // when: asked of Windows every few seconds at most (the volume label is
    // read from the drive), the drives thread only.
    RemovableDrives m_removable;
    std::chrono::steady_clock::time_point m_removableAt {};
    WorkerPool m_pool {WorkerPool::defaultThreadCount()}; // both lanes: one parallelFor at a time
    std::mutex m_mutex;
    std::condition_variable_any m_cv; // both lanes and the drives thread
    std::array<std::optional<Job>, 2> m_pending; // by Lane
    // The name search whose rows ask which removable drives there are, on a
    // thread of their own: Windows may take its time to say, and the index's
    // list of volumes waits for its journal work.
    std::optional<quint64> m_drivesFor;
    quint64 m_counter = 0;
    std::atomic<quint64> m_latest {0};
    // The newest name search (0: cancelled since). Its drives are told even
    // after the content scan that follows it in 全部, as the launcher keeps
    // them for the names' request.
    std::atomic<quint64> m_latestNames {0};
    // For the log, how long a content scan left behind took to stop: the one
    // running (0: none; under m_mutex), and when the request came that left
    // it behind (steady_clock ticks).
    quint64 m_scanRunning = 0;
    std::atomic<std::int64_t> m_scanLeftBehindAt {0};
    // Last: started after everything above exists.
    std::jthread m_drivesWorker;
    std::jthread m_scanWorker;
    std::jthread m_nameWorker;
};

} // namespace ws

Q_DECLARE_METATYPE(ws::ContentLeftOut)
Q_DECLARE_METATYPE(ws::ContentScope)
Q_DECLARE_METATYPE(ws::RemovableDrive)
