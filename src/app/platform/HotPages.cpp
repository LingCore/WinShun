#include "HotPages.h"

#include <algorithm>
#include <utility>

#include <windows.h>
#include <psapi.h>

namespace ws {

namespace {

// Protections Windows cannot or need not read back: nothing behind them.
bool readable(const MEMORY_BASIC_INFORMATION& m)
{
    constexpr DWORD kNot = PAGE_NOACCESS | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;
    return m.State == MEM_COMMIT && m.Protect != 0 && !(m.Protect & kNot);
}

} // namespace

PageSet::PageSet(std::vector<Range> ranges)
    : m_ranges(std::move(ranges))
{
    std::erase_if(m_ranges, [](const Range& r) { return r.bytes == 0; });
    std::ranges::sort(m_ranges, {}, &Range::start);
    auto out = m_ranges.begin();
    for (auto in = m_ranges.begin(); in != m_ranges.end(); ++in) {
        if (out != m_ranges.begin() && in->start <= std::prev(out)->end())
            std::prev(out)->bytes = std::max(std::prev(out)->end(), in->end()) - std::prev(out)->start;
        else
            *out++ = *in;
    }
    m_ranges.erase(out, m_ranges.end());
    for (const Range& r : m_ranges)
        m_bytes += r.bytes;
}

PageSet PageSet::within(std::span<const std::pair<const void*, std::size_t>> blocks)
{
    std::vector<Range> ranges;
    ranges.reserve(blocks.size());
    for (const auto& [data, size] : blocks) {
        const auto from = reinterpret_cast<std::uintptr_t>(data);
        const std::uintptr_t start = (from + kPage - 1) / kPage * kPage;
        const std::uintptr_t end = (from + size) / kPage * kPage;
        if (start < end)
            ranges.push_back({start, end - start});
    }
    return PageSet(std::move(ranges));
}

PageSet PageSet::resident()
{
    // Asked again with room for what it said it holds (it grows meanwhile).
    std::vector<ULONG_PTR> buffer(1 + 16'384);
    for (int attempt = 0;; ++attempt) {
        if (::QueryWorkingSet(::GetCurrentProcess(), buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(ULONG_PTR))))
            break;
        if (::GetLastError() != ERROR_BAD_LENGTH || attempt == 3)
            return {};
        buffer.resize(1 + buffer[0] + buffer[0] / 8 + 1'024);
    }
    const auto* info = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
    std::vector<Range> pages;
    pages.reserve(info->NumberOfEntries);
    for (ULONG_PTR i = 0; i < info->NumberOfEntries; ++i)
        pages.push_back({static_cast<std::uintptr_t>(info->WorkingSetInfo[i].VirtualPage) * kPage, kPage});
    return PageSet(std::move(pages));
}

PageSet PageSet::united(const PageSet& other) const
{
    std::vector<Range> all;
    all.reserve(m_ranges.size() + other.m_ranges.size());
    all.insert(all.end(), m_ranges.begin(), m_ranges.end());
    all.insert(all.end(), other.m_ranges.begin(), other.m_ranges.end());
    return PageSet(std::move(all));
}

PageSet PageSet::without(const PageSet& other) const
{
    PageSet left;
    left.m_ranges.reserve(m_ranges.size());
    auto cut = other.m_ranges.begin();
    for (const Range& r : m_ranges) {
        std::uintptr_t at = r.start;
        while (cut != other.m_ranges.end() && cut->end() <= at)
            ++cut;
        // `cut` stays on a range that reaches past this one's end: the next may begin inside it.
        for (auto c = cut; at < r.end(); ++c) {
            const std::uintptr_t to = c != other.m_ranges.end() ? std::min(c->start, r.end()) : r.end();
            if (at < to)
                left.m_ranges.push_back({at, to - at});
            if (c == other.m_ranges.end() || c->start >= r.end())
                break;
            at = std::max(at, c->end());
        }
    }
    for (const Range& r : left.m_ranges)
        left.m_bytes += r.bytes;
    return left;
}

PageSet PageSet::intersected(const PageSet& other) const
{
    PageSet both;
    auto a = m_ranges.begin();
    auto b = other.m_ranges.begin();
    while (a != m_ranges.end() && b != other.m_ranges.end()) {
        const std::uintptr_t from = std::max(a->start, b->start);
        const std::uintptr_t to = std::min(a->end(), b->end());
        if (from < to) {
            both.m_ranges.push_back({from, to - from});
            both.m_bytes += to - from;
        }
        (a->end() < b->end() ? a : b)++;
    }
    return both;
}

PageSet PageSet::smallest(std::size_t bytes) const
{
    if (m_bytes <= bytes)
        return *this;
    std::vector<Range> bySize = m_ranges;
    std::ranges::stable_sort(bySize, {}, &Range::bytes);
    std::size_t kept = 0;
    std::size_t count = 0;
    while (count < bySize.size() && kept + bySize[count].bytes <= bytes)
        kept += bySize[count++].bytes;
    bySize.resize(count);
    return PageSet(std::move(bySize));
}

void PageSet::release() const
{
    // VirtualUnlock on pages that are not locked takes them out of the
    // working set (and says ERROR_NOT_LOCKED): over several allocations at
    // once, past pages freed since, leaving guard pages as they are; ~0.6 µs
    // a call and ~7 ms per 100 MB (measured 2026-11-12). On locked ones it
    // would unlock them: every page is asked about first, a batch at a time,
    // and the runs between the locked ones go. A batch that cannot be asked
    // about stays.
    constexpr std::size_t kBatch = 4'096;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> asked;
    asked.reserve(kBatch);
    const auto out = [](std::uintptr_t from, std::uintptr_t to) {
        if (from < to)
            ::VirtualUnlock(reinterpret_cast<void*>(from), to - from);
    };
    for (const Range& r : m_ranges) {
        for (std::uintptr_t from = r.start; from < r.end();) {
            asked.clear();
            for (std::uintptr_t at = from; at < r.end() && asked.size() < kBatch; at += kPage)
                asked.push_back({reinterpret_cast<void*>(at), {}});
            const std::uintptr_t to = from + asked.size() * kPage;
            if (::QueryWorkingSetEx(::GetCurrentProcess(), asked.data(), static_cast<DWORD>(asked.size() * sizeof asked[0]))) {
                std::uintptr_t run = from;
                for (std::size_t i = 0; i < asked.size(); ++i) {
                    if (asked[i].VirtualAttributes.Valid && asked[i].VirtualAttributes.Locked) {
                        out(run, from + i * kPage);
                        run = from + (i + 1) * kPage;
                    }
                }
                out(run, to);
            }
            from = to;
        }
    }
}

std::size_t PageSet::prefetch() const
{
    std::vector<WIN32_MEMORY_RANGE_ENTRY> entries;
    entries.reserve(m_ranges.size());
    std::size_t total = 0;
    // Freed or made inaccessible since: left out, a range split where its
    // parts differ.
    for (const Range& r : m_ranges) {
        std::uintptr_t at = r.start;
        while (at < r.end()) {
            MEMORY_BASIC_INFORMATION m {};
            if (!::VirtualQuery(reinterpret_cast<const void*>(at), &m, sizeof m))
                break;
            const std::uintptr_t regionEnd = reinterpret_cast<std::uintptr_t>(m.BaseAddress) + m.RegionSize;
            const std::uintptr_t to = std::min(r.end(), regionEnd);
            if (readable(m)) {
                entries.push_back({reinterpret_cast<void*>(at), static_cast<SIZE_T>(to - at)});
                total += to - at;
            }
            at = to;
        }
    }
    if (entries.empty())
        return 0;
    ::PrefetchVirtualMemory(::GetCurrentProcess(), entries.size(), entries.data(), 0);
    return total;
}

void HotPages::learn(const PageSet& used)
{
    const std::lock_guard lock(m_mutex);
    if (m_pages.empty()) {
        m_pages = m_first = used.smallest(kMostBytes);
        m_once = {};
        m_whole = m_pages.bytes() == used.bytes();
        return;
    }
    if (!m_whole)
        return; // what was left out then would come in now, and be too much again
    m_pages = m_pages.united(used.intersected(m_once));
    m_once = used.without(m_pages);
    if (m_pages.bytes() > m_first.bytes() + std::max(m_first.bytes(), kSlackBytes)) {
        m_pages = m_first;
        m_once = {};
    }
}

PageSet HotPages::pages() const
{
    const std::lock_guard lock(m_mutex);
    return m_pages;
}

std::size_t HotPages::bytes() const
{
    const std::lock_guard lock(m_mutex);
    return m_pages.bytes();
}

} // namespace ws
