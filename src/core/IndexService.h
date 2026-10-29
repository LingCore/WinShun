#pragma once

#include "ChangeWatcher.h"
#include "ContentIndex.h"
#include "ContentIndexer.h"
#include "Crawler.h"
#include "FileIndex.h"
#include "Ntfs.h"
#include "RemoteContent.h"
#include "ShareIndexer.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace ws {

// Owns the file index and keeps it current:
//   1. startup: load the snapshot (searchable within a second), keeping the
//      volumes that are still there and reading the new ones
//   2. NTFS volumes: catch up with each volume's change journal from where
//      the snapshot left off, then follow it live. When the journal no
//      longer reaches back that far (or on the first run), read the MFT.
//   3. other volumes (exFAT, FAT, ReFS): watch them with ReadDirectoryChangesW
//      and, after a short delay, walk them in place at low I/O priority to
//      pick up changes made while the app was not running
//   4. drives come and go: new ones are read, gone ones dropped; one that is
//      being ejected or locked is let go of first (suspendVolume)
//   5. periodically and on exit: save a compact snapshot
//   6. the content index (ContentIndex) of the files on NTFS volumes: the
//      journal says which files were written to, the ContentIndexer reads
//      them again in the background; it is saved with the snapshot
//   7. its files (see indexfolder) can be moved to another folder while it
//      runs (moveTo)
//   8. network shares the user added: each a root of the index of its own,
//      kept current by the ShareIndexer
//   9. with no way to the journals, only the folders the user chose, when
//      asked to (GitHub #52, Options::folders): the disks they are on keep
//      the folders above them (their names only) and the chosen ones, walked
//      at startup, when the watcher lost events and every few hours
//
// The first run has no snapshot: the (empty) index is searchable at once and
// fills up while the volumes are read. There is only ever one index in memory.

// GitHub #52: whether a chosen folder is walked again now. Due `every` after
// its last walk (seconds since 1970) and no sooner than 60 times as long as
// that took, then once the user is away (win32::userAway), or two hours later
// whatever they do. `every` 0: never (only at startup); never walked: now.
enum class VerifyNow { No, IfAway, Yes };
VerifyNow folderVerifyDue(
    std::int64_t lastVerified, std::chrono::milliseconds took, std::chrono::hours every, std::int64_t now) noexcept;

// GitHub #52: how long after a watcher lost events of chosen folders they
// are walked again: 3 s; 30 s when it happens again within 10 minutes of the
// last time, 5 minutes from the third time on; 3 s again after 10 minutes
// without (a disk that keeps losing them is not walked over and over).
class LostEventsBackoff {
public:
    std::chrono::milliseconds next(std::chrono::steady_clock::time_point now) noexcept;

private:
    int m_streak = 0;
    std::optional<std::chrono::steady_clock::time_point> m_last;
};

// GitHub #52: of a batch of changes a watcher saw, those to apply while only
// the chosen folders are indexed (`chosenOn` gives a volume's, "C:"; none for
// one indexed whole, whose changes all stay): what happens in a chosen
// folder, and above one what takes it away. Half a rename that leaves (or
// enters) one is a removal (or an addition) by itself, never paired with half
// of another; a folder above chosen ones renamed (or moved above others) is
// moved with what the index has of it (the new half moveOnly). `owed` gets
// the chosen folders something happened to or above, or whose watcher lost
// events (to be walked again in a moment; `lost` then), not for a new name
// that differs in case only; `wholeLost` says a volume indexed whole lost
// events. A folder above chosen ones said to be gone (not half of a rename
// here) that `stillThere` finds on the disk stays, and nothing is owed:
// Windows says so of one whose name changes in case only, before the rename.
struct ScopedChanges {
    std::vector<FsChange> apply;
    std::vector<std::wstring> owed;
    bool lost = false;
    bool wholeLost = false;
};
ScopedChanges changesInScope(std::vector<FsChange> changes,
    const std::function<const std::vector<std::wstring>&(std::wstring_view)>& chosenOn,
    const std::function<bool(const std::wstring&)>& stillThere = {});

