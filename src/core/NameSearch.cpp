#include "NameSearch.h"

#include "TextUtil.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace ws {

namespace {

constexpr std::size_t kSliceSize = 16384; // divides FileIndex::kChunkSize
static_assert(FileIndex::kChunkSize % kSliceSize == 0);

constexpr int kLowPriorityCost = 35; // under system, program and tool folders

// Ranking signals that do not depend on the query.
int staticBonus(const Entry& e) noexcept
{
    int bonus = 0;
    if (e.isDir())
        bonus += 2;
    if (e.flags & EntryFlag::LowPriority)
        bonus -= kLowPriorityCost;
    if (e.flags & EntryFlag::Hidden)
        bonus -= 15;
    return bonus;
}

// What a file of a type listed first (PreferredTypes) scores more; 0 for
// any other. Named just what was typed, it is the program asked for: what
// being in a program folder took off is given back. Asked for matches only.
int preferredBonus(const FileIndex& index, const Entry& e, const NameMatcher& matcher, const PreferredTypes& preferred)
{
    std::size_t extLength = 0;
    const std::string_view name = index.fileName(e, extLength);
    if (extLength == 0 || !preferred.has(name.substr(name.size() - extLength)))
        return 0;
    int bonus = PreferredTypes::kBonus;
    if ((e.flags & EntryFlag::LowPriority) && matcher.isExactName(name, false))
        bonus += kLowPriorityCost;
    return bonus;
}

// When written, as the order wants it: the larger the sooner. Oldest first
// turns the time round; a time not known (0) comes last both ways.
template <RankBy By> constexpr std::uint32_t timeKey(FileTime modified) noexcept
{
    if constexpr (By == RankBy::Oldest)
        return modified == 0 ? 0 : ~modified;
    else
        return modified;
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
            return timeKey<By>(a.modified) > timeKey<By>(b.modified);
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
        if constexpr (By == RankBy::Match)
            return score >= m_heap.front().score;
        else
            return timeKey<By>(modified) >= timeKey<By>(m_heap.front().modified);
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

// Which of the index's stored names pass a test: a bit for each byte of the
// name storage, set where such a name starts. A search tests each distinct
// name once this way (a third of the names on a disk are distinct: index.js,
// __init__.py...), reading the names in the order they are stored; the pass
// over the entries then only looks at a bit for most of them. That pass is
// what costs: matching every entry's name took 10 ms for 3.3 million items,
// testing the names first and then a bit each about 2.
class NameBits {
public:
    // False when cancelled. `test(name, fileExtLength, han)`.
    template <typename Test>
    bool build(const FileIndex& index, WorkerPool& pool, const std::function<bool()>& isCancelled, Test&& test)
    {
        const std::size_t chunks = index.nameChunkCount();
        m_words.resize(chunks * kWordsPerChunk); // kept from search to search: no zeroing, no page faults
        std::atomic<bool> cancelled {false};
        pool.parallelFor(chunks, [&](std::size_t chunk) {
            if (cancelled.load(std::memory_order_relaxed) || (isCancelled && isCancelled())) {
                cancelled.store(true, std::memory_order_relaxed);
                return;
            }
            std::fill_n(m_words.data() + chunk * kWordsPerChunk, kWordsPerChunk, 0);
            index.forEachName(chunk, [&](std::uint32_t offset, std::string_view name) {
                if (test(name, index.nameExtLength(offset), index.nameHasHan(offset))) {
                    const std::size_t bit = bitOf(offset);
                    m_words[bit >> 6] |= std::uint64_t {1} << (bit & 63);
                }
            });
        });
        return !cancelled.load();
    }

    bool has(std::uint32_t nameOffset) const noexcept
    {
        const std::size_t bit = bitOf(nameOffset);
        return (m_words[bit >> 6] >> (bit & 63)) & 1;
    }

    // One per searching thread, reused.
    static NameBits& forThisThread()
    {
        thread_local NameBits bits;
        return bits;
    }

private:
    // A stored name takes 3 bytes at least (FileIndex::storeName), so a bit
    // for every third byte of a chunk is one per name at most: 1.2 MB for
    // 29 MB of names. Each chunk has words of its own (a thread each).
    static constexpr std::size_t kMinRecord = 3;
    static constexpr std::size_t kWordsPerChunk = (FileIndex::kNameChunkSize / kMinRecord + 63) / 64;
    static std::size_t bitOf(std::uint32_t offset) noexcept
    {
        return (offset >> FileIndex::kNameChunkBits) * kWordsPerChunk * 64
            + (offset & (FileIndex::kNameChunkSize - 1)) / kMinRecord;
    }

    std::vector<std::uint64_t> m_words;
};

// The best `limit` of `hits`, best first.
template <typename Order> void keepBest(std::vector<NameHit>& hits, std::size_t limit, const Order& order)
{
    const std::size_t keep = std::min(limit, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(keep), hits.end(), order);
    hits.resize(keep);
}

// Fixed-capacity min-heap holding the best hits seen so far, by `order`
// (a comparator: whether the first hit goes before the second).
template <typename Order> class TopHits {
public:
    TopHits(std::size_t capacity, const Order& order)
        : m_capacity(capacity)
        , m_order(order)
    {
        m_heap.reserve(capacity);
    }

    // Whether a hit could still make it, from what is known before its last
    // adjustments (they only lower the score).
    bool mayAccept(const NameHit& hit) const noexcept
    {
        return m_heap.size() < m_capacity || m_order(hit, m_heap.front());
    }

    void push(const NameHit& hit)
    {
        if (m_heap.size() < m_capacity) {
            m_heap.push_back(hit);
            std::push_heap(m_heap.begin(), m_heap.end(), m_order);
        } else if (m_order(hit, m_heap.front())) {
            std::pop_heap(m_heap.begin(), m_heap.end(), m_order);
            m_heap.back() = hit;
            std::push_heap(m_heap.begin(), m_heap.end(), m_order);
        }
    }

    std::vector<NameHit> take() { return std::move(m_heap); }

private:
    std::size_t m_capacity;
    const Order& m_order;
    std::vector<NameHit> m_heap;
};

// ---- the orders a list can be in (SortOrder) ---------------------------------------

// As the rank says (RankBy): the match, or the time written.
template <RankBy By> struct RankOrder {
    bool operator()(const NameHit& a, const NameHit& b) const noexcept { return better<By>(a, b); }
};

// By size, a size not known (and every folder) last either way; then as a match.
struct SizeOrder {
    bool descending;
    bool operator()(const NameHit& a, const NameHit& b) const noexcept
    {
        if (a.size != b.size) {
            if (a.size == kUnknownSize || b.size == kUnknownSize)
                return b.size == kUnknownSize;
            return descending ? a.size > b.size : a.size < b.size;
        }
        return better<RankBy::Match>(a, b);
    }
};

// Names (WTF-8), ASCII case aside, byte by byte: the order of their code points.
int compareNames(std::string_view a, std::string_view b) noexcept
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = text::fold(a[i]);
        const unsigned char y = text::fold(b[i]);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

struct NameOrder {
    const FileIndex& index;
    bool descending;
    bool operator()(const NameHit& a, const NameHit& b) const noexcept
    {
        if (a.id == b.id)
            return false;
        const int c = compareNames(index.name(a.id), index.name(b.id));
        if (c != 0)
            return descending ? c > 0 : c < 0;
        return a.id < b.id;
    }
};

// By type: the extension (ASCII case aside), then the name. A folder or a
// file with no extension has none, so it comes before "7z" ascending.
struct TypeOrder {
    const FileIndex& index;
    bool descending;
    bool operator()(const NameHit& a, const NameHit& b) const noexcept
    {
        if (a.id == b.id)
            return false;
        int c = compareNames(extensionOf(a.id), extensionOf(b.id));
        if (c == 0)
            c = compareNames(index.name(a.id), index.name(b.id));
        if (c != 0)
            return descending ? c > 0 : c < 0;
        return a.id < b.id;
    }

private:
    std::string_view extensionOf(EntryId id) const noexcept
    {
        const Entry& e = index.entry(id);
        if (e.isDir())
            return {};
        std::size_t extLength = 0;
        const std::string_view name = index.fileName(e, extLength);
        return name.substr(name.size() - extLength);
    }
};

struct CreatedOrder {
    const FileIndex& index;
    bool descending;
    bool operator()(const NameHit& a, const NameHit& b) const noexcept
    {
        const FileTime x = knownTime(index.created(a.id)), y = knownTime(index.created(b.id));
        if (x != y) {
            if (x == 0 || y == 0)
                return y == 0;
            return descending ? x > y : x < y;
        }
        return better<RankBy::Match>(a, b);
    }
};

struct FolderOrder {
    const FileIndex& index;
    bool descending;
    bool operator()(const NameHit& a, const NameHit& b) const noexcept
    {
        const EntryId x = index.entry(a.id).parent, y = index.entry(b.id).parent;
        const auto name = [&](EntryId id) { return id == kNoEntry ? std::string_view() : index.name(id); };
        const int compared = compareNames(name(x), name(y));
        return compared != 0 ? (descending ? compared > 0 : compared < 0) : NameOrder {index, descending}(a, b);
    }
};

// The folders in the order of their paths: each folder's place in a walk
// of the tree, roots and children by name (ASCII case aside), parents
// before what is in them. One number a slot (folders alone mean anything):
// a comparison by path is then two reads, not two paths built. Made once
// per search in that order; 425 000 folders take some tens of milliseconds,
// which building a path per comparison took seconds for (measured on 3.3
// million entries: 5 s a browse, now under 100 ms).
std::vector<std::uint32_t> folderRanks(const FileIndex& index)
{
    std::vector<std::uint32_t> ranks(index.slotCount(), 0);
    std::vector<EntryId> stack;
    std::vector<EntryId> children;
    const auto pushSorted = [&](std::vector<EntryId>& folders) {
        std::sort(folders.begin(), folders.end(), [&](EntryId a, EntryId b) {
            const int c = compareNames(index.name(a), index.name(b));
            return c != 0 ? c < 0 : a < b;
        });
        for (auto it = folders.rbegin(); it != folders.rend(); ++it) // the first on top
            stack.push_back(*it);
    };
    std::vector<EntryId> roots(index.roots());
    std::erase_if(roots, [&](EntryId r) { return index.entry(r).isDeleted(); });
    pushSorted(roots);
    std::uint32_t next = 1;
    while (!stack.empty()) {
        const EntryId folder = stack.back();
        stack.pop_back();
        ranks[folder] = next++;
        children.clear();
        for (EntryId c = index.entry(folder).firstChild; c != kNoEntry; c = index.entry(c).nextSibling) {
            const Entry& e = index.entry(c);
            if (e.isDir() && !e.isDeleted())
                children.push_back(c);
        }
        pushSorted(children);
    }
    return ranks;
}

// By the folder each is in (its path, ASCII case aside: folderRanks), then
// by name: a folder's files stay together.
struct PathOrder {
    const FileIndex& index;
    const std::vector<std::uint32_t>& ranks;
    bool descending;
    bool operator()(const NameHit& a, const NameHit& b) const noexcept
    {
        if (a.id == b.id)
            return false;
        const EntryId pa = index.entry(a.id).parent;
        const EntryId pb = index.entry(b.id).parent;
        int c = 0;
        if (pa != pb) {
            const std::uint32_t ra = pa == kNoEntry ? 0 : ranks[pa];
            const std::uint32_t rb = pb == kNoEntry ? 0 : ranks[pb];
            c = ra < rb ? -1 : ra > rb ? 1 : 0;
        }
        if (c == 0)
            c = compareNames(index.name(a.id), index.name(b.id));
        if (c != 0)
            return descending ? c > 0 : c < 0;
        return a.id < b.id;
    }
};

// Whether an entry lies under one of some folders (sorted ids), remembered
// by its parent: siblings ask about the same one.
class UnderAny {
public:
    UnderAny(const FileIndex& index, const std::vector<EntryId>& folders)
        : m_index(index)
        , m_folders(folders)
    {
        m_cache.fill({kNoEntry, false});
    }
    bool operator()(const Entry& e)
    {
        const EntryId parent = e.parent;
        if (parent == kNoEntry)
            return false;
        Slot& slot = m_cache[parent & (m_cache.size() - 1)];
        if (slot.parent != parent) {
            bool under = false;
            for (EntryId cur = parent; cur != kNoEntry && !under; cur = m_index.entry(cur).parent)
                under = std::binary_search(m_folders.begin(), m_folders.end(), cur);
            slot = {parent, under};
        }
        return slot.under;
    }

private:
    struct Slot {
        EntryId parent;
        bool under;
    };
    const FileIndex& m_index;
    const std::vector<EntryId>& m_folders;
    std::array<Slot, 4096> m_cache {};
};

// The hits in the folder typed in first (NameSearchOptions::insideFirst),
// then the others, each part in `order`: one key more in front of it, so the
// slices' heaps and their merge keep the best of the folder's own first.
template <typename Order> struct InsideFirst {
    Order order;
    bool operator()(const NameHit& a, const NameHit& b) const noexcept
    {
        if (a.inside != b.inside)
            return a.inside;
        return order(a, b);
    }
};

// `makeOrder(index)`: the order for one slice (or the merge), made anew each
// time (PathOrder keeps a slice's folder paths).
template <typename MakeOrder>
NameSearchOutput search(const FileIndex& index, const NameMatcher& matcher, const NameSearchOptions& options,
    WorkerPool& pool, const std::function<bool()>& isCancelled, MakeOrder&& makeOrder)
{
    NameSearchOutput output;
    const std::size_t limit = options.limit;
    // Files and folders apart as the caller asks (KindOrder). The panel asks
    // not to in a column's order: that is one list; the launcher's sorts
    // (#44) keep the kinds apart, each in the order.
    const bool apart = options.kindsApart && !options.foldersOnly;
    // The types listed first: by how well they match only.
    const bool prefer = !options.preferred.empty() && !options.foldersOnly && options.rankBy == RankBy::Match
        && options.order.key == SortKey::Rank;
    const EntryFilter& filter = options.filter;
    // The size of each match is read only when something asks for it.
    const bool wantSize = filter.bySize() || options.order.key == SortKey::Size;
    const std::size_t slotCount = index.slotCount();
    const std::size_t slices = (slotCount + kSliceSize - 1) / kSliceSize;
    std::vector<std::vector<NameHit>> partial(slices);
    std::vector<std::vector<NameHit>> partialFolders(apart ? slices : 0);
    std::atomic<std::size_t> total {0};
    std::atomic<bool> cancelled {false};
    // The folder typed in (insideFirst), for UnderAny: one, so its test is a
    // compare per folder up the way, remembered by parent as for the filter.
    const std::vector<EntryId> insideOf
        = options.insideFirst == kNoEntry ? std::vector<EntryId>() : std::vector<EntryId> {options.insideFirst};

    NameBits* names = nullptr;
    if (matcher.filtersNames()) {
        names = &NameBits::forThisThread();
        const bool built = names->build(index, pool, isCancelled,
            [&](std::string_view name, std::size_t extLength, bool han) { return matcher.nameMayMatch(name, extLength, han); });
        if (!built) {
            output.cancelled = true;
            return output;
        }
    }

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

        const auto order = makeOrder(index);
        TopHits<decltype(order)> top(limit, order);
        TopHits<decltype(order)> folders(apart ? limit : 0, order);
        NameMatcher::Folders seen;
        UnderAny under(index, filter.folders);
        // Each slice its own, as `under`: their caches are written as they are asked.
        std::optional<UnderAny> inside;
        if (!insideOf.empty())
            inside.emplace(index, insideOf);
        std::size_t matches = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Entry& e = entries[i];
            if (e.flags & (EntryFlag::Deleted | EntryFlag::Root))
                continue;
            if (options.foldersOnly && !e.isDir())
                continue;
            if (names && !names->has(e.nameOffset))
                continue;
            // The bounds before the name: a time or a size is one read, a
            // name a search through it.
            const auto id = static_cast<EntryId>(first + i);
            const FileTime modified = knownTime(index.modified(id)); // 1970: none, last either way
            const FileSize size = wantSize && !e.isDir() ? index.size(id) : kUnknownSize;
            if (!filter.fits(modified, size, e.isDir()))
                continue;
            if (!filter.folders.empty() && !under(e))
                continue;
            int score = matcher.match(index, e, seen, id);
            if (score < 0)
                continue;
            ++matches;
            score += staticBonus(e);
            // Before mayAccept, which takes the score for the most it can be:
            // asking it first with the most this could add let many more
            // through each slice's heap (1 ms slower for "a", not faster).
            if (prefer && !e.isDir())
                score += preferredBonus(index, e, matcher, options.preferred);
            TopHits<decltype(order)>& heap = apart && e.isDir() ? folders : top;
            // Asked first as if it were in the folder typed in (InsideFirst
            // puts those first), so its parents are walked only for hits
            // that could still make the list; one found elsewhere is asked
            // again before its depth is counted (another walk up).
            NameHit hit {.id = id,
                .score = score,
                .modified = modified > options.trustedUntil ? FileTime {0} : modified,
                .inside = inside.has_value(),
                .size = size};
            if (!heap.mayAccept(hit))
                continue;
            if (inside) {
                hit.inside = (*inside)(e);
                if (!hit.inside && !heap.mayAccept(hit))
                    continue;
            }
            hit.score -= std::min(index.depth(id), 12);
            heap.push(hit);
        }
        total.fetch_add(matches, std::memory_order_relaxed);
        partial[slice] = top.take();
        if (apart)
            partialFolders[slice] = folders.take();
    });

    output.cancelled = cancelled.load();
    if (output.cancelled)
        return output;

    const auto order = makeOrder(index);
    const auto merge = [&](std::vector<std::vector<NameHit>>& parts) {
        std::vector<NameHit> hits;
        std::size_t merged = 0;
        for (const auto& p : parts)
            merged += p.size();
        hits.reserve(merged);
        for (auto& p : parts)
            hits.insert(hits.end(), p.begin(), p.end());
        keepBest(hits, limit, order);
        return hits;
    };
    output.hits = merge(partial);
    if (apart) {
        const std::vector<NameHit> folderHits = merge(partialFolders);
        output.hits.insert(output.hits.end(), folderHits.begin(), folderHits.end());
        std::inplace_merge(output.hits.begin(), output.hits.end() - static_cast<std::ptrdiff_t>(folderHits.size()),
            output.hits.end(), order);
    }
    output.totalMatches = total.load();
    return output;
}

