#pragma once

#include <QString>
#include <qnamespace.h>

#include <windows.h>

#include <atomic>
#include <bitset>
#include <functional>
#include <thread>

namespace ws {

// Marks the input Win顺 sends itself (SendInput's dwExtraInfo): the paste
// keys, KeyRouter's masking key. KeyRouter lets it through.
inline constexpr ULONG_PTR kOwnInput = 0x5753'4B52; // "WSKR"

// The clipboard window over another program, the way Windows' own Win+V
// panel is: that program keeps the foreground and its text field the
// keyboard, so whatever closes when it loses the focus stays open (VS Code's
// command palette, a browser's address bar suggestions, an autocomplete
// list, an input method's composition), and the paste lands where the caret
// still is. The window is shown without being activated (WindowFrame::
// setNoActivate); the keys it uses are taken from the keyboard here, before
// that program sees them, and handed to it.
//
// A low-level keyboard hook, which every key press in the system waits on:
// it lives only while the window is up, on a thread of its own, and decides
// at once from the key alone. It takes only what the window acts on (typing
// for its search, arrows, Enter, Esc, Tab, Delete, Alt+1…9, Alt+P, the Ctrl
// keys of a text field and of the list; Ctrl+1…9 in their place when the
// settings say so) and passes on the rest: Win
// combinations, Alt+Tab, media keys, F keys, other programs' hotkeys. Keys
// pressed before it started are left alone up to their release.
//
// Shift, Ctrl and Alt are passed on too (Alt+Tab, other programs' hotkeys,
// a Ctrl+click on the window need them), and seen here: the program must
// not take them for pressed alone. Alt alone opens the menu bar (VS Code's
// takes the focus from the command palette), Shift alone switches a Chinese
// input method between Chinese and English. So while one is held, a key
// taken is replaced by one that means nothing (vkE8, as AutoHotkey masks
// the Start menu), and a Shift or Alt let go goes on to the program only
// after such a key. A double Ctrl stays a double Ctrl.
//
// Clicks are not taken (the window does not activate on them); a button
// pressed outside Win顺's windows, or another window coming to the front,
// tells the window to go, as a menu would.
//
// Done (drain), it takes no more keys, but stays until the keys it took are
// let go: their releases are its too (Enter pasted, Esc closed), and would
// reach the program alone otherwise.
class KeyRouter {
public:
    struct Key {
        int key = 0; // Qt::Key
        // Held before the key, as QKeyEvent wants them: its modifiers() then
        // counts a modifier's own press in and its release out.
        Qt::KeyboardModifiers modifiers;
        QString text; // typed, for a text field
        quint32 vk = 0;
        quint32 scanCode = 0;
        bool press = true;
        bool autoRepeat = false;
    };
    struct Callbacks {
        std::function<void(const Key&)> key; // taken, or Shift, Ctrl or Alt (seen, passed on)
        std::function<void()> clickedAway; // a mouse button pressed outside Win顺's windows
        std::function<void(HWND)> foregroundChanged; // the top-level window now in front
        std::function<void()> drained; // after drain(): the keys it took are all let go
    };

    // Keys are taken while `target` (top level) is in front and `window` is
    // shown. The callbacks run on the router's thread: post to the GUI thread.
    // `numberKeys`: the modifier that pastes a row with a digit (Alt, Ctrl,
    // or none: Settings::clipboardNumberKeys).
    KeyRouter(HWND target, HWND window, Callbacks callbacks, Qt::KeyboardModifier numberKeys = Qt::AltModifier);
    ~KeyRouter();

    KeyRouter(const KeyRouter&) = delete;
    KeyRouter& operator=(const KeyRouter&) = delete;

    bool isActive() const noexcept { return m_active.load(); }
    void drain(); // no more keys taken, no more callbacks but `drained`

private:
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);
    static void CALLBACK foregroundProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child,
                                        DWORD thread, DWORD time);
    bool handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event); // true: taken
    bool handleModifier(bool press, const KBDLLHOOKSTRUCT& event);
    void handleMouse(WPARAM message, const MSLLHOOKSTRUCT& event);
    bool taking() const; // the target in front, the window up and answering
    void checkDrained();
    Key translate(const KBDLLHOOKSTRUCT& event, bool press, bool autoRepeat) const;

    const HWND m_target;
    const HWND m_window;
    const Qt::KeyboardModifier m_numberKeys;
    const DWORD m_process;
    Callbacks m_callbacks;
    std::bitset<256> m_taken; // keys held whose press was taken; only touched on the router thread
    std::atomic<bool> m_draining {false}; // takes effect at once, from the GUI thread
    std::atomic<DWORD> m_threadId {0};
    std::atomic<bool> m_active {false};
    std::jthread m_thread;
};

} // namespace ws
