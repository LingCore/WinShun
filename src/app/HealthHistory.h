#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ws::health {

// The parts Win顺's memory (Task Manager's Memory column, Win顺's row) is told
// apart into on the 运行状况 page; together they are the total, never more.
// Only what can be measured is a part of its own: the file index (its blocks'
// pages in the working set), the plugins' processes and the document readers
// (WinShunExtract). The rest of the main process (the interface, the
// clipboard history, the content index's working buffers: heap that cannot
// be told apart) is "界面和其他", the difference.
enum Part : std::uint8_t { FileIndex, Other, Plugins, Documents, kParts };

// One reading, 40 bytes. Memory in KB; CPU in hundredths of a percent of the
// whole machine (all logical processors, as Task Manager), 0..10000; reads
// in KB a second, over the time since the reading before.
// Trivial on purpose: the rings are allocated without being written to, so
// a slot's page is touched only once a reading lands there.
struct Sample {
    std::int64_t atMs; // wall clock, ms since 1970
    std::array<std::uint32_t, kParts> partKb;
    std::uint32_t serviceKb; // WinShunService and what it started: not in the total
    std::uint16_t cpu; // Win顺 (its row in Task Manager)
    std::uint16_t serviceCpu;
    std::uint32_t readKbps;
    std::uint32_t serviceReadKbps;

    std::uint32_t totalKb() const noexcept;
};

// Fixed-size ring, oldest dropped. Its storage is reserved when made and
// filled as readings come; never reallocated.
class Ring {
public:
    explicit Ring(std::size_t capacity);
    void push(const Sample& sample) noexcept;
    void clear() noexcept { m_size = 0; m_head = 0; }
    std::size_t size() const noexcept { return m_size; }
    std::size_t capacity() const noexcept { return m_capacity; }
    const Sample& at(std::size_t i) const noexcept; // 0: the oldest
    const Sample& back() const noexcept { return at(m_size - 1); }

private:
    std::unique_ptr<Sample[]> m_data;
    std::size_t m_capacity;
    std::size_t m_head = 0; // where the next goes
    std::size_t m_size = 0;
};

// What a chart is drawn from: a range cut into buckets of equal time; each
// bucket that had readings becomes one point. Memory takes the reading with
// the highest total in its bucket (a stack of parts that belong together, its
// peak kept); CPU and reads take the highest, so a spike thinner than a
// bucket still shows, and the mean. A point after a stretch without readings
// (asleep, Win顺 not running) starts a new line (breakBefore).
struct Series {
    std::int64_t fromMs = 0;
    std::int64_t toMs = 0;
    std::vector<double> t; // ms since 1970: the bucket's peak reading
    std::vector<std::uint8_t> breakBefore;
    std::array<std::vector<double>, kParts> partMb;
    std::vector<double> totalMb; // the same reading's sum
    std::vector<double> serviceMb;
    std::vector<double> cpuMax, cpuMean, serviceCpuMax; // percent
    std::vector<double> readMax, readMean, serviceReadMax; // MB/s
    double peakTotalMb = 0, peakServiceMb = 0, peakCpu = 0, peakRead = 0; // over the range: the axes' tops
    std::size_t size() const noexcept { return t.size(); }
};

// Win顺's readings since it started, in memory only (never written to disk):
// every 10 seconds for 24 hours (8,640 × 40 B ≈ 340 KB), and every second
// for the last 10 minutes while the 运行状况 page is on screen.
class History {
public:
    static constexpr std::int64_t kCoarseMs = 10'000;
    static constexpr std::int64_t kFineMs = 1'000;
    static constexpr std::size_t kCoarseCount = 24 * 3600 * 1000 / kCoarseMs;
    static constexpr std::size_t kFineCount = 10 * 60 * 1000 / kFineMs;
    // Readings further apart than this have a hole between them.
    static constexpr std::int64_t kGapMs = 3 * kCoarseMs + kCoarseMs / 2;
    static constexpr std::int64_t kFineGapMs = 3 * kFineMs + kFineMs / 2;

    History();
    void addCoarse(const Sample& sample) noexcept { m_coarse.push(sample); }
    void addFine(const Sample& sample) noexcept { m_fine.push(sample); }
    // The page is gone: its fine readings with it (the 10-second ones stay).
    void clearFine() noexcept { m_fine.clear(); }
    void clear() noexcept;

