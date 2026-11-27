#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ws::memory {

// What this process's memory is made of, for a probe that asks why a window
// costs what it does (--preview-settings --page-memory): never for the running
// app, the heap walk reads every block's header and brings a trimmed process
// back in (ProcessUsage.h).
struct Census {
    std::uint64_t commit = 0; // private memory committed, resident or not
    std::uint64_t privateWorkingSet = 0; // Task Manager's Memory
    // This process's share of the graphics cards' memory, as Windows counts it
    // (IDXGIAdapter3::QueryVideoMemoryInfo, all adapters): on a card of its
    // own; in the PC's memory, which is where an integrated one keeps it all.
    std::uint64_t videoDedicated = 0;
    std::uint64_t videoShared = 0;

    // The heaps' blocks in use, by size: up to 64 B, 256 B, 1 KB, 4 KB, 64 KB,
    // 1 MB, and above.
    struct Bucket {
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
    };
    static constexpr std::array<std::uint64_t, 6> kUpTo {64, 256, 1024, 4096, 65536, 1048576};
    std::array<Bucket, 7> heap {};
    std::uint64_t heapBytes = 0;
    std::uint64_t heapBlocks = 0;
    std::uint64_t heapCommitted = 0; // the heaps' regions: the blocks in use, and what was freed and not given back
    // Private allocations of 1 MB committed and more (VirtualAlloc: a heap's
    // segments, a picture's pixels, a driver's buffers), the largest first.
    struct Large {
        std::uint64_t bytes = 0;
        bool heap = false; // a heap's region
        bool code = false; // executable: compiled JavaScript
    };
    std::vector<Large> large;
    std::uint64_t privateOutsideHeaps = 0; // committed private memory that is no heap's
};

Census census(bool walkHeaps);

} // namespace ws::memory
