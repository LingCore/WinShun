#pragma once

#include "OwnInput.h" // kOwnInput: the paste keys, KeyRouter's masking key

#include <QJsonObject>
#include <QString>
#include <qnamespace.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <thread>
#include <unordered_map>

namespace ws {

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
// Installed when the window shows, it is the newest hook, called before
// those of a key remapper (AutoHotkey, PowerToys): a key it wants goes to
// them first, and is taken only if none of them took it. Their replacement
// keys come back as keys of their own (GitHub #9).
//
// Shift, Ctrl and Alt are passed on too (Alt+Tab, other programs' hotkeys,
// a Ctrl+click on the window need them), and seen here: the program must
// not take them for pressed alone. Alt alone opens the menu bar (VS Code's
// takes the focus from the command palette), Shift alone switches a Chinese
// input method between Chinese and English. So while one is held, a key
// taken is replaced by one that means nothing (vkE8, as AutoHotkey masks
// the Start menu). Shift and Alt presses are also masked (kOwnMask); their
// original releases always pass through. A double Ctrl or Alt stays one.
//
// No input method composes the keys taken: they reach the window without
// one, and its search finds Chinese by the pinyin typed. A Shift pressed
// and let go alone is still meant for one (Chinese input methods switch to
// Chinese on it): its release goes on masked all the same, and the
// clipboard is told (shiftTapped), which says that pinyin will do (App).
// It never takes the focus for one: what closes without it would close.
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
        // Shift pressed with nothing else held and let go with no other key
        // or click between, as a Chinese input method is switched; told
        // once its release has gone on masked (Policy::Clipboard).
        std::function<void()> shiftTapped;
    };

    // Which keys it takes: the clipboard's (above), or a menu's (the folder
    // menu, GitHub #22): arrows, Home, End, Page Up and Down, Enter, Esc and
    // what types a character, without Ctrl or Alt.
    enum class Policy { Clipboard, Menu };

    // Keys are taken while `target` (top level) is in front and `window` is
    // shown. The callbacks run on the router's thread: post to the GUI thread.
    // `numberKeys`: the modifier that pastes a row with a digit (Alt, Ctrl,
    // or none: Settings::clipboardNumberKeys).
    KeyRouter(HWND target, HWND window, Callbacks callbacks, Qt::KeyboardModifier numberKeys = Qt::AltModifier,
        Policy policy = Policy::Clipboard);
    ~KeyRouter();

    KeyRouter(const KeyRouter&) = delete;
    KeyRouter& operator=(const KeyRouter&) = delete;

    bool isActive() const noexcept { return m_active.load(); }
    // Approximate session totals, readable from the GUI thread. No keys,
    // text, window titles or identifiers; never writes logs from a hook.
    QJsonObject diagnosticStatus() const;
    // A click in the search field can recover a hook Windows silently
    // removed after a timeout, without activating the clipboard window.
    void refresh();
    void drain(); // no more keys taken, no more callbacks but `drained`

private:
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);
    static void CALLBACK foregroundProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child,
                                        DWORD thread, DWORD time);
    LRESULT handleKey(int code, WPARAM message, LPARAM lParam); // the hook's answer: nonzero, taken
    void handleModifier(bool press, const KBDLLHOOKSTRUCT& event);
    void handleMouse(WPARAM message, const MSLLHOOKSTRUCT& event);
    enum class Availability { Ready, OtherForeground, Hidden, Hung };
    Availability availability() const;
    bool taking() const { return availability() == Availability::Ready; }
    enum Counter : std::size_t {
        Presses, Routed, OtherForeground, Hidden, Hung, WinHeld, AlreadyHeld,
        NotWanted, GlobalHotkey, OtherHook, Refreshes, RefreshFailed, CounterCount
    };
    void count(Counter counter) { m_counts[counter].fetch_add(1, std::memory_order_relaxed); }
    void checkDrained();
    Key translate(const KBDLLHOOKSTRUCT& event, bool press, bool autoRepeat) const;
    bool heldAsHotkey(const Key& key); // router thread

    const HWND m_target;
    const HWND m_window;
    const Qt::KeyboardModifier m_numberKeys;
    const Policy m_policy;
    const DWORD m_process;
    DWORD m_windowProcess = 0;
    Callbacks m_callbacks;
    std::bitset<256> m_taken; // keys held whose press was taken; only touched on the router thread
    std::unordered_map<UINT, bool> m_hotkeys; // heldAsHotkey's answers by chord; router thread
    UINT m_lonelyShift = 0; // the Shift down with nothing else so far (its key), else 0; router thread
    std::atomic<bool> m_draining {false}; // takes effect at once, from the GUI thread
    std::atomic<DWORD> m_threadId {0};
    std::atomic<bool> m_active {false};
    std::array<std::atomic<quint64>, CounterCount> m_counts {};
    const char* m_setupStep = "ready"; // written before the constructor's ready latch
    DWORD m_setupError = 0;
    std::atomic<DWORD> m_refreshError {0};
    std::atomic<DWORD> m_refreshUnhookError {0};
    std::jthread m_thread;
};

} // namespace ws
