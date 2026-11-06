#include "ProcessUsage.h"

#include <algorithm>
#include <cwchar>
#include <string>
#include <utility>

#include <windows.h>
#include <psapi.h>

namespace ws::memory {

namespace {

// SYSTEM_PROCESS_INFORMATION as Windows lays it out (winternl.h declares only
// part of it; the fields read here are stable since Windows 7).
struct UnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct SystemProcessInformation {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UnicodeString ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
};

using QuerySystemInformation = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemProcessInformation = 5;
constexpr LONG kInfoLengthMismatch = static_cast<LONG>(0xC0000004);

QuerySystemInformation query()
{
    static const auto f = reinterpret_cast<QuerySystemInformation>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    return f;
}

bool winShun(const UnicodeString& name)
{
    constexpr wchar_t kPrefix[] = L"WinShun";
    constexpr std::size_t kLength = 7;
    return name.Buffer && name.Length / sizeof(wchar_t) >= kLength && ::_wcsnicmp(name.Buffer, kPrefix, kLength) == 0;
}

// PROCESS_MEMORY_COUNTERS_EX2 (Windows 11 22H2), spelled out: older SDKs lack it.
struct MemoryCountersEx2 {
    PROCESS_MEMORY_COUNTERS_EX base;
    SIZE_T privateWorkingSetSize;
    ULONG64 sharedCommitUsage;
};

std::uint64_t fileTime(const FILETIME& t)
{
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

// One known process again; false if it is not the same one or a number is not told.
bool readAgain(HANDLE h, Process& p)
{
    FILETIME created, exited, kernel, user;
    if (!::GetProcessTimes(h, &created, &exited, &kernel, &user)
        || static_cast<std::int64_t>(fileTime(created)) != p.created)
        return false;
    IO_COUNTERS io {};
    MemoryCountersEx2 memory {};
    memory.base.cb = sizeof(memory);
    if (!::GetProcessIoCounters(h, &io)
        || !::GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))
        || memory.privateWorkingSetSize == 0) // not told (older Windows): never really 0
        return false;
    p.cpu = fileTime(kernel) + fileTime(user);
    p.readBytes = io.ReadTransferCount;
    p.privateBytes = memory.privateWorkingSetSize;
    return true;
}

} // namespace

Look again(const std::vector<Process>& known)
{
    Look out;
    out.self = ::GetCurrentProcessId();
    FILETIME now;
    ::GetSystemTimeAsFileTime(&now);
    out.atFt = static_cast<std::int64_t>(fileTime(now));
    out.atMs = (out.atFt - 116'444'736'000'000'000LL) / 10'000;
    out.processes = known;
    for (Process& p : out.processes) {
        if (p.pid == out.self) {
            if (!readAgain(::GetCurrentProcess(), p))
                return {};
            continue;
        }
        const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, p.pid);
        if (!h)
            return {};
        const bool read = readAgain(h, p);
        ::CloseHandle(h);
        if (!read)
            return {};
    }
    out.ok = true;
    return out;
}

