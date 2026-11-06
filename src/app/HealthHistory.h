#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ws::health {

// What Win顺 did and when (the 运行状况 page's 最近的事): in memory only,
// kept while it runs, the last kCapacity of them.
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