// GitHub #52: a worker pass is over. `running` (IndexService's) is cleared,
// and only then is `owed` asked whether chosen folders were owed a walk
// meanwhile, which then get a pass of their own (true). In that order one
// asked for at the last moment is never lost: the pass sees it, or whoever
// asked sees no pass running and starts one (walkOwedFolders).
bool passOver(std::atomic<bool>& running, const std::function<bool()>& owed);

class IndexService : public QObject {
    Q_OBJECT

public:
    enum class State { Idle, Loading, Building, Ready };
    Q_ENUM(State)

    struct Options {
        CrawlRules rules;
        bool includeRemovable = false;
        std::optional<std::vector<std::wstring>> selectedDrives; // unset: all; empty: no local disks
        bool rescanOnStartup = true; // walked volumes only: NTFS always catches up
        int rescanDelayMs = 15000;
        ContentIndexer::Options content;
        ShareIndexer::Options shares;
        // Where NTFS volumes' MFT records and change journals come from:
        // the volumes themselves (empty: DirectVolumeAccess, administrator
        // rights), the index service, or nowhere (every volume walked and
        // watched). Taken at construction; setOptions() leaves it as it is,
        // setVolumeAccess() replaces it.
        std::shared_ptr<ntfs::VolumeAccess> volumeAccess;
        // With no way to the journals (NoVolumeAccess) and a snapshot that
        // followed NTFS volumes through theirs, the passes wait this long
        // after start() for setVolumeAccess() before treating those volumes
        // as walked: the index service comes up a moment after the user's
        // Win顺 at login, and a volume walked meanwhile would have lost its
        // journal position (a whole MFT read once the service is there,
        // instead of catching up). 0: not at all.
        int accessWaitMs = 0;
        // GitHub #52: only these folders ("C:\Users\me\Documents": the drive
        // letter upper case, no backslash at the end, none inside another)
        // and the folders above them (names only), nothing else of the disks
        // they are on; the other disks are left out, removable ones taken
        // whole when includeRemovable says so. A drive's root ("D:") takes
        // that drive whole. Unset: whole disks, as before. Only while there is
        // no way to the journals (NoVolumeAccess), when every volume is walked.
        std::optional<std::vector<std::wstring>> folders;
        // Explicit directory links are walked in addition to whole-disk indexing,
        // also when the MFT and journals are read by the service or administrator.
        std::vector<std::wstring> extraFolders;
        // Each chosen folder is walked again this often (folderVerifyDue).
        // 0: only at startup, as rescanOnStartup says.
        std::chrono::hours folderVerifyEvery {24};
        int folderVerifyCheckMs = 60'000; // how often what is due is looked at (tests: sooner)
    };

    // GitHub #52: a chosen folder (Options::folders) as it stands.
    struct FolderStatus {
        enum class State : std::uint8_t {
            Waiting, // not walked yet
            Verifying, // being walked, or about to be
            Ready,
            NotFound, // its disk is there, the folder is not: its entries are gone
            Unreadable, // there, but this account cannot list it: what the index has of it stays
            Excluded, // in a folder left out of searches: not indexed
            Offline, // its disk is not there
            WholeDrive, // a drive's root: the drive is indexed whole
        };
        std::wstring path;
        State state = State::Waiting;
        std::size_t items = 0; // in it, at its last walk
        std::size_t failed = 0; // folders in it that could not be read then
        std::int64_t lastVerified = 0; // seconds since 1970 (UTC); 0: never
        std::chrono::milliseconds took {0}; // the last walk
        bool watching = true; // its changes come in as they happen; false: only with the next walk
        bool operator==(const FolderStatus&) const = default;
    };

    // `folder` holds the index files (indexfolder), or will.
    IndexService(QString folder, Options options, QObject* parent = nullptr);
    ~IndexService() override;