// search() in `makeOrder`'s order, or with the hits in the folder typed in
// first (insideFirst): each order is made both ways, so that a search
// without one costs nothing more than before.
template <typename MakeOrder>
NameSearchOutput searchIn(const FileIndex& index, const NameMatcher& matcher, const NameSearchOptions& options,
    WorkerPool& pool, const std::function<bool()>& isCancelled, MakeOrder&& makeOrder)
{
    if (options.insideFirst == kNoEntry)
        return search(index, matcher, options, pool, isCancelled, makeOrder);
    return search(index, matcher, options, pool, isCancelled,
        [&makeOrder](const FileIndex& i) { return InsideFirst<decltype(makeOrder(i))> {makeOrder(i)}; });
}

} // namespace

PreferredTypes::PreferredTypes(const std::vector<std::string>& extensions)
{
    for (const std::string& ext : extensions) {
        if (ext.empty() || ext.size() > kLongest || m_count == kMax || has(ext))
            continue;
        m_keys[m_count++] = keyOf(ext);
        m_lengths |= std::uint32_t {1} << ext.size();
        // Both cases of a letter: has() tests the byte as it is.
        const unsigned char lower = text::fold(ext.front());
        const auto upper = static_cast<unsigned char>(lower >= 'a' && lower <= 'z' ? lower - 32 : lower);
        for (const unsigned char first : {lower, upper})
            m_firsts[first >> 6] |= std::uint64_t {1} << (first & 63);
    }
}

