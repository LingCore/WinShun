#include "ProcessUsage.h"

#include <algorithm>
#include <cwchar>
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

} // namespace

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