Look look(std::size_t& sizeHint)
{
    Look out;
    out.self = ::GetCurrentProcessId();
    const QuerySystemInformation f = query();
    if (!f)
        return out;
    std::size_t size = std::max<std::size_t>(sizeHint, 1u << 20);
    for (int attempt = 0; attempt < 4; ++attempt) {
        // The system's pages, given back below: not left in the heap's working set.
        auto* buffer = static_cast<unsigned char*>(::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!buffer)
            return out;
        ULONG needed = 0;
        FILETIME now;
        ::GetSystemTimeAsFileTime(&now);
        const LONG status = f(kSystemProcessInformation, buffer, static_cast<ULONG>(size), &needed);
        if (status == kInfoLengthMismatch) {
            ::VirtualFree(buffer, 0, MEM_RELEASE);
            size = std::max<std::size_t>(needed + needed / 8, size * 2);
            continue;
        }
        if (status >= 0) {
            sizeHint = std::max<std::size_t>(needed + needed / 8, 1u << 20);
            out.atFt = (static_cast<std::int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            out.atMs = (out.atFt - 116'444'736'000'000'000LL) / 10'000; // 1601 → 1970
            for (std::size_t offset = 0;;) {
                const auto* p = reinterpret_cast<const SystemProcessInformation*>(buffer + offset);
                const auto pid = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(p->UniqueProcessId));
                if (pid == out.self || winShun(p->ImageName)) {
                    Process one;
                    one.pid = pid;
                    one.parent = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(p->InheritedFromUniqueProcessId));
                    one.created = p->CreateTime.QuadPart;
                    one.privateBytes = static_cast<std::uint64_t>(p->WorkingSetPrivateSize.QuadPart);
                    if (pid == out.self) { // not the list itself, in this process's pages only while looking
                        const std::uint64_t list = (static_cast<std::uint64_t>(needed) + 4095) & ~std::uint64_t(4095);
                        one.privateBytes -= std::min(one.privateBytes, list);
                    }
                    one.cpu = static_cast<std::uint64_t>(p->UserTime.QuadPart + p->KernelTime.QuadPart);
                    one.readBytes = static_cast<std::uint64_t>(p->ReadTransferCount.QuadPart);
                    const std::size_t chars = std::min<std::size_t>(p->ImageName.Length / sizeof(wchar_t), std::size(one.exe) - 1);
                    if (p->ImageName.Buffer)
                        std::wmemcpy(one.exe, p->ImageName.Buffer, chars);
                    one.exe[chars] = L'\0';
                    out.processes.push_back(one);
                }
                if (p->NextEntryOffset == 0)
                    break;
                offset += p->NextEntryOffset;
            }
            out.ok = true;
            out.full = true;
        }
        ::VirtualFree(buffer, 0, MEM_RELEASE);
        return out;
    }
    return out;
}
std::vector<const Process*> family(const std::vector<Process>& all, std::uint32_t root)
{
    std::vector<const Process*> out;
    for (const Process& p : all) {
        if (p.pid == root) {
            out.push_back(&p);
            break;
        }
    }
    for (std::size_t i = 0; i < out.size(); ++i) { // breadth first; `out` grows meanwhile
        const Process* parent = out[i];
        for (const Process& p : all) {
            if (p.parent != parent->pid || p.pid == parent->pid || p.created < parent->created)
                continue;
            if (std::find(out.begin(), out.end(), &p) == out.end())
                out.push_back(&p);
        }
    }
    return out;
}

