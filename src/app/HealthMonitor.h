#pragma once

#include "platform/ProcessUsage.h"

#include <QJsonObject>
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
// what it holds, CPU, reads), each of its processes, and its current tasks.
//
// While the page is on screen (watching): every second the whole process
// list (~20 ms of a pool thread with 950 processes), and every third second
// this process's memory page by page (memory::map, ~20 ms, also off the GUI
// thread). No sampling while the page is hidden.
class HealthMonitor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made by the app")
    Q_PROPERTY(bool watching READ watching WRITE setWatching NOTIFY watchingChanged FINAL)
    Q_PROPERTY(QVariantMap now READ now NOTIFY updated FINAL)

public:
    using Blocks = std::vector<std::pair<const void*, std::size_t>>;
    // What the app tells it; each may be left unset.
    struct Sources {
        // The file index's blocks (address, size) and its tables' bytes on the
        // heap, under its lock; false: not now (a writer has it).
        std::function<bool(Blocks& blocks, std::uint64_t& tableBytes)> fileIndex;
        std::function<std::uint64_t()> cachedPictures; // thumbnails kept, bytes; any thread
        // The pages that stay when the memory is given back (HotPages::kept),
        // as blocks: addresses only, never read. Asked on the GUI thread.
        std::function<Blocks()> kept;
        std::function<QVariantList()> tasks; // BackgroundStatus::toVariant
        std::function<std::pair<qint64, qint64>()> lastTrim; // ms since 1970 (0: none), MB
        // Windows' memory in use, in percent (IdleTrim::memoryLoad): short of
        // it, idle memory is held, and the page says so. Null: not told.
        std::function<int()> memoryLoad;
    };

    explicit HealthMonitor(Sources sources, QObject* parent = nullptr);
    // A monitor with made-up readings (the previews); never reads. With
    // `memoryLoad` (0 to 100), Windows' memory as that much in use.
    static HealthMonitor* demo(QObject* parent, bool idle, int memoryLoad = -1);

    // The page's breakdown read once, for the diagnostics package (MB, one
    // decimal; program names, nothing else): each process of Win顺's, the
    // main one told apart as on the page and by kind of page. `fileIndex`:
    // its blocks, taken under its lock; `fileIndexTables`, `pictures`: bytes;
    // `kept`: Sources::kept's blocks.
    // Any thread, ~40 ms (memory::look and memory::map).
    static QJsonObject snapshot(Blocks fileIndex, std::uint64_t fileIndexTables, std::uint64_t pictures, const Blocks& kept = {});

    bool watching() const noexcept { return m_watching; }
    void setWatching(bool on);
    QVariantMap now() const { return m_now; }

    // Refresh the memory breakdown after 〔现在就归还〕, if the page is shown.
    void refreshMemory();
    // Something began or ended: refresh while the page is shown.
    void poke();

signals:
    void watchingChanged();
    void updated();

private:
    // One reading: memory as Task Manager counts it, bytes.
    struct Reading {
        std::uint64_t main = 0, plugins = 0, documents = 0, service = 0;
        std::uint64_t workingSet = 0, privateCommit = 0;
        double cpu = 0, serviceCpu = 0; // percent of the whole machine, over the last second
        double read = 0, serviceRead = 0; // MB/s
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
        std::uint64_t kept = 0; // of Sources::kept, what is in memory now
    };

    void take(); // a reading off the GUI thread, then apply()
    void apply(const memory::Look& look, std::optional<Mapped> mapped);
    void publish(); // `now` for the page
    Group since(const Baseline& baseline, const std::vector<const memory::Process*>& procs) const;
    static void rebase(Baseline& baseline, const std::vector<const memory::Process*>& procs, std::int64_t nowNs);

    Sources m_sources;
    bool m_demo = false;
    bool m_watching = false;
    QTimer m_fine; // every second while watching
    QTimer m_soon; // poke(): a look in a moment
    static constexpr int kFineMs = 1'000;
    bool m_pending = false; // another reading was requested while one was under way
    bool m_looking = false;
    int m_fineCount = 0; // memory::map every third second
    std::size_t m_sizeHint = 0; // memory::look's

    Reading m_reading;
    bool m_haveReading = false;
    std::optional<Mapped> m_mapped;
    std::uint64_t m_fileIndexHeld = 0; // its blocks' bytes, in memory or parked
    Baseline m_fineBase, m_fineServiceBase;
    unsigned m_processors = 1;
    QVariantMap m_now;
    QVariantList m_processes; // the newest look's, for `now`
};

} // namespace ws