    void start();
    void rebuild(); // reads every volume again
    // New rules are applied to the index in place: newly excluded folders are
    // dropped, folders no longer excluded are walked. Only a folder name that
    // is no longer excluded (it could be anywhere) takes reading every volume.
    // Other folders chosen (Options::folders) are applied as setScope does.
    void setOptions(Options options);
    // GitHub #52: these folders from now on (Options::folders), in place: what
    // is no longer chosen goes, new ones are walked; from whole disks, all
    // but the chosen folders goes; back to whole disks, those disks are
    // walked whole. GUI thread.
    void setScope(std::optional<std::vector<std::wstring>> folders);
    // Only the chosen folders are indexed now: some are set, and there is no
    // way to the journals.
    bool foldersOnly() const;
    std::vector<FolderStatus> folderStatuses() const; // in the order of Options::folders
    // Walks a chosen folder again now, not waiting for the user to be away
    // (after the walk under way, if any). GUI thread.
    void verifyFolder(const std::wstring& path);
    // Another way to the volumes from now on (the index service connected,
    // or went away for good; the mode was changed): NTFS volumes are
    // planned again (Pass::Volumes). A volume followed through its journal
    // goes on from its position; a walked one is read from its MFT when the
    // new access can. GUI thread.
    void setVolumeAccess(std::shared_ptr<ntfs::VolumeAccess> access);
    void shutdown(); // stops threads, saves the snapshot; idempotent

    // Moves the index files to `folder`, in the background: searches go on,
    // the content indexer waits meanwhile. Reports through moveProgress()
    // and moveFinished(); when it fails, the files stay (and are used) where
    // they were. Ignored while a move is under way.
    void moveTo(const QString& folder);
    bool moving() const noexcept { return m_moving.load(); }

    // On a hard disk the content index's files are read ahead into the file
    // cache, so that content searches do not wait for the disk (or for it to
    // spin up): a while after start, after a move, after Windows resumes, and
    // when a search found them gone from the cache. Any thread; of two
    // requests, the earlier one counts.
    void prewarmSoon(std::chrono::milliseconds delay);

    // Drives coming and going; GUI thread only. suspendVolume() stops
    // reading, following and watching a volume and returns once every handle
    // on it is closed, so it can be ejected (or locked, by format or chkdsk).
    // Its entries stay until it is gone. resumeVolume() ends that: the volume
    // is followed again if it is still there, dropped if not.
    void suspendVolume(const std::wstring& root); // "E:"
    void resumeVolume(const std::wstring& root);
    void refreshVolumes(); // drives were added or removed (handled a moment later, all at once)
    std::vector<std::wstring> volumeRoots() const; // the indexed volumes, suspended ones included

    std::shared_ptr<FileIndex> index() const { return m_index.load(); }
    // Its entry ids are the index's: look things up with the index's read lock held.
    std::shared_ptr<const ContentIndex> contentIndex() const { return m_content; }
    bool readingContent() const noexcept { return m_contentIndexer->reading(); }
    // Content searches hand it what they read (ContentIndexer::Intake).
    ContentIndexer* contentIndexer() const noexcept { return m_contentIndexer.get(); }
    State state() const noexcept { return m_state.load(); }
    ntfs::VolumeAccess::Kind volumeAccessKind() const noexcept { return m_access.load()->kind(); }
    // The one in use (a ServiceVolumeAccess says the service's version, or why not).
    std::shared_ptr<ntfs::VolumeAccess> volumeAccess() const { return m_access.load(); }
    ShareIndexer& shares() const noexcept { return *m_shares; }
    // The contents of the shares that have them indexed (GitHub #114): how
    // far each is read. A change is told by sharesChanged().
    std::vector<remotecontent::Indexer::Status> shareContent() const { return m_shareContent->statuses(); }
    bool isRefreshing() const noexcept { return m_refreshing.load(); }
    std::size_t itemCount() const;
    // A volume being read from disk now: its MFT, or walked.
    struct Reading {
        std::wstring root; // "D:"
        int percent = -1; // of its MFT read so far; -1 while walked, or while the index is built from what was read
        bool operator==(const Reading&) const = default;
    };
    std::vector<Reading> readingVolumes() const;

signals:
    void stateChanged();
    void volumesChanged(); // volumeRoots() differs
    void moveProgress(int percent); // of the bytes copied
    // 0 once the files are in the new folder (and the old ones deleted), else
    // a Win32 error code; ERROR_CANCELLED when it was shut down meanwhile.
    void moveFinished(int error);
    void sharesChanged(); // shares().statuses() differ
    void foldersChanged(); // folderStatuses() differ

private:
    // What a worker run reads again: everything (Full), the walked volumes
    // (Walked: their watcher lost events), or only what the index cannot be
    // brought up to date with otherwise: new volumes, journals that no
    // longer reach back far enough (Volumes; Startup loads the snapshot first).
    enum class Pass { Startup, Full, Walked, Volumes };
    enum class RuleChange { Applied, NeedsFullRead, Stopped };
    struct Followed; // an NTFS volume followed through its change journal

