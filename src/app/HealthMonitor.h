#pragma once

#include "HealthHistory.h"
#include "platform/ProcessUsage.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ws {

// The 运行状况 page's readings: what Win顺 uses (memory by part, CPU, reads),
// every 10 seconds since it started, every second while the page is on
// screen (watching), and what happened (loading, building, content rounds,
// big documents, give-backs) to tell the peaks by. In memory only.
// A look: the process list off the GUI thread (~20 ms of a pool thread with
// 950 processes, its 2.6 MB given back at once), the file index's pages on
// it (~1.3 ms for 128 MB); the rings ~400 KB when full.
class HealthMonitor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made by the app")
    Q_PROPERTY(bool watching READ watching WRITE setWatching NOTIFY watchingChanged FINAL)
    Q_PROPERTY(QVariantMap now READ now NOTIFY updated FINAL)
    Q_PROPERTY(QVariantList recent READ recent NOTIFY updated FINAL)
    Q_PROPERTY(int revision READ revision NOTIFY updated FINAL)

public:
    // What the app tells it; each may be left unset.
    struct Sources {
        std::function<health::Facts()> facts;
        // The file index's blocks (address, size), under its lock; false: not
        // now (the lock is taken by a writer): the last reading stands.
        std::function<bool(std::vector<std::pair<const void*, std::size_t>>&)> fileIndexBlocks;
        std::function<QVariantList()> tasks; // BackgroundStatus::toVariant
        std::function<std::pair<qint64, qint64>()> lastTrim; // ms since 1970 (0: none), MB
    };

    explicit HealthMonitor(Sources sources, QObject* parent = nullptr);
    // A monitor with made-up readings (the previews): 24 hours with a first
    // index, content rounds, a big document and give-backs; never samples.
    static HealthMonitor* demo(QObject* parent, bool idle);

    bool watching() const noexcept { return m_watching; }
    void setWatching(bool on);
    QVariantMap now() const { return m_now; }
    QVariantList recent() const;
    int revision() const noexcept { return m_revision; }

    // The settings' 〔现在就归还〕 gave back `megabytes` (IdleTrim noted it too).
    void noteGiveBack(qint64 megabytes, qint64 atMs);
    // Something began or ended: looked at now, not at the next reading.
    void poke();

    const health::History& history() const noexcept { return m_history; }
    const health::Events& events() const noexcept { return m_events; }
    // What was going on at `ms` (the charts' hover): "读文件内容，41,382 个文件 · …", or "".
    Q_INVOKABLE QString eventsAt(qint64 ms) const;
    // For the charts: the events' words.
    static QString eventTitle(const health::Event& e);
    static QString eventDetail(const health::Event& e);

signals:
    void watchingChanged();
    void updated();

private:
    struct Totals { // one process's counters at the last reading of a ring
        std::uint32_t pid;
        std::int64_t created;
        std::uint64_t cpu, read;
    };
    struct Baseline {
        std::vector<Totals> totals;
        std::int64_t atNs = 0; // FILETIME (100 ns) of the reading; 0: none yet
    };
    struct Group { // a family's use since its baseline
        std::uint64_t cpu = 0, read = 0;
    };
    void take(bool fine, bool whole = false); // a reading off the GUI thread, then apply()
    void apply(const memory::Look& look, bool fine);
    void publish(); // `now` for the page
    Group since(const Baseline& baseline, const std::vector<const memory::Process*>& procs) const;
    static void rebase(Baseline& baseline, const std::vector<const memory::Process*>& procs, std::int64_t nowNs);

    Sources m_sources;
    bool m_demo = false;
    bool m_watching = false;
    QTimer m_coarse;
    QTimer m_fine;
    QTimer m_soon; // poke(): a look in a moment
    std::int64_t m_lastCoarseMs = 0;
    static constexpr int kCoarse = 1, kFine = 2, kWhole = 4;
    // Between whole looks (a minute apart unless watching or poked) only this
    // one and its own are read again, by handle; the service stands as last seen.
    static constexpr std::int64_t kWholeMs = 60'000;
    int m_wanted = 0; // the readings the look under way is for (and kWhole)
    bool m_looking = false;
    std::size_t m_sizeHint = 0; // memory::look's

    std::vector<std::pair<const void*, std::size_t>> m_blocks;
    std::uint64_t m_fileIndexBytes = 0;
    std::uint64_t m_fileIndexHeld = 0;
    std::vector<memory::Process> m_known; // the family at the last whole look
    std::int64_t m_lastWholeMs = 0;
    struct ServiceUse {
        std::uint32_t kb = 0, readKbps = 0, documentKb = 0;
        std::uint16_t cpu = 0;
    } m_service; // the last whole coarse reading's, carried until the next
    Baseline m_coarseBase, m_coarseServiceBase;
    Baseline m_fineBase, m_fineServiceBase;
    unsigned m_processors = 1;
    health::History m_history;
    health::Events m_events;
    health::EventTracker m_tracker {m_events};
    qint64 m_lastTrimSeen = 0;
    QVariantMap m_now;
    QVariantList m_processes; // the newest look's, for `now`
    int m_revision = 0;
};

} // namespace ws