    const Ring& coarse() const noexcept { return m_coarse; }
    const Ring& fine() const noexcept { return m_fine; }
    std::size_t bytes() const noexcept; // what the rings take, full

    // [fromMs, toMs] in at most `buckets` buckets: the 10-second readings,
    // the fine ones from where they begin.
    Series series(std::int64_t fromMs, std::int64_t toMs, int buckets) const;

private:
    Ring m_coarse;
    Ring m_fine;
};

// What happened, for the time axis and 最近的事: kept while Win顺 runs, the
// last kCapacity of them.
enum class EventKind : std::uint8_t {
    IndexLoad, // the file index read from the disk
    IndexBuild, // built for the first time
    IndexRebuild, // on request (the tray's 重建索引)
    ContentRound, // the content index read files
    BigDocument, // a document reader grew past kBigDocumentKb
    IndexSave,
    IndexMove,
    AutoTrim, // memory given back while idle
    ManualTrim, // 〔现在就归还〕
};

struct Event {
    int id = 0;
    EventKind kind = EventKind::IndexLoad;
    std::int64_t startMs = 0; // wall clock
    std::int64_t endMs = 0; // 0: still going (an instant one: == startMs)
    std::uint32_t peakKb = 0; // Win顺's total at its highest while it went on
    std::uint32_t freedMb = 0; // a give-back's
    std::uint64_t count = 0; // files read (ContentRound), the reader's peak KB (BigDocument)
    bool open() const noexcept { return endMs == 0; }
    bool instant() const noexcept { return kind == EventKind::AutoTrim || kind == EventKind::ManualTrim; }
};

class Events {
public:
    static constexpr std::size_t kCapacity = 200;
    // A document reader this big is a big document (the queue for them: ContentIndexer::largeDocumentBytes).
    static constexpr std::uint32_t kBigDocumentKb = 200 * 1024;
    // Shorter than this, a save is not worth a line (it is most of them).
    static constexpr std::int64_t kShortSaveMs = 1'000;

    int begin(EventKind kind, std::int64_t nowMs, std::uint32_t totalKb);
    // Ended. A content round that read nothing, a save under kShortSaveMs: forgotten.
    void end(int id, std::int64_t nowMs);
    void setCount(int id, std::uint64_t count);
    void instant(EventKind kind, std::int64_t nowMs, std::uint32_t totalKb, std::uint32_t freedMb);
    // Each reading: the open ones' peaks.
    void notePeak(std::uint32_t totalKb) noexcept;
    // Those that overlap [fromMs, toMs], oldest first.
    std::vector<Event> between(std::int64_t fromMs, std::int64_t toMs) const;
    // The newest first, at most `most`.
    std::vector<Event> recent(std::size_t most) const;
    const Event* find(int id) const;
    std::size_t size() const noexcept { return m_events.size(); }
    void clear() noexcept { m_events.clear(); }

private:
    Event* findMutable(int id);
    std::vector<Event> m_events; // oldest first (by start)
    int m_nextId = 1;
};

// What the background is doing, as seen at each look; its changes are the
// events (EventTracker).
struct Facts {
    bool loading = false; // the file index read from the disk
    bool building = false; // built for the first time
    bool rebuilding = false; // on request (the tray's 重建索引)
    bool contentReading = false;
    std::uint64_t contentDone = 0;
    bool saving = false;
    bool moving = false;
    std::uint32_t biggestDocumentKb = 0; // the largest WinShunExtract now
};

// Turns the Facts seen one after another into begun and ended events.
class EventTracker {
public:
    explicit EventTracker(Events& events) : m_events(events) {}
    void observe(const Facts& facts, std::int64_t nowMs, std::uint32_t totalKb);

private:
    void track(int& id, bool on, EventKind kind, std::int64_t nowMs, std::uint32_t totalKb);
    Events& m_events;
    int m_load = 0, m_build = 0, m_rebuild = 0, m_content = 0, m_save = 0, m_move = 0, m_document = 0;
    std::uint64_t m_contentMost = 0;
    std::uint32_t m_documentPeakKb = 0;
};

} // namespace ws::health