    // GitHub #52: Options::folders, by volume; replaced whole when they change.
    class Scope {
    public:
        explicit Scope(std::optional<std::vector<std::wstring>> folders = {}, const std::vector<std::wstring>& extra = {});
        bool set() const noexcept { return m_set; } // Options::folders has a value (maybe none)
        bool supplemental() const noexcept { return !m_set && !m_all.empty(); }
        // The chosen folders on the volume `root` ("C:"); none when it is
        // chosen whole (its root), or nothing on it is.
        const std::vector<std::wstring>& on(std::wstring_view root) const;
        bool whole(std::wstring_view root) const; // its root is chosen
        bool touches(std::wstring_view root) const; // something on it is chosen
        bool onRemovable() const; // a removable drive is touched
        const std::vector<std::wstring>& all() const noexcept { return m_all; }

    private:
        struct Volume {
            std::wstring root;
            bool whole = false;
            std::vector<std::wstring> folders;
        };
        bool m_set = false;
        std::vector<std::wstring> m_all;
        std::vector<Volume> m_volumes;
    };

    std::shared_ptr<const Crawler> crawler() const { return m_crawler.load(); }
    std::shared_ptr<const Scope> scope() const { return m_scope.load(); }
    void replaceScope(); // the worker is stopped; keeps track of removed supplemental roots
    // Only the chosen folders now (foldersOnly()), as `scope` has them.
    bool foldersOnly(const Scope& scope) const;
    void launch(Pass pass);
    void stopWorker();
    void run(std::stop_token stop, Pass pass);
    void ranOut(); // run() is done: folders owed a walk meanwhile get a pass of their own
    RuleChange applyRuleChanges(const CrawlRules& before, const Crawler& crawler, std::stop_token stop);
    // `which`: volumes read whole (walked, or from the MFT; a volume walked
    // for its chosen folders: all of them). `folders`: chosen folders walked
    // as well, and then those owed a walk meanwhile (m_verifyOwed).
    bool syncWithDisk(std::stop_token stop, const std::vector<std::size_t>& which, std::vector<std::wstring> folders = {});
    // GitHub #52: the entries of the volumes (roots, "C:") walked for their
    // chosen folders, brought in line with them: what is neither in one nor
    // on the way to one goes, the folders on the way and the chosen ones
    // come in. Returns the chosen folders to walk: those that came in, and
    // those whose last walk did not finish (or found them unreadable);
    // nothing, and nothing done, when stopped while it looks at the disk.
    std::vector<std::wstring> prepareScope(std::stop_token stop, const Scope& scope,
        const std::vector<std::wstring>& roots, const Crawler& crawler, bool prune = true,
        const std::vector<std::wstring>& renewed = {});
    bool walkFolders(std::stop_token stop, FileIndex& index, const Crawler& crawler, std::vector<std::wstring> folders,
        bool firstBuild);
    void setFolderStates(const std::vector<std::pair<std::wstring, FolderStatus::State>>& states,
        std::optional<std::int64_t> checked = {});
    void resetFolderStatuses(); // one for each chosen folder, keeping what is known of them
    void walkOwedFolders(); // GUI thread: m_verifyOwed now, or after the walk under way
    // GUI thread: chosen folders owed a walk in a moment (m_overflowed);
    // `lost`: their watcher lost events, which backs off (LostEventsBackoff).
    void resyncFoldersSoon(bool lost);
    void verifyDueFolders(); // GUI thread, every Options::folderVerifyCheckMs: those foldersDue() says
    // m_journalMutex held: the chosen folder's disk is here and not being
    // ejected (one that comes back is walked anyway).
    bool diskHere(std::wstring_view path) const;
    void noteFoldersChanged();
    // Waits (until the deadline Options::accessWaitMs set, or the stop) for
    // an access that reads the journals, when there is none and a volume
    // would lose its journal position without one (see Options).
    void awaitAccess(std::stop_token stop, const std::vector<VolumeInfo>& volumes,
        const std::vector<JournalPosition>& journals, const std::vector<bool>& known);
    void setReading(const std::wstring& root, bool reading);
    void setReadingPercent(const std::wstring& root, int percent);
    bool syncFromMft(const VolumeInfo& volume, EntryId root, const Crawler& crawler, bool lowPriority,
        std::stop_token stop);
    std::unique_ptr<Followed> follow(std::size_t volume);
    bool readJournal(Followed& f, std::stop_token stop);
    bool catchUp(Followed& f, std::stop_token stop);
    void recover(std::unique_ptr<Followed>& f, std::stop_token stop);
    void followJournals(
        std::stop_token stop, std::vector<std::unique_ptr<Followed>>& followed, std::vector<std::size_t> walkLater);
    void ensureWatcher(); // watches the volumes that are walked; never with m_journalMutex held
    void onChanges(std::vector<FsChange>&& changes);
    // Files on the volume may have changed without the journal saying so:
    // the content index checks them again.
    void contentUnsureOf(const std::wstring& root);
    std::vector<ContentIndexer::Volume> contentVolumes() const;
    static std::shared_ptr<const std::vector<std::wstring>> contentSharesOf(const ShareIndexer::Options& shares);
    void compactIfWasteful(bool always = false); // drops removed items once they add up
    // Without `wait`, a save that would have to wait for the files (being
    // moved, or saved by another thread) is left to the next one.
    void saveSnapshot(bool wait = true);
    enum class Save { Written, Skipped, Failed };
    // m_filesMutex held. `segments`: the content index's files it refers to.
    Save writeSnapshot(const QString& path, std::vector<std::uint64_t>& segments);
    std::error_code moveFiles(const QString& to, std::stop_token stop);
    void prewarmLoop(std::stop_token stop);
    void prewarm(std::stop_token stop);
    void saveInBackground();
    void setState(State state);
    void setRefreshing(bool refreshing);
    void notifyLater();

