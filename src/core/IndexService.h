#pragma once

#include "ChangeWatcher.h"
#include "ContentIndex.h"
#include "ContentIndexer.h"
#include "Crawler.h"
#include "FileIndex.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
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
//
// The first run has no snapshot: the (empty) index is searchable at once and
// fills up while the volumes are read. There is only ever one index in memory.
class IndexService : public QObject {
    Q_OBJECT

public:
    enum class State { Idle, Loading, Building, Ready };
    Q_ENUM(State)

    struct Options {
        CrawlRules rules;
        QString snapshotPath;
        bool includeRemovable = false;
        bool rescanOnStartup = true; // walked volumes only: NTFS always catches up
        int rescanDelayMs = 15000;
        ContentIndexer::Options content;
    };

    explicit IndexService(Options options, QObject* parent = nullptr);
    ~IndexService() override;

    void start();
    void rebuild(); // reads every volume again
    // New rules are applied to the index in place: newly excluded folders are
    // dropped, folders no longer excluded are walked. Only a folder name that
    // is no longer excluded (it could be anywhere) takes reading every volume.
    void setOptions(Options options);
    void shutdown(); // stops threads, saves the snapshot; idempotent

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

private:
    // What a worker run reads again: everything (Full), the walked volumes
    // (Walked: their watcher lost events), or only what the index cannot be
    // brought up to date with otherwise: new volumes, journals that no
    // longer reach back far enough (Volumes; Startup loads the snapshot first).
    enum class Pass { Startup, Full, Walked, Volumes };
    enum class RuleChange { Applied, NeedsFullRead, Stopped };
    struct Followed; // an NTFS volume followed through its change journal

    std::shared_ptr<const Crawler> crawler() const { return m_crawler.load(); }
    void launch(Pass pass);
    void stopWorker();
    void run(std::stop_token stop, Pass pass);
    RuleChange applyRuleChanges(const CrawlRules& before, const Crawler& crawler, std::stop_token stop);
    bool syncWithDisk(std::stop_token stop, const std::vector<std::size_t>& which);
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
    void applyChanges(FileIndex& index, const std::vector<FsChange>& changes, const Crawler& crawler);
    // Files on the volume may have changed without the journal saying so:
    // the content index checks them again.
    void contentUnsureOf(const std::wstring& root);
    std::vector<ContentIndexer::Volume> contentVolumes() const;
    void compactIfWasteful(bool always = false); // drops removed items once they add up
    void saveSnapshot();
    void saveInBackground();
    void setState(State state);
    void setRefreshing(bool refreshing);
    void notifyLater();

    Options m_options;
    std::atomic<std::shared_ptr<const Crawler>> m_crawler; // replaced whole when the rules change
    std::atomic<std::shared_ptr<FileIndex>> m_index;
    std::atomic<State> m_state {State::Idle};
    std::atomic<bool> m_refreshing {false};
    std::atomic<bool> m_dirty {false};

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

    mutable std::mutex m_readingMutex;
    std::vector<Reading> m_reading; // readingVolumes()

    std::shared_ptr<ContentIndex> m_content;
    std::unique_ptr<ContentIndexer> m_contentIndexer;
    std::atomic<bool> m_compactOwed {false}; // put off while ids were pinned

    std::mutex m_saveMutex;
    std::stop_source m_shutdown;
    std::unique_ptr<ChangeWatcher> m_watcher; // worker thread, or GUI thread while no worker runs
    std::jthread m_worker;
    std::future<void> m_pendingSave;
    QTimer m_saveTimer;
    QTimer m_resyncTimer;
    QTimer m_volumesTimer;
    bool m_stopped = false;
};

} // namespace ws
