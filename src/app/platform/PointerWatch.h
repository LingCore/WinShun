#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace ws {

// Whether the pointer, hidden now, was hidden by Windows on a key press
// ("hide pointer while typing", SPI_GETMOUSEVANISH) rather than by the
// program in front, which would have taken the mouse (GameGuard,
// hiddenForTyping; GitHub #111). Follows the window in front
// (EVENT_SYSTEM_FOREGROUND) and, in its process alone, the pointer hidden
// and shown again (EVENT_OBJECT_HIDE and SHOW for OBJID_CURSOR): a few
// events while typing there, none at all otherwise. Never the pointer's
// moves (EVENT_OBJECT_LOCATIONCHANGE would wake it at each).
//
// Whatever it misses counts as the program's: a hide before the watch
// began or before the window came to the front, one Windows does not tell
// (an elevated program's, UIPI), a pointer moved since. On the GUI thread.
class PointerWatch {
public:
    // The last press of a key other than a modifier at or before a time, on
    // the 32-bit tick clock (KeyListener's Raw Input); none: as if no key
    // had been pressed, every hide is the program's.
    using KeyAt = std::function<std::optional<std::uint32_t>(std::uint32_t atOrBefore)>;

    explicit PointerWatch(KeyAt keyAt);
    ~PointerWatch();

    PointerWatch(const PointerWatch&) = delete;
    PointerWatch& operator=(const PointerWatch&) = delete;

    // Hidden by Windows for typing in the window in front now, and not
    // moved since (a move would have brought it back). Found so when the
    // hide was told, or failing that now: the press before it may have been
    // seen after it, this thread or the listener's behind (GitHub #111).
    bool hiddenByTyping();

private:
    static void CALLBACK onEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child, DWORD thread,
        DWORD time);
    void foregroundChanged(HWND window, std::uint32_t time);
    void cursorChanged(DWORD event, std::uint32_t time);
    void watchProcess(DWORD pid);

    struct Hide {
        std::uint32_t at = 0;
        POINT where {};
        bool vanish = false; // the setting on and no mouse button held at the hide: Windows' maybe
        bool typing = false; // Windows' for a key press: never undone by a press found later
    };

    KeyAt m_keyAt;
    HWINEVENTHOOK m_foregroundHook = nullptr;
    HWINEVENTHOOK m_cursorHook = nullptr;
    DWORD m_pid = 0; // whose pointer events come
    HWND m_foreground = nullptr;
    std::uint32_t m_foregroundAt = 0;
    std::optional<Hide> m_hide; // the last one in this foreground, until shown again
    static PointerWatch* s_watch; // WinEvent callbacks carry no context: one at a time
};

} // namespace ws