    Options m_options;
    // Never null; replaced whole by setVolumeAccess(). Each use loads it: a
    // worker thread may be in a pass while it is replaced (the pass is
    // stopped right after, and planned again).
    std::atomic<std::shared_ptr<ntfs::VolumeAccess>> m_access;
    std::chrono::steady_clock::time_point m_accessDeadline {}; // until when awaitAccess() waits (start())
    std::mutex m_accessMutex; // guards the wait below against a lost wake-up
    std::condition_variable_any m_accessCv;
    std::atomic<bool> m_accessAwaited {false}; // the worker is in awaitAccess(): told, not relaunched
    std::atomic<std::shared_ptr<const Crawler>> m_crawler; // replaced whole when the rules change
    std::atomic<std::shared_ptr<const Scope>> m_scope; // ... when the chosen folders do
    std::atomic<std::shared_ptr<FileIndex>> m_index;
    std::atomic<State> m_state {State::Idle};
    std::atomic<bool> m_refreshing {false};
    // Which volumes are followed is known (contentVolumes): from a little
    // after a snapshot is loaded, when the index is Ready already.
    std::atomic<bool> m_volumesKnown {false};
    std::atomic<bool> m_dirty {false};
    // Worker thread only: the chosen folders whose entries prepareScope() made
    // (or found) this pass. One of them walkFolders() cannot find is not
    // looked for again (GitHub #52 review).
    std::vector<std::wstring> m_placed;

