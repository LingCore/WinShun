#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace ws {

// Pages of this process's memory: address ranges of whole 4 KB pages, by
// address, that neither overlap nor touch. A value; any thread.
class PageSet {
public:
    static constexpr std::size_t kPage = 4096;

    struct Range {
        std::uintptr_t start = 0;
        std::size_t bytes = 0;
        constexpr std::uintptr_t end() const noexcept { return start + bytes; }
        friend bool operator==(const Range&, const Range&) = default;
    };

    PageSet() = default;
    // Of ranges in any order, overlapping or not; each on page boundaries.
    explicit PageSet(std::vector<Range> ranges);
    // The pages lying wholly inside these blocks of memory.
    static PageSet within(std::span<const std::pair<const void*, std::size_t>> blocks);
    // The working set: the pages in memory for this process now. ~2 ms for
    // 40,000 pages.
    static PageSet resident();

    PageSet united(const PageSet& other) const;
    PageSet without(const PageSet& other) const;
    PageSet intersected(const PageSet& other) const;
    // The smallest ranges, as many as fit in `bytes`.
    PageSet smallest(std::size_t bytes) const;

    std::span<const Range> ranges() const noexcept { return m_ranges; }
    std::size_t bytes() const noexcept { return m_bytes; }
    bool empty() const noexcept { return m_ranges.empty(); }
    friend bool operator==(const PageSet& a, const PageSet& b) noexcept { return a.m_ranges == b.m_ranges; }

    // Out of the working set, each keeping what it holds: to the standby
    // list, counted as available memory, and back by a soft fault when next
    // used. Pages locked in memory (VirtualLock) stay, locked. ~10 ms per 100 MB.
    void release() const;
    // Has Windows read back those of them it wrote to the disk, in large
    // reads side by side (PrefetchVirtualMemory), instead of one page fault
    // at a time. Those freed since are left out; those in memory cost next
    // to nothing. Returns the bytes asked for.
    std::size_t prefetch() const;

private:
    std::vector<Range> m_ranges;
    std::size_t m_bytes = 0;
};

// The pages a window needs to come up. They stay in memory while the rest is
// given back (IdleTrim), so an opening after sitting idle is as quick as one
// a moment after the last. Before, everything went, and the first opening
// brought back 13,000 to 40,000 pages one fault at a time: 10 to 50 ms more
// while Windows still had them in memory, 100 ms and seconds once it had
// given them to other programs and they came from the disk (GitHub #111,
// #265, #280). Should Windows take these all the same while memory is short,
// they are read back ahead at each press (PageSet::prefetch).
//
// Which pages: after the memory was given back, whatever is in the working
// set once the window is seen, and was not kept, was used since. The first
// time, that is taken as it is. From then on a page joins only when two such
// openings in a row had to bring it back: an opening also touches memory it
// never needs again (what it allocates anew, ~2 MB each time), and kept, that
// would add up. The caller leaves out what the file index holds: a search
// reads it on its own threads, and it is most of the memory.
//
// A window's very first opening after the start is not helped: nothing is
// learnt yet, and most of what it touches it makes then. Drawing the window
// once unseen after the first giving back, to learn from that, was tried:
// short of memory 148 to 211 ms instead of 183 to 259, not worth its code.
class HotPages {
public:
    // At most this much is taken the first time: of more, the scattered
    // pages, which cost a fault each, while the large runs are left to
    // Windows and nothing is added later. Grown since to twice that (or by
    // kSlackBytes, where that is more), what was added goes again: pages an
    // opening no longer uses are never told apart from those it does, and a
    // little is added every day. Those still used are back after two
    // openings; the first ones stay, so that no opening starts from nothing.
    // Twice: the first opening after the start shows an empty box, later
    // ones the last search's rows as well (43 MB, then 64, measured).
    static constexpr std::size_t kMostBytes = std::size_t {96} << 20;
    static constexpr std::size_t kSlackBytes = std::size_t {32} << 20;

    // `used`: in the working set as the window was seen, the first time
    // since the memory was given back, and not among those kept. Any thread.
    void learn(const PageSet& used);
    PageSet pages() const;
    std::size_t bytes() const;

private:
    mutable std::mutex m_mutex;
    PageSet m_pages;
    PageSet m_once; // used at the last such opening, for the first time
    PageSet m_first; // m_pages as first taken
    bool m_whole = true; // all that was used then, not just kMostBytes of it
};

} // namespace ws
