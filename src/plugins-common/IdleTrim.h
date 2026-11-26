// Giving memory back while idle, for the C++ plugins on the single-header SDK
// (src/plugins-sdk/winshun.h), which have no Qt: the app's own is
// src/app/platform/IdleTrim. What a query or a pass paged in stays in the
// working set long after it. Idle for kIdleMs, and grown by more than kAbove
// since the last trim, the working set goes to Windows' standby list: nothing
// is freed, and soft faults bring back what the next query reads (a few ms
// for tens of MB).
//
// Not while Windows itself is short of memory (kShortOfMemoryLoad, the same
// mark as the app's): given back then, the pages are taken by the other
// programs within minutes, and the next query reads them back from the disk
// one at a time (GitHub #471). Windows trims every program by itself when it
// needs the memory.
//
// A plugin starts it once (trimWhenIdle(), from initialized()) and holds a
// Working on the stack of each query, action and pass.
#pragma once

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace wsplugin {

namespace idle {

inline constexpr ULONGLONG kIdleMs = 20'000;
inline constexpr std::chrono::seconds kRecheck {30};
// Less is noise: the threads' stacks, the pipe's buffers. More let an index
// smaller than it stay in for good (GitHub #478: the archive plugin's 16 MB,
// under the 24 MB it waited for).
inline constexpr SIZE_T kAbove = SIZE_T {4} << 20;
// Short of memory: this much of it in use, in percent (Task Manager's figure).
inline constexpr DWORD kShortOfMemoryLoad = 80;

inline std::atomic<int> working {0}; // queries, actions and passes under way
inline std::atomic<ULONGLONG> lastUse {0}; // GetTickCount64 when one last started or ended

// Task Manager's Memory column, the private working set. Windows tells it from
// Windows 11 22H2 on; before, the call succeeds and leaves it 0, and a plugin
// that waited for it to grow never gave anything back on Windows 10 (GitHub
// #478). There the whole working set stands in: read the same way before and
// after a trim, the comparison holds all the same. SIZE_MAX: not told at all.
inline SIZE_T workingSet()
{
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        counters = {}; // in case an older Windows refuses the larger size
        counters.cb = sizeof(PROCESS_MEMORY_COUNTERS);
        if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                sizeof(PROCESS_MEMORY_COUNTERS)))
            return SIZE_MAX;
    }
    return counters.PrivateWorkingSetSize != 0 ? counters.PrivateWorkingSetSize : counters.WorkingSetSize;
}

inline bool shortOfMemory()
{
    MEMORYSTATUSEX memory {sizeof memory};
    return ::GlobalMemoryStatusEx(&memory) && memory.dwMemoryLoad >= kShortOfMemoryLoad;
}

} // namespace idle

// A query, an action or a pass under way: nothing is trimmed from under it,
// and the idle time counts from its end.
class Working {
public:
    Working()
    {
        ++idle::working;
        idle::lastUse = ::GetTickCount64();
    }
    ~Working()
    {
        idle::lastUse = ::GetTickCount64();
        --idle::working;
    }
    Working(const Working&) = delete;
    Working& operator=(const Working&) = delete;
};

// Watches, on a thread of its own, for as long as the process runs.
inline void trimWhenIdle()
{
    idle::lastUse = ::GetTickCount64();
    std::thread([] {
        SIZE_T floor = 0; // what stayed after the last trim
        for (;;) {
            std::this_thread::sleep_for(idle::kRecheck);
            if (idle::working.load() > 0 || ::GetTickCount64() - idle::lastUse.load() < idle::kIdleMs)
                continue;
            if (const SIZE_T now = idle::workingSet(); now != SIZE_MAX && now <= floor + idle::kAbove)
                continue;
            if (idle::shortOfMemory())
                continue; // until there is room again
            ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
            const SIZE_T after = idle::workingSet();
            floor = after == SIZE_MAX ? 0 : after;
        }
    }).detach(); // ends with the process
}

} // namespace wsplugin
