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
#include <optional>
#include <utility>
#include <vector>

namespace ws {

// The 运行状况 page's readings: what Win顺 uses now (its memory told apart by
// what it holds, CPU, reads), each of its processes, and what it did
// (loading, building, content rounds, big documents, give-backs: the events).
// In memory only.
//
// While the page is on screen (watching): every second the whole process
// list (~20 ms of a pool thread with 950 processes), and every third second
// this process's memory page by page (memory::map, ~20 ms, also off the GUI
// thread; the heaps walked once a minute and after a give-back).
// Otherwise only what the events need: every 10 seconds this process and its
// own read again by handle (microseconds); the whole list when something
// begins or ends, and a minute apart while a content round reads (its
// document readers run under the service).
class HealthMonitor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made by the app")
    Q_PROPERTY(bool watching READ watching WRITE setWatching NOTIFY watchingChanged FINAL)
    Q_PROPERTY(QVariantMap now READ now NOTIFY updated FINAL)
    Q_PROPERTY(QVariantList recent READ recent NOTIFY updated FINAL)

public:
    using Blocks = std::vector<std::pair<const void*, std::size_t>>;
    // What the app tells it; each may be left unset.
    struct Sources {
        std::function<health::Facts()> facts;
        // The file index's blocks (address, size) and its tables' bytes on the
        // heap, under its lock; false: not now (a writer has it).
        std::function<bool(Blocks& blocks, std::uint64_t& tableBytes)> fileIndex;
        std::function<std::uint64_t()> cachedPictures; // thumbnails kept, bytes; any thread
        std::function<QVariantList()> tasks; // BackgroundStatus::toVariant
        std::function<std::pair<qint64, qint64>()> lastTrim; // ms since 1970 (0: none), MB
    };

    explicit HealthMonitor(Sources sources, QObject* parent = nullptr);
    // A monitor with made-up readings (the previews); never reads.
    static HealthMonitor* demo(QObject* parent, bool idle);

    bool watching() const noexcept { return m_watching; }
    void setWatching(bool on);
    QVariantMap now() const { return m_now; }
    QVariantList recent() const;

    // The settings' 〔现在就归还〕 gave back `megabytes` (IdleTrim noted it too).
    void noteGiveBack(qint64 megabytes, qint64 atMs);
    // Something began or ended: looked at now, the whole list.
    void poke();

    const health::Events& events() const noexcept { return m_events; }
    static QString eventTitle(const health::Event& e);
    static QString eventDetail(const health::Event& e);

signals:
    void watchingChanged();
    void updated();

private:
    // One reading: memory as Task Manager counts it, bytes.
    struct Reading {
        std::uint64_t main = 0, plugins = 0, documents = 0, service = 0;
        std::uint64_t workingSet = 0, privateCommit = 0;
        std::uint32_t biggestDocumentKb = 0; // the largest WinShunExtract, the service's too
        double cpu = 0, serviceCpu = 0; // percent of the whole machine, over the last second
        double read = 0, serviceRead = 0; // MB/s
        std::uint32_t totalKb() const noexcept { return static_cast<std::uint32_t>((main + plugins + documents) / 1024); }
    };
    struct Totals { // one process's counters at the last reading
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
    struct Mapped { // the main process page by page, and what goes with it
        memory::Map map;
        std::uint64_t fileIndexTables = 0;
        std::uint64_t pictures = 0;
    };

    void take(bool fine, bool whole = false); // a reading off the GUI thread, then apply()
    void apply(const memory::Look& look, bool fine, std::optional<Mapped> mapped);
    void publish(); // `now` for the page
    Group since(const Baseline& baseline, const std::vector<const memory::Process*>& procs) const;
    static void rebase(Baseline& baseline, const std::vector<const memory::Process*>& procs, std::int64_t nowNs);

    Sources m_sources;
    bool m_demo = false;
    bool m_watching = false;
    QTimer m_coarse; // every 10 s: the events
    QTimer m_fine; // every second while watching
    QTimer m_soon; // poke(): a look in a moment
    static constexpr int kCoarseMs = 10'000, kFineMs = 1'000;
    static constexpr int kCoarse = 1, kFine = 2, kWhole = 4;
    // While a content round reads: the whole list this often (its readers).
    static constexpr std::int64_t kWholeMs = 60'000;
    int m_wanted = 0; // the readings the look under way is for (and kWhole)
    bool m_looking = false;
    int m_fineCount = 0; // memory::map every third second
    std::size_t m_sizeHint = 0; // memory::look's

    std::vector<memory::Process> m_known; // the family at the last whole look
    std::int64_t m_lastWholeMs = 0;
    bool m_contentReading = false; // the last facts'
    Reading m_reading;
    Reading m_lastService; // the service as last seen (a whole look)
    bool m_haveReading = false;
    std::optional<Mapped> m_mapped;
    std::uint64_t m_fileIndexHeld = 0; // its blocks' bytes, in memory or parked
    Baseline m_fineBase, m_fineServiceBase;
    unsigned m_processors = 1;
    health::Events m_events;
    health::EventTracker m_tracker {m_events};
    qint64 m_lastTrimSeen = 0;
    QVariantMap m_now;
    QVariantList m_processes; // the newest look's, for `now`
};

} // namespace ws
