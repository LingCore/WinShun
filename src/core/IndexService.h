#pragma once

#include "ChangeWatcher.h"
#include "Crawler.h"
#include "FileIndex.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace qf {

// Owns the file index and keeps it current:
//   1. startup: load the snapshot (searchable within a second)
//   2. NTFS volumes: catch up with each volume's change journal from where
//      the snapshot left off, then follow it live. When the journal no
//      longer reaches back that far (or on the first run), read the MFT.
//   3. other volumes (exFAT, FAT, ReFS): watch them with ReadDirectoryChangesW
//      and, after a short delay, walk them in place at low I/O priority to
//      pick up changes made while the app was not running
//   4. periodically and on exit: save a compact snapshot
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
    };

    explicit IndexService(Options options, QObject* parent = nullptr);
    ~IndexService() override;

    void start();
    void rebuild(); // reads every volume again
    void setOptions(Options options); // re-syncs when rules or drives change
    void shutdown(); // stops threads, saves the snapshot; idempotent

    std::shared_ptr<FileIndex> index() const { return m_index.load(); }
    State state() const noexcept { return m_state.load(); }
    bool isRefreshing() const noexcept { return m_refreshing.load(); }
    std::size_t itemCount() const;

signals:
    void stateChanged();

private:
    // What a worker run reads again: everything (Full), the walked volumes
    // (Walked: their watcher lost events), or at Startup only what the
    // snapshot and the change journals cannot bring up to date.
    enum class Pass { Startup, Full, Walked };
    struct Followed; // an NTFS volume followed through its change journal

    void launch(Pass pass);
    void run(std::stop_token stop, Pass pass);
    bool syncWithDisk(std::stop_token stop, const std::vector<std::size_t>& which);
    bool syncFromMft(const VolumeInfo& volume, EntryId root, bool lowPriority, std::stop_token stop);
    std::unique_ptr<Followed> follow(std::size_t volume);
    bool readJournal(Followed& f, std::stop_token stop);
    bool catchUp(Followed& f, std::stop_token stop);
    void recover(std::unique_ptr<Followed>& f, std::stop_token stop);
    void followJournals(
        std::stop_token stop, std::vector<std::unique_ptr<Followed>>& followed, std::vector<std::size_t> walkLater);
    void ensureWatcher(); // watches the volumes that are walked
    void onChanges(std::vector<FsChange>&& changes);
    void applyChanges(FileIndex& index, const std::vector<FsChange>& changes);
    void compactIfWasteful(bool always = false); // drops removed items once they add up
    void saveSnapshot();
    void saveInBackground();
    void setState(State state);
    void setRefreshing(bool refreshing);
    void notifyLater();

    Options m_options;
    std::unique_ptr<Crawler> m_crawler;
    std::atomic<std::shared_ptr<FileIndex>> m_index;
    std::atomic<State> m_state {State::Idle};
    std::atomic<bool> m_refreshing {false};
    std::atomic<bool> m_dirty {false};

    // While a sync reads the disk, watcher changes are journaled and replayed
    // afterwards, so the final index reflects the latest state. Whatever
    // holds entry ids across write locks (syncs, applying changes) holds this
    // mutex or has m_journaling set, which keeps compaction out.
    std::mutex m_journalMutex; // guards the members below
    bool m_journaling = false;
    std::vector<FsChange> m_journal;
    bool m_resyncAfter = false;
    std::vector<VolumeInfo> m_volumes;
    std::vector<JournalPosition> m_journals; // parallel to m_volumes; journalId 0: walked and watched

    std::mutex m_saveMutex;
    std::stop_source m_shutdown;
    std::unique_ptr<ChangeWatcher> m_watcher; // worker thread, or GUI thread while no worker runs
    std::jthread m_worker;
    std::future<void> m_pendingSave;
    QTimer m_saveTimer;
    QTimer m_resyncTimer;
    bool m_stopped = false;
};

} // namespace qf
