#pragma once

#include <qnamespace.h>

#include <windows.h>

#include <atomic>
#include <bitset>
#include <functional>
#include <thread>

namespace ws {

// While the settings window records a shortcut, its key presses are taken
// straight from the keyboard, the way PowerToys' shortcut boxes do it: a
// low-level keyboard hook sees them before Windows, Qt or any other program
// acts on them, and keeps them from all three. Otherwise Qt keeps Alt+Space
// to itself (it opens the system menu), Alt+F4 closes the window, Win+E
// opens Explorer, and a combination another program registered (PowerToys
// Run and Copilot like Alt+Space) starts that program instead of being
// recorded. Recorded, it fails to register and the settings say it is taken.
//
// Every key press in the system waits on such a hook, which is why the
// double Ctrl uses Raw Input instead (see KeyListener). This one lives only
// while a shortcut is being recorded, and passes keys straight on unless the
// recording window is in the foreground.
class ShortcutCapture {
public:
    struct Key {
        int key; // Qt::Key
        Qt::KeyboardModifiers modifiers; // as Qt reports them: a modifier's own press includes it
        bool press;
        bool autoRepeat;
    };

    // `onKey` runs on the capture thread; post to the GUI thread from it.
    ShortcutCapture(HWND window, std::function<void(const Key&)> onKey);
    ~ShortcutCapture();

    ShortcutCapture(const ShortcutCapture&) = delete;
    ShortcutCapture& operator=(const ShortcutCapture&) = delete;

    bool isActive() const noexcept { return m_active.load(); }

private:
    static LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam);
    bool handle(WPARAM message, const KBDLLHOOKSTRUCT& event); // true: swallowed
    bool isDown(UINT vk) const;
    Qt::KeyboardModifiers modifiers() const;

    HWND m_window;
    std::function<void(const Key&)> m_onKey;
    std::bitset<256> m_taken; // keys held whose press was swallowed; only touched on the capture thread
    std::atomic<DWORD> m_threadId {0};
    std::atomic<bool> m_active {false};
    std::jthread m_thread;
};

} // namespace ws
