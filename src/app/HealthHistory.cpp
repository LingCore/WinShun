#include "HealthHistory.h"

#include <algorithm>
#include <limits>

namespace ws::health {

static_assert(sizeof(Sample) == 40, "a reading is 40 bytes: 24 hours of them ~340 KB");

std::uint32_t Sample::totalKb() const noexcept
{
    std::uint64_t sum = 0;
    for (const std::uint32_t kb : partKb)
        sum += kb;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, std::numeric_limits<std::uint32_t>::max()));
}

Ring::Ring(std::size_t capacity)
    : m_data(new Sample[capacity]) // default-initialised: not written, its pages untouched until used
    , m_capacity(capacity)
{
}

void Ring::push(const Sample& sample) noexcept
{
    m_data[m_head] = sample;
    m_head = (m_head + 1) % m_capacity;
    if (m_size < m_capacity)
        ++m_size;
}

const Sample& Ring::at(std::size_t i) const noexcept
{
    const std::size_t oldest = m_size < m_capacity ? 0 : m_head;
    return m_data[(oldest + i) % m_capacity];
}

History::History()
    : m_coarse(kCoarseCount)
    , m_fine(kFineCount)
{
}

void History::clear() noexcept
{
    m_coarse.clear();
    m_fine.clear();
}

std::size_t History::bytes() const noexcept
{
    return (m_coarse.capacity() + m_fine.capacity()) * sizeof(Sample);
}

Series History::series(std::int64_t fromMs, std::int64_t toMs, int buckets) const
{
    Series s;
    s.fromMs = fromMs;
    s.toMs = toMs;
    if (toMs <= fromMs || buckets <= 0)
        return s;
    const double width = static_cast<double>(toMs - fromMs) / buckets;
    // Where the fine readings begin, the 10-second ones give way to them.
    const std::int64_t fineFrom = m_fine.size() > 0 ? m_fine.at(0).atMs : std::numeric_limits<std::int64_t>::max();

    struct Bucket {
        int index = -1;
        const Sample* peak = nullptr;
        std::uint32_t peakTotal = 0;
        int n = 0;
        double cpuSum = 0, readSum = 0;
        std::uint16_t cpuMax = 0, serviceCpuMax = 0;
        std::uint32_t readMax = 0, serviceReadMax = 0, serviceMax = 0;
        bool breakBefore = false;
    } b;
    std::int64_t lastMs = std::numeric_limits<std::int64_t>::min();
    bool lastFine = false;
    constexpr double kb = 1024.0;

    const auto flush = [&] {
        if (b.n == 0)
            return;
        s.t.push_back(static_cast<double>(b.peak->atMs));
        s.breakBefore.push_back(b.breakBefore ? 1 : 0);
        for (int p = 0; p < kParts; ++p)
            s.partMb[p].push_back(b.peak->partKb[p] / kb);
        s.totalMb.push_back(b.peakTotal / kb);
        s.serviceMb.push_back(b.serviceMax / kb);
        s.cpuMax.push_back(b.cpuMax / 100.0);
        s.cpuMean.push_back(b.cpuSum / b.n / 100.0);
        s.serviceCpuMax.push_back(b.serviceCpuMax / 100.0);
        s.readMax.push_back(b.readMax / kb);
        s.readMean.push_back(b.readSum / b.n / kb);
        s.serviceReadMax.push_back(b.serviceReadMax / kb);
        s.peakTotalMb = std::max(s.peakTotalMb, s.totalMb.back());
        s.peakServiceMb = std::max(s.peakServiceMb, s.serviceMb.back());
        s.peakCpu = std::max({s.peakCpu, s.cpuMax.back(), s.serviceCpuMax.back()});
        s.peakRead = std::max({s.peakRead, s.readMax.back(), s.serviceReadMax.back()});
    };
    const auto feed = [&](const Sample& x, bool fine) {
        if (x.atMs < fromMs || x.atMs > toMs)
            return;
        const int index = std::min(buckets - 1, static_cast<int>((x.atMs - fromMs) / width));
        if (index != b.index) {
            flush();
            const std::int64_t gap = (fine && lastFine) ? History::kFineGapMs : History::kGapMs;
            b = Bucket {};
            b.index = index;
            b.breakBefore = s.t.empty() || x.atMs - lastMs > gap;
        }
        const std::uint32_t total = x.totalKb();
        if (!b.peak || total > b.peakTotal) {
            b.peak = &x;
            b.peakTotal = total;
        }
        ++b.n;
        b.cpuSum += x.cpu;
        b.readSum += x.readKbps;
        b.cpuMax = std::max(b.cpuMax, x.cpu);
        b.serviceCpuMax = std::max(b.serviceCpuMax, x.serviceCpu);
        b.readMax = std::max(b.readMax, x.readKbps);
        b.serviceReadMax = std::max(b.serviceReadMax, x.serviceReadKbps);
        b.serviceMax = std::max(b.serviceMax, x.serviceKb);
        lastMs = x.atMs;
        lastFine = fine;
    };
    for (std::size_t i = 0; i < m_coarse.size(); ++i) {
        const Sample& x = m_coarse.at(i);
        if (x.atMs >= fineFrom)
            break;
        feed(x, false);
    }
    for (std::size_t i = 0; i < m_fine.size(); ++i)
        feed(m_fine.at(i), true);
    flush();
    return s;
}

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

std::vector<Event> Events::between(std::int64_t fromMs, std::int64_t toMs) const
{
    std::vector<Event> out;
    for (const Event& e : m_events) {
        const std::int64_t end = e.open() ? std::numeric_limits<std::int64_t>::max() : e.endMs;
        if (e.startMs <= toMs && end >= fromMs)
            out.push_back(e);
    }
    return out;
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
