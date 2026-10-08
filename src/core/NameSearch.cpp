#include "NameSearch.h"

#include <algorithm>
#include <atomic>

namespace ws {

namespace {

constexpr std::size_t kSliceSize = 16384; // divides FileIndex::kChunkSize
static_assert(FileIndex::kChunkSize % kSliceSize == 0);

// Ranking signals that do not depend on the query.
int staticBonus(const Entry& e) noexcept
{
    int bonus = 0;
    if (e.isDir())
        bonus += 2;
    if (e.flags & EntryFlag::LowPriority)
        bonus -= 35;
    if (e.flags & EntryFlag::Hidden)
        bonus -= 15;
    return bonus;
}

bool better(const NameHit& a, const NameHit& b) noexcept
{
    return a.score != b.score ? a.score > b.score : a.id < b.id;
}

// Fixed-capacity min-heap holding the best hits seen so far.
class TopK {
public:
    explicit TopK(std::size_t capacity)
        : m_capacity(capacity)
    {
        m_heap.reserve(capacity);
    }

    bool accepts(int score) const noexcept { return m_heap.size() < m_capacity || score > m_heap.front().score; }

    void push(NameHit hit)
    {
        if (m_heap.size() < m_capacity) {
            m_heap.push_back(hit);
            std::push_heap(m_heap.begin(), m_heap.end(), better);
        } else if (better(hit, m_heap.front())) {
            std::pop_heap(m_heap.begin(), m_heap.end(), better);
            m_heap.back() = hit;
            std::push_heap(m_heap.begin(), m_heap.end(), better);
        }
    }

    std::vector<NameHit> take() { return std::move(m_heap); }

private:
    std::size_t m_capacity;
    std::vector<NameHit> m_heap;
};

} // namespace

NameSearchOutput searchNames(const FileIndex& index, const NameMatcher& matcher, Scope scope, std::size_t limit,
    WorkerPool& pool, const std::function<bool()>& isCancelled)
{
    NameSearchOutput output;
    if (limit == 0)
        return output;

    const std::size_t slotCount = index.slotCount();
    const std::size_t slices = (slotCount + kSliceSize - 1) / kSliceSize;
    std::vector<std::vector<NameHit>> partial(slices);
    std::atomic<std::size_t> total {0};
    std::atomic<bool> cancelled {false};

    pool.parallelFor(slices, [&](std::size_t slice) {
        if (cancelled.load(std::memory_order_relaxed))
            return;
        if (isCancelled && isCancelled()) {
            cancelled.store(true, std::memory_order_relaxed);
            return;
        }
        const std::size_t first = slice * kSliceSize;
        const std::size_t count = std::min(kSliceSize, slotCount - first);
        const Entry* entries
            = index.chunk(first >> FileIndex::kChunkBits).data() + (first & (FileIndex::kChunkSize - 1));

        TopK top(limit);
        std::size_t matches = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Entry& e = entries[i];
            if (e.flags & (EntryFlag::Deleted | EntryFlag::Root))
                continue;
            if ((scope == Scope::Files && e.isDir()) || (scope == Scope::Folders && !e.isDir()))
                continue;
            int score = matcher.match(index, e);
            if (score < 0)
                continue;
            ++matches;
            score += staticBonus(e);
            if (!top.accepts(score))
                continue; // the depth penalty below can only lower the score
            const auto id = static_cast<EntryId>(first + i);
            score -= std::min(index.depth(id), 12);
            top.push({id, score});
        }
        total.fetch_add(matches, std::memory_order_relaxed);
        partial[slice] = top.take();
    });

    output.cancelled = cancelled.load();
    if (output.cancelled)
        return output;

    std::size_t merged = 0;
    for (const auto& p : partial)
        merged += p.size();
    output.hits.reserve(merged);
    for (auto& p : partial)
        output.hits.insert(output.hits.end(), p.begin(), p.end());
    const std::size_t keep = std::min(limit, output.hits.size());
    std::partial_sort(
        output.hits.begin(), output.hits.begin() + static_cast<std::ptrdiff_t>(keep), output.hits.end(), better);
    output.hits.resize(keep);
    output.totalMatches = total.load();
    return output;
}

} // namespace ws
