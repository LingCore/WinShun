#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ws {

// The pages a window needs to come up, read back ahead at each press.
// Emptied (IdleTrim, or Windows' own trimming while memory is short), the
// working set's pages wait on the standby list; while memory is short,
// Windows gives them to other programs, and the next opening reads them
// back from the disk one page fault at a time, each waited for before the
// GUI thread goes on: 13,000 to 40,000 faults an opening here, seconds on a
// loaded system (GitHub #265). Told the ranges at the press, Windows reads
// them in large reads side by side (PrefetchVirtualMemory) while the GUI
// thread starts; it finds them in memory, or on their way. With memory to
// spare it costs next to nothing: they are in memory still.
//
// Which pages: those in the working set as the window is seen, after an
// opening that followed an emptying. That is what the opening touched, and
// the little idling touched since. Each such opening records them anew.
class HotPages {
public:
    // The working set now, as page ranges, in place of the last ones; at
    // most 256 MB of them, the smallest. Any thread; ~2 ms for 40,000 pages.
    void record();
    // Has Windows read the recorded ranges back, the parts of them still
    // committed. Any thread; returns the bytes asked for (0: none recorded).
    std::size_t prefetch() const;
    std::size_t bytes() const;

private:
    struct Range {
        std::uintptr_t start = 0;
        std::size_t bytes = 0;
    };
    mutable std::mutex m_mutex;
    std::vector<Range> m_ranges; // by address
};

} // namespace ws
