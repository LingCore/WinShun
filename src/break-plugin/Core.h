// What the two threads of WinShunBreak.exe share: the break clock behind a
// lock. The protocol thread (main.cpp, serve()) reads it for the launcher
// rows and acts on Enter; the window thread (Ui.cpp) ticks it and shows the
// notice and the break screen.
#pragma once

#include "Schedule.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace brk {

struct Core {
    std::mutex mutex;
    Schedule schedule; // under `mutex`
    bool chinese = true; // under `mutex`, as Win顺's language and theme change
    bool light = true; // the notice follows Win顺's theme; the break screen is always dark
    std::atomic<bool> quietRule {true}; // hold breaks back during full-screen programs and Do not disturb
    std::function<void(const std::string&)> log; // to plugins.log; any thread

    static std::int64_t now() { return static_cast<std::int64_t>(::GetTickCount64()); }
    static std::int64_t wall()
    {
        FILETIME ft {};
        ::GetSystemTimeAsFileTime(&ft);
        const std::int64_t t = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return t / 10'000 - 11'644'473'600'000; // 100 ns since 1601 to Unix ms
    }
};

} // namespace brk