namespace {

enum class Owner { Own, Qt, Graphics, System };

Owner ownerOf(const void* allocationBase)
{
    wchar_t path[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(static_cast<HMODULE>(const_cast<void*>(allocationBase)), path, MAX_PATH);
    if (n == 0)
        return Owner::System;
    const wchar_t* name = path + n;
    while (name > path && name[-1] != L'\\')
        --name;
    const auto starts = [name](const wchar_t* prefix) { return ::_wcsnicmp(name, prefix, std::wcslen(prefix)) == 0; };
    if (starts(L"WinShun"))
        return Owner::Own;
    static const std::wstring folder = [] { // the program's folder: Qt and the rest it brings
        wchar_t exe[MAX_PATH];
        const DWORD length = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring s(exe, length);
        return s.substr(0, s.find_last_of(L'\\') + 1);
    }();
    if (::_wcsnicmp(path, folder.c_str(), folder.size()) == 0)
        return Owner::Qt; // Qt6*.dll, its QML modules and plugins, and what else came with Win顺
    for (const wchar_t* prefix : {L"nv", L"amd", L"ati", L"ig", L"d3d", L"dxgi", L"dxcore", L"dcomp", L"D3DSCache"}) {
        if (starts(prefix))
            return Owner::Graphics;
    }
    return Owner::System;
}

} // namespace

Map map(std::vector<std::pair<const void*, std::size_t>> fileIndex)
{
    Map out;
    std::sort(fileIndex.begin(), fileIndex.end());
    struct Region {
        std::uintptr_t base;
        std::size_t size;
        DWORD state, protect, type;
    };
    constexpr std::size_t kPage = 4096;
    constexpr std::size_t kStep = 4096;
    static thread_local PSAPI_WORKING_SET_EX_INFORMATION info[kStep];
    const HANDLE self = ::GetCurrentProcess();

    // Pages of [from, to) private and in the working set; those in the file index's blocks apart.
    const auto count = [&](std::uintptr_t from, std::uintptr_t to, std::uint64_t& into, bool index) {
        for (std::uintptr_t at = from; at < to;) {
            const std::size_t n = std::min<std::size_t>(kStep, (to - at) / kPage);
            for (std::size_t i = 0; i < n; ++i)
                info[i].VirtualAddress = reinterpret_cast<void*>(at + i * kPage);
            if (::QueryWorkingSetEx(self, info, static_cast<DWORD>(n * sizeof(info[0])))) {
                for (std::size_t i = 0; i < n; ++i) {
                    const auto& a = info[i].VirtualAttributes;
                    if (!a.Valid || a.Shared)
                        continue;
                    out.total += kPage;
                    if (index) {
                        const auto page = reinterpret_cast<const char*>(at + i * kPage);
                        auto it = std::upper_bound(fileIndex.begin(), fileIndex.end(), std::pair<const void*, std::size_t>(page, SIZE_MAX));
                        if (it != fileIndex.begin()) {
                            --it;
                            const auto* start = static_cast<const char*>(it->first);
                            if (page + kPage > start && page < start + it->second) {
                                out.fileIndex += kPage;
                                continue;
                            }
                        }
                    }
                    into += kPage;
                }
            }
            at += n * kPage;
        }
    };

    std::vector<Region> allocation; // the regions of one allocation, then counted together
    const auto flush = [&] {
        if (allocation.empty())
            return;
        const DWORD type = allocation.front().type;
        std::uint64_t* into = &out.other;
        if (type == MEM_IMAGE) {
            switch (ownerOf(reinterpret_cast<const void*>(allocation.front().base))) {
            case Owner::Own: into = &out.ownProgram; break;
            case Owner::Qt: into = &out.qt; break;
            case Owner::Graphics: into = &out.graphics; break;
            case Owner::System: into = &out.system; break;
            }
        } else if (std::any_of(allocation.begin(), allocation.end(), [](const Region& r) { return (r.protect & PAGE_GUARD) != 0; })) {
            into = &out.stacks; // a thread's stack: reserved, with a guard page below what it uses
        }
        for (const Region& r : allocation) {
            if (r.state == MEM_COMMIT && !(r.protect & PAGE_GUARD))
                count(r.base, r.base + r.size, *into, type == MEM_PRIVATE && into == &out.other);
        }
        allocation.clear();
    };

    const void* lastAllocation = nullptr;
    for (std::uintptr_t at = 0;;) {
        MEMORY_BASIC_INFORMATION m;
        if (::VirtualQuery(reinterpret_cast<const void*>(at), &m, sizeof(m)) == 0)
            break;
        if (m.State != MEM_FREE) {
            if (m.AllocationBase != lastAllocation)
                flush();
            lastAllocation = m.AllocationBase;
            allocation.push_back({reinterpret_cast<std::uintptr_t>(m.BaseAddress), m.RegionSize, m.State, m.Protect, m.Type});
        }
        const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (next <= at)
            break;
        at = next;
    }
    flush();
    return out;
}

std::uint64_t residentBytes(const std::vector<std::pair<const void*, std::size_t>>& blocks)
{
    constexpr std::size_t kPage = 4096;
    constexpr std::size_t kStep = 4096; // pages per call: a 64 KB buffer, not one the size of the index
    PSAPI_WORKING_SET_EX_INFORMATION info[kStep];
    std::uint64_t pages = 0;
    for (const auto& [address, size] : blocks) {
        const auto first = reinterpret_cast<std::uintptr_t>(address) & ~(kPage - 1);
        const auto last = (reinterpret_cast<std::uintptr_t>(address) + size + kPage - 1) & ~(kPage - 1);
        for (std::uintptr_t at = first; at < last;) {
            const std::size_t n = std::min<std::size_t>(kStep, (last - at) / kPage);
            for (std::size_t i = 0; i < n; ++i)
                info[i].VirtualAddress = reinterpret_cast<void*>(at + i * kPage);
            at += n * kPage;
            if (!::QueryWorkingSetEx(::GetCurrentProcess(), info, static_cast<DWORD>(n * sizeof(info[0]))))
                continue; // not told: left out, not guessed
            for (std::size_t i = 0; i < n; ++i) {
                const auto& a = info[i].VirtualAttributes;
                if (a.Valid && !a.Shared) // in the working set and private (what Task Manager counts)
                    ++pages;
            }
        }
    }
    return pages * kPage;
}

} // namespace ws::memory