PreferredTypes::Key PreferredTypes::keyOf(std::string_view extension) noexcept
{
    std::array<unsigned char, sizeof(Key)> bytes {};
    for (std::size_t i = 0; i < extension.size(); ++i)
        bytes[i] = text::fold(extension[i]);
    Key key;
    std::memcpy(key.data(), bytes.data(), sizeof key);
    return key;
}

bool PreferredTypes::has(std::string_view extension) const noexcept
{
    if (extension.empty() || extension.size() > kLongest || !(m_lengths >> extension.size() & 1))
        return false;
    const auto first = static_cast<unsigned char>(extension.front());
    if (!(m_firsts[first >> 6] >> (first & 63) & 1))
        return false;
    const Key key = keyOf(extension);
    for (std::size_t i = 0; i < m_count; ++i) {
        if (m_keys[i] == key)
            return true;
    }
    return false;
}

bool PreferredTypes::hasName(std::string_view name) const noexcept
{
    const std::size_t length = extensionLength(name, false); // as the index has it
    return length > 0 && has(name.substr(name.size() - length));
}

NameSearchOutput searchNames(const FileIndex& index, const NameMatcher& matcher, const NameSearchOptions& options,
    WorkerPool& pool, const std::function<bool()>& isCancelled)
{
    if (options.limit == 0)
        return {};
    const bool down = options.order.descending;
    switch (options.order.key) {
    case SortKey::Name:
        return searchIn(index, matcher, options, pool, isCancelled, [down](const FileIndex& i) { return NameOrder {i, down}; });
    case SortKey::Path: {
        const std::vector<std::uint32_t> ranks = folderRanks(index);
        return searchIn(index, matcher, options, pool, isCancelled,
            [down, &ranks](const FileIndex& i) { return PathOrder {i, ranks, down}; });
    }
    case SortKey::Type:
        return searchIn(index, matcher, options, pool, isCancelled, [down](const FileIndex& i) { return TypeOrder {i, down}; });
    case SortKey::Created:
        return searchIn(index, matcher, options, pool, isCancelled, [down](const FileIndex& i) { return CreatedOrder {i, down}; });
    case SortKey::Folder:
        return searchIn(index, matcher, options, pool, isCancelled, [down](const FileIndex& i) { return FolderOrder {i, down}; });
    case SortKey::Source:
        return searchIn(index, matcher, options, pool, isCancelled, [down](const FileIndex& i) { return NameOrder {i, down}; });
    case SortKey::Size:
        return searchIn(index, matcher, options, pool, isCancelled, [down](const FileIndex&) { return SizeOrder {down}; });
    case SortKey::Modified:
        if (down)
            return searchIn(index, matcher, options, pool, isCancelled, [](const FileIndex&) { return RankOrder<RankBy::Newest> {}; });
        return searchIn(index, matcher, options, pool, isCancelled, [](const FileIndex&) { return RankOrder<RankBy::Oldest> {}; });
    case SortKey::Rank:
        break;
    }
    switch (options.rankBy) {
    case RankBy::Newest:
        return searchIn(index, matcher, options, pool, isCancelled, [](const FileIndex&) { return RankOrder<RankBy::Newest> {}; });
    case RankBy::Oldest:
        return searchIn(index, matcher, options, pool, isCancelled, [](const FileIndex&) { return RankOrder<RankBy::Oldest> {}; });
    case RankBy::Match:
        break;
    }
    return searchIn(index, matcher, options, pool, isCancelled, [](const FileIndex&) { return RankOrder<RankBy::Match> {}; });
}

