#include "MemoryCensus.h"

#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>

namespace ws::memory {

namespace {

// One number per graphics card. A card is listed once for every display path
// to it, each with an id of its own: a laptop with four remote-desktop
// programs' virtual displays lists its one card six times, and each entry
// tells the same use. Entries of the same card (vendor, device, subsystem,
// revision) count once, with the largest of what they tell.
void video(Census& out)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return;
    struct Card {
        UINT vendor, device, subsystem, revision;
        std::uint64_t dedicated, shared;
    };
    std::vector<Card> cards;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIAdapter3> usage;
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && SUCCEEDED(adapter.As(&usage))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local {}, shared {};
            (void)usage->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            (void)usage->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
            const auto same = std::ranges::find_if(cards, [&desc](const Card& card) {
                return card.vendor == desc.VendorId && card.device == desc.DeviceId && card.subsystem == desc.SubSysId
                    && card.revision == desc.Revision;
            });
            if (same == cards.end()) {
                cards.push_back({desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision, local.CurrentUsage, shared.CurrentUsage});
            } else {
                same->dedicated = std::max<std::uint64_t>(same->dedicated, local.CurrentUsage);
                same->shared = std::max<std::uint64_t>(same->shared, shared.CurrentUsage);
            }
        }
        adapter.Reset();
    }
    for (const Card& card : cards) {
        out.videoDedicated += card.dedicated;
        out.videoShared += card.shared;
    }
}
using Ranges = std::vector<std::pair<const std::byte*, const std::byte*>>;

void heaps(Census& out, Ranges& theirs)
{
    std::vector<HANDLE> all(64);
    DWORD count = ::GetProcessHeaps(static_cast<DWORD>(all.size()), all.data());
    if (count > all.size()) {
        all.resize(count);
        count = ::GetProcessHeaps(static_cast<DWORD>(all.size()), all.data());
    }
    all.resize(std::min<std::size_t>(count, all.size()));
    for (HANDLE heap : all) {
        if (!::HeapLock(heap))
            continue;
        PROCESS_HEAP_ENTRY entry {};
        while (::HeapWalk(heap, &entry)) {
            if (entry.wFlags & PROCESS_HEAP_REGION) {
                const auto* first = static_cast<const std::byte*>(entry.lpData);
                theirs.emplace_back(first, first + entry.Region.dwCommittedSize + entry.Region.dwUnCommittedSize);
                out.heapCommitted += entry.Region.dwCommittedSize;
                continue;
            }
            if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY))
                continue;
            const std::uint64_t size = entry.cbData;
            const auto bucket = static_cast<std::size_t>(
                std::ranges::lower_bound(Census::kUpTo, size) - Census::kUpTo.begin());
            ++out.heap[bucket].count;
            out.heap[bucket].bytes += size;
            ++out.heapBlocks;
            out.heapBytes += size;
        }
        ::HeapUnlock(heap);
    }
}

void regions(Census& out, const Ranges& heaps)
{
    const std::byte* at = nullptr;
    const void* base = nullptr;
    std::uint64_t committed = 0;
    bool code = false;
    const auto close = [&] {
        const auto* first = static_cast<const std::byte*>(base);
        const bool heap = std::ranges::any_of(heaps, [first](const auto& range) { return first >= range.first && first < range.second; })
            || std::ranges::any_of(heaps, [first](const auto& range) { return range.first >= first && range.first < first + 65536; });
        if (!heap)
            out.privateOutsideHeaps += committed;
        if (committed >= 1048576)
            out.large.push_back({committed, heap, code});
        committed = 0;
        code = false;
    };
    MEMORY_BASIC_INFORMATION info {};
    while (::VirtualQuery(at, &info, sizeof info) == sizeof info) {
        if (info.AllocationBase != base) {
            close();
            base = info.AllocationBase;
        }
        if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE) {
            committed += info.RegionSize;
            code = code || (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        }
        at = static_cast<const std::byte*>(info.BaseAddress) + info.RegionSize;
    }
    close();
    std::ranges::sort(out.large, std::greater<>(), &Census::Large::bytes);
}

} // namespace

Census census(bool walkHeaps)
{
    Census out;
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof counters;
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters)) {
        out.commit = counters.PrivateUsage;
        out.privateWorkingSet = counters.PrivateWorkingSetSize ? counters.PrivateWorkingSetSize : counters.WorkingSetSize;
    }
    video(out);
    if (walkHeaps) {
        Ranges theirs;
        heaps(out, theirs);
        regions(out, theirs);
    }
    return out;
}

} // namespace ws::memory