    // While a sync reads the disk, watcher changes are journaled and replayed
    // afterwards, so the final index reflects the latest state. Whatever
    // holds entry ids across write locks (syncs, applying changes) holds this
    // mutex or has m_journaling set, which keeps compaction out.
    mutable std::mutex m_journalMutex; // guards the members below
    bool m_journaling = false;
    std::vector<FsChange> m_journal;
    bool m_resyncAfter = false;
    std::vector<VolumeInfo> m_volumes; // indexed volumes, suspended ones included
    std::vector<JournalPosition> m_journals; // parallel to m_volumes; journalId 0: walked and watched
    std::vector<std::wstring> m_suspended; // being ejected or locked: not touched at all
    std::vector<std::wstring> m_rewalk; // walked volumes owed a walk: never finished, or unwatched for a while
    std::optional<CrawlRules> m_rulesBefore; // the rules the index still reflects, when they changed
    // GitHub #52: volumes whose entries are their chosen folders' only (walked
    // whole again once they are not chosen any more); kept in scope.json.
    std::vector<std::wstring> m_scopedVolumes;
    std::vector<std::wstring> m_removedExtra; // descendants to discard when an explicit link is removed
    std::vector<std::wstring> m_verifyOwed; // chosen folders owed a walk as soon as can be
    std::vector<std::wstring> m_overflowed; // ... whose watcher lost events: walked a moment later (resyncFoldersSoon)

    mutable std::mutex m_readingMutex;
    std::vector<Reading> m_reading; // readingVolumes()

    std::shared_ptr<ContentIndex> m_content;
    // The shares whose contents are indexed (ShareConfig::content): for
    // contentVolumes(), on the indexer's and searches' threads.
    std::atomic<std::shared_ptr<const std::vector<std::wstring>>> m_contentShares;
    std::unique_ptr<ContentIndexer> m_contentIndexer;
    std::unique_ptr<ShareIndexer> m_shares;
    std::unique_ptr<remotecontent::Indexer> m_shareContent; // after m_shares: its threads ask it
    std::atomic<bool> m_compactOwed {false}; // put off while ids were pinned

    // The files on disk: saving, loading and moving them, and where they are.
    std::mutex m_filesMutex;
    QString m_folder; // guarded by m_filesMutex
    std::atomic<bool> m_moving {false};
    std::stop_source m_shutdown;
    std::unique_ptr<ChangeWatcher> m_watcher; // worker thread, or GUI thread while no worker runs
    // What m_watcher was last asked to watch (ensureWatcher), like it.
    std::vector<std::wstring> m_watchRoots;
    std::vector<ChangeWatcher::Fallback> m_watchInstead;
    std::jthread m_worker;
    std::jthread m_mover; // moveTo
    std::mutex m_prewarmMutex;
    std::condition_variable_any m_prewarmCv;
    std::optional<std::chrono::steady_clock::time_point> m_prewarmAt; // asked for, not started yet
    std::jthread m_prewarmer; // from start() on
    std::future<void> m_pendingSave;
    QTimer m_saveTimer; // checks whether a save is due
    std::atomic<std::chrono::steady_clock::time_point> m_lastSave {std::chrono::steady_clock::now()};
    std::atomic<std::uint64_t> m_garbageAfterSave {0}; // ContentIndex::obsoleteBytes() after the last save
    QTimer m_resyncTimer;
    QTimer m_volumesTimer;
    QTimer m_folderResyncTimer; // resyncFoldersSoon
    LostEventsBackoff m_lostBackoff; // GUI thread
    QTimer m_verifyTimer; // verifyDueFolders
    std::atomic<bool> m_running {false}; // a worker pass is under way (from launch() to ranOut())
    mutable std::mutex m_foldersMutex;
    std::vector<FolderStatus> m_folderStatus; // folderStatuses(); guarded by m_foldersMutex
    bool m_stopped = false;
};

// GitHub #52: the chosen folders to walk again now, as they stand
// (folderVerifyDue; `away` is asked only when it matters). Those walked
// before: ready, not found, or unreadable; not those waiting for a walk or
// in one, left out of searches, on a disk not there, or taken whole (the
// disk is walked as before: at startup, and when its watcher lost events).
std::vector<std::wstring> foldersDue(const std::vector<IndexService::FolderStatus>& statuses, std::chrono::hours every,
    std::int64_t now, const std::function<bool()>& away);

} // namespace ws