namespace {

// Whether a folder is, or lies in, one that a build tool or an environment
// fills: a CMake or Ninja build folder, a cache folder so tagged (Cargo's
// target), a Python virtual environment, or by name CMakeFiles and obj. A
// build writes hundreds of files at once there; none is one a person works
// on. Remembered by folder, for one thread.
class OutputTrees {
public:
    explicit OutputTrees(const FileIndex& index)
        : m_index(index)
    {
    }

    bool contains(EntryId folder)
    {
        // Up to a folder known already (or the top), then down again.
        m_chain.clear();
        bool inside = false;
        for (EntryId cur = folder; cur != kNoEntry; cur = m_index.entry(cur).parent) {
            if (const auto it = m_known.find(cur); it != m_known.end()) {
                inside = it->second;
                break;
            }
            m_chain.push_back(cur);
        }
        for (auto it = m_chain.rbegin(); it != m_chain.rend(); ++it) {
            inside = inside || isOutput(*it);
            m_known.emplace(*it, inside);
        }
        return inside;
    }

private:
    bool isOutput(EntryId folder) const
    {
        static constexpr std::string_view kNames[] {"CMakeFiles", "obj"};
        static constexpr std::string_view kMarkers[] {"CMakeCache.txt", "build.ninja", "CACHEDIR.TAG", "pyvenv.cfg"};
        const std::string_view name = m_index.name(folder);
        return std::any_of(std::begin(kNames), std::end(kNames),
                   [&](std::string_view n) { return text::equalsIgnoreAsciiCase(name, n); })
            || std::any_of(std::begin(kMarkers), std::end(kMarkers),
                [&](std::string_view marker) { return m_index.findChild(folder, marker) != kNoEntry; });
    }

