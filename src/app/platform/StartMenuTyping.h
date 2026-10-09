#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace ws {

// Typing in the Start menu searches with Win顺 instead of Windows (a
// setting, off by default; EverythingToolbar does the same).
//
// Windows' search takes over the Start menu as soon as one types there. Here
// the first key that types something while the Start menu (or Windows'
// search) is in front is taken from it, and the launcher is asked for: it
// opens over the taskbar. That key and the ones typed after it are held back
// until the launcher has the keyboard (deliver), then typed again in order
// (SendInput, marked as Win顺's own): they go through the input method as if
// typed into the search box.
//
// A low-level keyboard hook, which every key press in the system waits on
// (see KeyListener for why that matters): installed only while the Start
// menu or Windows' search is in front, as an out-of-context event on the
// foreground tells, and while keys are held back; on a thread of its own,
// deciding at once from the key alone. Win, Ctrl and Alt combinations stay
// Windows'. Left Alt pressed in the Start menu leaves its search alone until
// it closes: a way to Windows' own search when it is wanted.
class StartMenuTyping {
public:
    struct Callbacks {
        std::function<void()> typed; // the first key was taken: show the launcher. On the hook's thread.
    };

    explicit StartMenuTyping(Callbacks callbacks);
    ~StartMenuTyping();

    StartMenuTyping(const StartMenuTyping&) = delete;
    StartMenuTyping& operator=(const StartMenuTyping&) = delete;

    // The launcher has the keyboard: the keys held back are typed into it.
    // Any thread.
    void deliver();

private:
    struct Held {
        WORD vk = 0;
        WORD scanCode = 0;
        bool extended = false;
        bool release = false;
        bool shift = false; // held with Shift (its press)
    };

    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static void CALLBACK foregroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD);
    bool handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event); // true: taken
    void foregroundChanged(HWND window);
    void setHooked(bool hooked);
    void typeHeld(); // deliver(), on the hook's thread
    void dropHeld(); // the launcher never came

    Callbacks m_callbacks;
    std::atomic<DWORD> m_threadId = 0;

    // The hook's thread only.
    HHOOK m_hook = nullptr;
    bool m_startInFront = false;
    bool m_windowsSearch = false; // Left Alt in the Start menu: its own search this time
    bool m_holding = false; // keys are held back for the launcher
    std::vector<Held> m_held;
    UINT_PTR m_timer = 0; // while holding: gives up on the launcher

    std::jthread m_thread; // last: joined before the members above go
};

} // namespace ws
