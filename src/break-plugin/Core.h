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

// The notice's look from Win顺's 界面风格 (appearance().nativeLook, see
// docs/plugins.md, "外观"). A colour 0 (an older Win顺 sends none): the
// notice's own, by `light`. The break screen keeps its own dim and white.
struct Look {
    bool rounded = true; // false: the 直角 setting, every corner square
    std::uint32_t background = 0; // 0xAARRGGBB
    std::uint32_t edge = 0;
    std::uint32_t ink = 0;
    std::uint32_t button = 0; // a button's fill at rest (tagFill); hovered, twice as strong
    bool operator==(const Look&) const = default;
};

// "#RRGGBB" or "#AARRGGBB" (QColor's names) as 0xAARRGGBB; 0 for anything else.
inline std::uint32_t argbOf(const std::string& name)
{
    if ((name.size() != 7 && name.size() != 9) || name[0] != '#')
        return 0;
    std::uint32_t value = 0;
    for (std::size_t i = 1; i < name.size(); ++i) {
        const char c = name[i];
        const int digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f'             ? c - 'a' + 10
            : c >= 'A' && c <= 'F'             ? c - 'A' + 10
                                               : -1;
        if (digit < 0)
            return 0;
        value = value << 4 | static_cast<std::uint32_t>(digit);
    }
    return name.size() == 7 ? 0xFF000000u | value : value;
}

// The notice's look from Win顺's appearance (winshun::Json, or a test's
// stand-in): the corners setting (every Win顺 sends it), the 界面风格's
// colours when it sends them (nativeLook; an older Win顺 does not) and
// they are for the theme in use (`light`).
template <class Json>
Look lookOf(const Json& appearance, bool light)
{
    Look look;
    look.rounded = appearance["corners"].toString() != "square";
    const Json& native = appearance["nativeLook"];
    if (!native.isObject())
        return look;
    if (native["rounded"].isBool())
        look.rounded = native["rounded"].toBool();
    if (native["dark"].isBool() && native["dark"].toBool() == light)
        return look; // written for the other theme, a moment before it changes
    look.background = argbOf(native["background"].toString());
    look.edge = argbOf(native["windowEdge"].toString());
    look.ink = argbOf(native["text"].toString());
    look.button = argbOf(native["tagFill"].toString());
    return look;
}

struct Core {
    std::mutex mutex;
    Schedule schedule; // under `mutex`
    bool chinese = true; // under `mutex`, as Win顺's language and theme change
    bool light = true; // the notice follows Win顺's theme; the break screen is always dark
    Look look; // under `mutex`, with `light`
    std::atomic<bool> quietRule {true}; // hold breaks back during full-screen programs and Do not disturb
    std::function<void(const std::string&)> log; // to plugins.log; any thread
    std::function<void()> pauseChanged; // keeps a pause made on the notice or the break screen (main.cpp); not under `mutex`

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
