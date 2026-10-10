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

// By how well they match, a tie to the one written last; or by when they
// were written, a tie to the better match. Then by id, so that the order
// never depends on how the slices were split.
template <RankBy By> bool better(const NameHit& a, const NameHit& b) noexcept
{
    if constexpr (By == RankBy::Match) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.modified != b.modified)
            return a.modified > b.modified;
    } else {
        if (a.modified != b.modified)
            return a.modified > b.modified;
        if (a.score != b.score)
            return a.score > b.score;
    }
    return a.id < b.id;
}

// Fixed-capacity min-heap holding the best hits seen so far.
template <RankBy By> class TopK {
public:
    explicit TopK(std::size_t capacity)
        : m_capacity(capacity)
    {
        m_heap.reserve(capacity);
    }

    // Whether a hit could still make it, from what is known before its last
    // adjustments (they only lower the score).
    bool mayAccept(int score, FileTime modified) const noexcept
    {
        if (m_heap.size() < m_capacity)
            return true;
        return By == RankBy::Match ? score >= m_heap.front().score : modified >= m_heap.front().modified;
    }

    void push(const NameHit& hit)
    {
        if (m_heap.size() < m_capacity) {
            m_heap.push_back(hit);
            std::push_heap(m_heap.begin(), m_heap.end(), better<By>);
        } else if (better<By>(hit, m_heap.front())) {
            std::pop_heap(m_heap.begin(), m_heap.end(), better<By>);
            m_heap.back() = hit;
            std::push_heap(m_heap.begin(), m_heap.end(), better<By>);
        }
    }

    std::vector<NameHit> take() { return std::move(m_heap); }

private:
    std::size_t m_capacity;
    std::vector<NameHit> m_heap;
};

// The best `limit` of `hits`, best first.
template <RankBy By> void keepBest(std::vector<NameHit>& hits, std::size_t limit)
{
    const std::size_t keep = std::min(limit, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(keep), hits.end(), better<By>);
    hits.resize(keep);
}

template <RankBy By>
NameSearchOutput search(const FileIndex& index, const NameMatcher& matcher, const NameSearchOptions& options,
    WorkerPool& pool, const std::function<bool()>& isCancelled)
{
    NameSearchOutput output;
    const std::size_t limit = options.limit;
    const bool apart = options.kindsApart && !options.foldersOnly;
    const std::size_t slotCount = index.slotCount();
    const std::size_t slices = (slotCount + kSliceSize - 1) / kSliceSize;
    std::vector<std::vector<NameHit>> partial(slices);
    std::vector<std::vector<NameHit>> partialFolders(apart ? slices : 0);
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

        TopK<By> top(limit);
        TopK<By> folders(apart ? limit : 0);
        std::size_t matches = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Entry& e = entries[i];
            if (e.flags & (EntryFlag::Deleted | EntryFlag::Root))
                continue;
            if (options.foldersOnly && !e.isDir())
                continue;
            int score = matcher.match(index, e);
            if (score < 0)
                continue;
            ++matches;
            score += staticBonus(e);
            TopK<By>& heap = apart && e.isDir() ? folders : top;
            const auto id = static_cast<EntryId>(first + i);
            const FileTime modified = index.modified(id);
            if (!heap.mayAccept(score, modified))
                continue;
            score -= std::min(index.depth(id), 12);
            heap.push({id, score, modified});
        }
        total.fetch_add(matches, std::memory_order_relaxed);
        partial[slice] = top.take();
        if (apart)
            partialFolders[slice] = folders.take();
    });

    output.cancelled = cancelled.load();
    if (output.cancelled)
        return output;

    const auto merge = [&](std::vector<std::vector<NameHit>>& parts) {
        std::vector<NameHit> hits;
        std::size_t merged = 0;
        for (const auto& p : parts)
            merged += p.size();
        hits.reserve(merged);
        for (auto& p : parts)
            hits.insert(hits.end(), p.begin(), p.end());
        keepBest<By>(hits, limit);
        return hits;
    };
    output.hits = merge(partial);
    if (apart) {
        const std::vector<NameHit> folderHits = merge(partialFolders);
        output.hits.insert(output.hits.end(), folderHits.begin(), folderHits.end());
        std::inplace_merge(output.hits.begin(), output.hits.end() - static_cast<std::ptrdiff_t>(folderHits.size()),
            output.hits.end(), better<By>);
    }
    output.totalMatches = total.load();
    return output;
}

} // namespace

NameSearchOutput searchNames(const FileIndex& index, const NameMatcher& matcher, const NameSearchOptions& options,
    WorkerPool& pool, const std::function<bool()>& isCancelled)
{
    if (options.limit == 0)
        return {};
    return options.rankBy == RankBy::Modified ? search<RankBy::Modified>(index, matcher, options, pool, isCancelled)
                                              : search<RankBy::Match>(index, matcher, options, pool, isCancelled);
}

} // namespace ws
