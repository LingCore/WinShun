#include "HotPages.h"

#include <algorithm>

#include <windows.h>
#include <psapi.h>

namespace ws {

namespace {

constexpr std::size_t kPage = 4096; // QueryWorkingSet's VirtualPage counts 4 KB pages
constexpr std::size_t kMostBytes = std::size_t {256} << 20; // an opening here: 50 to 160 MB

// Protections Windows cannot or need not read back: nothing behind them.
bool readable(const MEMORY_BASIC_INFORMATION& m)
{
    constexpr DWORD kNot = PAGE_NOACCESS | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;
    return m.State == MEM_COMMIT && m.Protect != 0 && !(m.Protect & kNot);
}

} // namespace

void HotPages::record()
{
    // Asked again with room for what it said it holds (it grows meanwhile).
    std::vector<ULONG_PTR> buffer(1 + 16'384);
    for (int attempt = 0;; ++attempt) {
        if (::QueryWorkingSet(::GetCurrentProcess(), buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(ULONG_PTR))))
            break;
        if (::GetLastError() != ERROR_BAD_LENGTH || attempt == 3)
            return;
        buffer.resize(1 + buffer[0] + buffer[0] / 8 + 1'024);
    }
    const auto* info = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
    std::vector<std::uintptr_t> pages;
    pages.reserve(info->NumberOfEntries);
    for (ULONG_PTR i = 0; i < info->NumberOfEntries; ++i)
        pages.push_back(info->WorkingSetInfo[i].VirtualPage);
    std::ranges::sort(pages);
    std::vector<Range> ranges;
    for (const std::uintptr_t page : pages) {
        const std::uintptr_t start = page * kPage;
        if (!ranges.empty() && ranges.back().start + ranges.back().bytes == start)
            ranges.back().bytes += kPage;
        else if (ranges.empty() || ranges.back().start + ranges.back().bytes < start)
            ranges.push_back({start, kPage});
    }
    // A large index searched as the window came (the words kept from last
    // time) puts GBs of its own pages in the working set; read back at each
    // press, they would push other programs out (FileIndex::warm leaves a
    // large index to demand paging too). Over kMostBytes the smallest ranges
    // are kept, scattered pages (heap, code) that cost a fault each, and the
    // largest are left to Windows.
    std::size_t total = 0;
    for (const Range& r : ranges)
        total += r.bytes;
    if (total > kMostBytes) {
        std::ranges::stable_sort(ranges, {}, &Range::bytes);
        std::size_t kept = 0;
        std::size_t count = 0;
        while (count < ranges.size() && kept + ranges[count].bytes <= kMostBytes)
            kept += ranges[count++].bytes;
        ranges.resize(count);
        std::ranges::sort(ranges, {}, &Range::start);
    }
    const std::lock_guard lock(m_mutex);
    m_ranges = std::move(ranges);
}

std::size_t HotPages::prefetch() const
{
    std::vector<WIN32_MEMORY_RANGE_ENTRY> entries;
    std::size_t total = 0;
    {
        const std::lock_guard lock(m_mutex);
        entries.reserve(m_ranges.size());
        // Freed or made inaccessible since: left out, a range split where
        // its parts differ.
        for (const Range& r : m_ranges) {
            std::uintptr_t at = r.start;
            const std::uintptr_t end = r.start + r.bytes;
            while (at < end) {
                MEMORY_BASIC_INFORMATION m {};
                if (!::VirtualQuery(reinterpret_cast<const void*>(at), &m, sizeof m))
                    break;
                const std::uintptr_t regionEnd = reinterpret_cast<std::uintptr_t>(m.BaseAddress) + m.RegionSize;
                const std::uintptr_t to = std::min(end, regionEnd);
                if (readable(m)) {
                    entries.push_back({reinterpret_cast<void*>(at), static_cast<SIZE_T>(to - at)});
                    total += to - at;
                }
                at = to;
            }
        }
    }
    if (entries.empty())
        return 0;
    ::PrefetchVirtualMemory(::GetCurrentProcess(), entries.size(), entries.data(), 0);
    return total;
}

std::size_t HotPages::bytes() const
{
    const std::lock_guard lock(m_mutex);
    std::size_t total = 0;
    for (const Range& r : m_ranges)
        total += r.bytes;
    return total;
}

} // namespace ws