    const FileIndex& m_index;
    std::unordered_map<EntryId, bool> m_known;
    std::vector<EntryId> m_chain;
};

// The tree of folders a folder belongs to, for taking only a few from one:
// in a user's folder (C:\Users\me\Documents\...), the folder in that one
// ("Documents\Project"); elsewhere, the folder at the top of the drive
// ("F:\Project"). Directly in such a place, the place itself.
EntryId workTree(const FileIndex& index, EntryId folder)
{
    std::vector<EntryId> chain; // folder first, the drive last
    for (EntryId cur = folder; cur != kNoEntry; cur = index.entry(cur).parent)
        chain.push_back(cur);
    const auto level = [&](std::size_t k) { return chain[chain.size() - 1 - k]; }; // 0: the drive
    const bool inProfile = chain.size() >= 3 && text::equalsIgnoreAsciiCase(index.name(level(1)), "Users");
    const std::size_t top = inProfile ? 4 : 1; // C:\Users\me\Documents\Project, F:\Project
    return level(std::min(top, chain.size() - 1));
}

} // namespace

NewestOutput newestFiles(const FileIndex& index, const NewestOptions& options, WorkerPool& pool,
    const std::function<bool()>& isCancelled)
{
    NewestOutput output;
    if (options.limit == 0)
        return output;
    // Enough to choose from once a busy folder has had its share: at least
    // 80 (a short list lets one folder take little of it). For the 300 of
    // the list to scroll, 4 times as many as listed gave the same list as
    // 16 times, in 7 ms instead of 14 (3.3 million entries).
    const std::size_t kept = std::max<std::size_t>(options.limit * 4, 80);
    const std::size_t slotCount = index.slotCount();
    const std::size_t slices = (slotCount + kSliceSize - 1) / kSliceSize;
    std::vector<std::vector<NameHit>> partial(slices);
    std::atomic<bool> cancelled {false};
    const std::uint8_t skipped = EntryFlag::Deleted | EntryFlag::Root | EntryFlag::LowPriority | EntryFlag::Hidden;

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
        TopK<RankBy::Newest> top(kept);
        OutputTrees outputs(index);
        for (std::size_t i = 0; i < count; ++i) {
            const Entry& e = entries[i];
            if ((e.flags & skipped) || e.isDir())
                continue;
            const auto id = static_cast<EntryId>(first + i);
            const FileTime modified = knownTime(index.modified(id));
            if (modified == 0 || modified > options.notAfter || !top.mayAccept(0, modified))
                continue;
            std::size_t extLength = 0;
            const std::string_view name = index.fileName(e, extLength);
            if (!options.extensions.empty()) {
                const std::string_view ext = name.substr(name.size() - extLength);
                if (extLength == 0 || std::none_of(options.extensions.begin(), options.extensions.end(),
                        [&](const std::string& x) { return text::equalsFolded(ext, x); }))
                    continue;
            }
            if (extLength > 0 && !options.excludedExtensions.empty()) {
                const std::string_view ext = name.substr(name.size() - extLength);
                if (std::any_of(options.excludedExtensions.begin(), options.excludedExtensions.end(),
                        [&](const std::string& x) { return text::equalsFolded(ext, x); }))
                    continue;
            }
            if (!options.excludedFileNames.empty() && nameMatchesAny(name, options.excludedFileNames))
                continue;
            if (isWorkFile(name, false) || outputs.contains(e.parent))
                continue;
            top.push({id, 0, modified});
        }
        partial[slice] = top.take();
    });

    output.cancelled = cancelled.load();
    if (output.cancelled)
        return output;
    std::vector<NameHit> hits;
    for (auto& p : partial)
        hits.insert(hits.end(), p.begin(), p.end());
    // Files: a few from each folder, and from each tree of folders (workTree),
    // so that a clone, an unpacked archive or an export that writes hundreds
    // of files at once leaves room for the rest. Folders: each once. Gone
    // through newest first, put in order a batch at a time: a long list
    // (hundreds of thousands to choose from) is mostly never reached.
    const std::size_t batch = kept;
    std::size_t sorted = 0;
    std::unordered_map<EntryId, std::size_t> perFolder;
    std::unordered_map<EntryId, std::size_t> perTree;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (output.hits.size() >= options.limit)
            break;
        if (i == sorted) {
            const auto from = hits.begin() + static_cast<std::ptrdiff_t>(sorted);
            const auto to = hits.begin() + static_cast<std::ptrdiff_t>(std::min(hits.size(), sorted + batch));
            std::nth_element(from, to - 1, hits.end(), better<RankBy::Newest>);
            std::sort(from, to, better<RankBy::Newest>);
            sorted = static_cast<std::size_t>(to - hits.begin());
        }
        const NameHit& hit = hits[i];
        const EntryId parent = index.entry(hit.id).parent;
        if (parent == kNoEntry)
            continue;
        std::size_t& inFolder = perFolder[parent];
        std::size_t& inTree = perTree[workTree(index, parent)];
        if (inFolder >= (options.folders ? 1 : options.perFolder) || inTree >= options.perTree)
            continue;
        ++inFolder;
        ++inTree;
        if (!options.folders)
            output.hits.push_back(hit);
        else if (!(index.entry(parent).flags & EntryFlag::Root)) // a drive is no place one works in
            output.hits.push_back({parent, 0, hit.modified});
    }
    return output;
}

