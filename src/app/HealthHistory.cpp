#include "HealthHistory.h"

#include <algorithm>
#include <limits>

namespace ws::health {

int Events::begin(EventKind kind, std::int64_t nowMs, std::uint32_t totalKb)
{
    if (m_events.size() >= kCapacity)
        m_events.erase(m_events.begin());
    Event e;
    e.id = m_nextId++;
    e.kind = kind;
    e.startMs = nowMs;
    e.peakKb = totalKb;
    m_events.push_back(e);
    return e.id;
}

void Events::end(int id, std::int64_t nowMs)
{
    const auto it = std::find_if(m_events.begin(), m_events.end(), [id](const Event& e) { return e.id == id; });
    if (it == m_events.end())
        return;
    it->endMs = std::max(nowMs, it->startMs + 1); // never 0, never "still going"
    const bool idle = (it->kind == EventKind::ContentRound && it->count == 0)
        || (it->kind == EventKind::IndexSave && it->endMs - it->startMs < kShortSaveMs);
    if (idle)
        m_events.erase(it);
}

void Events::setCount(int id, std::uint64_t count)
{
    if (Event* e = findMutable(id))
        e->count = count;
}

void Events::instant(EventKind kind, std::int64_t nowMs, std::uint32_t totalKb, std::uint32_t freedMb)
{
    const int id = begin(kind, nowMs, totalKb);
    Event* e = findMutable(id);
    e->endMs = nowMs;
    e->freedMb = freedMb;
}

void Events::notePeak(std::uint32_t totalKb) noexcept
{
    for (Event& e : m_events) {
        if (e.open())
            e.peakKb = std::max(e.peakKb, totalKb);
    }
}

std::vector<Event> Events::recent(std::size_t most) const
{
    std::vector<Event> out;
    for (auto it = m_events.rbegin(); it != m_events.rend() && out.size() < most; ++it)
        out.push_back(*it);
    return out;
}

const Event* Events::find(int id) const
{
    const auto it = std::find_if(m_events.begin(), m_events.end(), [id](const Event& e) { return e.id == id; });
    return it == m_events.end() ? nullptr : &*it;
}

Event* Events::findMutable(int id)
{
    return const_cast<Event*>(find(id));
}

void EventTracker::track(int& id, bool on, EventKind kind, std::int64_t nowMs, std::uint32_t totalKb)
{
    if (on && id == 0) {
        id = m_events.begin(kind, nowMs, totalKb);
    } else if (!on && id != 0) {
        m_events.end(id, nowMs);
        id = 0;
    }
}

void EventTracker::observe(const Facts& facts, std::int64_t nowMs, std::uint32_t totalKb)
{
    m_events.notePeak(totalKb);
    track(m_load, facts.loading, EventKind::IndexLoad, nowMs, totalKb);
    track(m_build, facts.building, EventKind::IndexBuild, nowMs, totalKb);
    track(m_rebuild, facts.rebuilding, EventKind::IndexRebuild, nowMs, totalKb);
    // Files read: the most seen while it went on (the count may start over at its end).
    if (facts.contentReading)
        m_contentMost = m_content == 0 ? facts.contentDone : std::max(m_contentMost, facts.contentDone);
    if (m_content != 0)
        m_events.setCount(m_content, m_contentMost);
    track(m_content, facts.contentReading, EventKind::ContentRound, nowMs, totalKb);
    if (m_content != 0)
        m_events.setCount(m_content, m_contentMost);
    track(m_save, facts.saving, EventKind::IndexSave, nowMs, totalKb);
    track(m_move, facts.moving, EventKind::IndexMove, nowMs, totalKb);
    const bool big = facts.biggestDocumentKb >= Events::kBigDocumentKb;
    if (big)
        m_documentPeakKb = m_document == 0 ? facts.biggestDocumentKb : std::max(m_documentPeakKb, facts.biggestDocumentKb);
    track(m_document, big, EventKind::BigDocument, nowMs, totalKb);
    if (m_document != 0)
        m_events.setCount(m_document, m_documentPeakKb);
}

} // namespace ws::health
