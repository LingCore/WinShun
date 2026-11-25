// The word winshun.topmost says on a window as it goes on top or comes off
// it: a small pill at the top of that window for a second, on a thread of its
// own (the protocol thread never draws). It takes no click and no focus.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace topmost {

// Win顺's 界面风格 (appearance().nativeLook, docs/plugins.md "外观"); a colour
// 0 (an older Win顺 sends none): the pill's own, by `dark`.
struct Look {
    bool rounded = true;
    bool dark = false;
    std::uint32_t accent = 0; // 0xAARRGGBB, for lines
    std::uint32_t accentFill = 0; // under white words
    std::uint32_t dangerFill = 0;
    std::uint32_t background = 0;
    std::uint32_t edge = 0;
    std::uint32_t text = 0;
};

enum class Tone {
    On, // on top now: the accent
    Off, // no longer: quiet
    Problem, // could not be done
};

// "#RRGGBB" or "#AARRGGBB" as 0xAARRGGBB; 0 for anything else.
std::uint32_t argbOf(const std::string& name);

class Note {
public:
    Note();
    ~Note();
    Note(const Note&) = delete;
    Note& operator=(const Note&) = delete;

    void setLook(const Look& look);
    // Over `window` (none: at the top of the monitor with the pointer). A
    // newer one takes the place of the one showing.
    void show(HWND window, Tone tone, std::wstring text);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace topmost