bool isWorkFile(std::string_view name, bool isDir) noexcept
{
    if (name.empty())
        return true;
    // Dot-names (.gitignore, .ninja_log) are tools' settings and records;
    // "~$Report.docx" is Office's lock file, "~WRL0001.tmp" its scratch.
    if (name.front() == '.' || name.front() == '~')
        return true;
    if (isDir)
        return false;
    const std::size_t dot = name.rfind('.');
    // A name that starts with a long hash ("28090b2f866c9be9…", "f76b2d…_t.dat"):
    // a program keeps its data under it. Digits alone may be a date or a
    // count that a person chose.
    const std::string_view stem = name.substr(0, dot);
    const auto hexEnd = std::find_if(stem.begin(), stem.end(), [](char c) {
        return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
    });
    if (hexEnd - stem.begin() >= 16 && std::any_of(stem.begin(), hexEnd, [](char c) { return c > '9'; }))
        return true;
    if (dot == std::string_view::npos)
        return false;
    const std::string_view ext = name.substr(dot + 1);
    // Downloads under way, scratch and lock files, logs, programs' own data
    // and databases, and what compilers and build tools leave next to the
    // sources.
    static constexpr std::string_view kWorkExtensions[] {
        "tmp", "temp", "bak", "crdownload", "part", "partial", "download", "opdownload", "!ut", "lock", "lck",
        "swp", "swo", "log", "etl", "blf", "regtrans-ms", "dat", "db", "wal",
        "shm", "ldb", "edb", "jfm", "crc", "mmkv", "dmp", "mdmp", "symcache", "obj", "o", "pdb", "ilk", "idb", "iobj", "ipdb", "pch",
        "ipch", "tlog", "lastbuildstate", "exp", "ninja_log", "ninja_deps", "d", "pyc", "class", "cache",
    };
    // A database's side files: "x.db-wal", "x.snapshot-shm".
    static constexpr std::string_view kWorkSuffixes[] {"-wal", "-shm", "-journal"};
    if (ext.size() > 4 && text::equalsIgnoreAsciiCase(ext.substr(0, 4), "bak-")) // a copy kept aside: "x.exe.bak-old"
        return true;
    return std::any_of(std::begin(kWorkExtensions), std::end(kWorkExtensions),
               [&](std::string_view work) { return text::equalsIgnoreAsciiCase(ext, work); })
        || std::any_of(std::begin(kWorkSuffixes), std::end(kWorkSuffixes), [&](std::string_view suffix) {
               return ext.size() > suffix.size()
                   && text::equalsIgnoreAsciiCase(ext.substr(ext.size() - suffix.size()), suffix);
           });
}

std::vector<EntryId> findRelativePath(const FileIndex& index, const std::vector<std::string>& names, bool foldersOnly,
    std::size_t limit, WorkerPool& pool, const std::function<bool()>& isCancelled)
{
    if (names.empty() || limit == 0 || names.back().size() > 0xFFFF)
        return {};
    const std::string& last = names.back();
    const std::size_t slotCount = index.slotCount();
    const std::size_t slices = (slotCount + kSliceSize - 1) / kSliceSize;
    std::vector<std::vector<EntryId>> partial(slices);
    std::atomic<bool> cancelled {false};
    NameBits& named = NameBits::forThisThread();
    if (!named.build(index, pool, isCancelled, [&](std::string_view name, std::size_t, bool) {
            return name.size() == last.size() && text::equalsIgnoreAsciiCase(name, last);
        }))
        return {};

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
        std::vector<EntryId>& found = partial[slice];
        for (std::size_t i = 0; i < count && found.size() < limit; ++i) {
            const Entry& e = entries[i];
            if ((e.flags & (EntryFlag::Deleted | EntryFlag::Root)) || (foldersOnly && !e.isDir())
                || !named.has(e.nameOffset))
                continue;
            // Then the folders it lies in, inside out.
            std::size_t k = names.size() - 1;
            for (EntryId cur = e.parent; k > 0 && cur != kNoEntry; cur = index.entry(cur).parent, --k) {
                if (!text::equalsIgnoreAsciiCase(index.name(cur), names[k - 1]))
                    break;
            }
            if (k == 0)
                found.push_back(static_cast<EntryId>(first + i));
        }
    });

    if (cancelled.load())
        return {};
    std::vector<EntryId> ids;
    for (const auto& p : partial) {
        ids.insert(ids.end(), p.begin(), p.end());
        if (ids.size() >= limit)
            break;
    }
    if (ids.size() > limit)
        ids.resize(limit);
    return ids;
}

} // namespace ws
