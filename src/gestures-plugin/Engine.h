// The working part of the winshun.gestures plugin: a low-level mouse hook
// on a thread of its own that takes the right button over (Gesture.h reads
// the drag) and watches the screen's corners and edges, and the window
// thread that draws the trail and does what the rules say.
//
// A press of the right button is held back. Let go without moving: the
// press and the release are sent again, so the program gets its plain right
// click. Moved a few pixels: a gesture, drawn while it goes on, done when
// the button is let go (Esc or the left button first: nothing). The wheel
// turned meanwhile: the wheel's rule, once a notch, and nothing on release.
// Held still a moment: the press is sent after all and the rest goes
// through, so a drag with the right button still works (hold, then drag).
// Never taken: a window of an administrator's program (Windows would drop
// the click sent again, UIPI; while one is in front the hook sees nothing
// anyway), a program in the exclusions, a full-screen window unless asked,
// a press with Win held, one with Ctrl, Alt or Shift held unless a gesture
// is set for those keys there ("shift:DR"; it then acts once they are let
// go), touch and pen, and input other programs send.
//
// The rules' shortcuts ("key:Ctrl+Alt+T") are hotkeys of the window thread
// (RegisterHotKey): pressed, the rule acts on the window in front, at the
// pointer, with the words selected there; let go while paused and while the
// settings window records a key.
#pragma once

#include "Cards.h"
#include "Gesture.h"
#include "Look.h"
#include "Selection.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gestures {

// What a translation came back as (the host's translate_text).
struct Translation {
    enum class State { Ok, NoService, NoPlugin, Failed };
    State state = State::Failed;
    std::wstring text; // Ok: the translation; Failed: why; NoService: what to set, when said
};

// What the engine asks of Win顺. Called on the window thread, except where
// it says so; those that wait for Win顺 are given a thread of their own.
struct EngineOptions {
    std::function<void(const std::string&)> log;
    std::function<void()> showSearch; // Win顺's search box, shown or hidden
    std::function<void(const std::wstring& target)> open;
    std::function<void(const std::wstring& words)> findInWinShun; // the search box with these words
    std::function<void(const std::wstring& words)> webSearch; // Win顺's web search
    // On a thread of its own; `done` may be called on any thread.
    std::function<void(const std::wstring& words, std::function<void(Translation)> done)> translate;
    std::function<void()> translateSetup; // the translate plugin's card, or the library's entry
    std::function<void(const std::wstring& words)> copy;
    // The words selected in `root` when it is the window in front, asked of
    // Win顺 (SelectionReader); on the reader's thread, waiting for the answer.
    // `copy`: Win顺 may press Ctrl+C there and put the clipboard back (a
    // shortcut pressed, never a gesture).
    std::function<Selected(HWND root, bool copy)> selectedText;
    // The shortcuts of the rules were registered (on the window thread):
    // those another program holds, as the rules write them ("key:Ctrl+Alt+T").
    std::function<void(const std::vector<std::string>& taken)> shortcutsRegistered;
    // "shutdown", "restart", "sleep": done by Win顺 (this plugin has no right
    // to). On a thread of its own; the error's words, empty when done.
    std::function<std::wstring(const std::string& what)> power;
};

class Engine {
public:
    explicit Engine(EngineOptions options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start();
    void stop();
    void configure(Settings settings, Look look);
    // Paused: the right button is plain again and the shortcuts are let go.
    void setPaused(bool paused);
    bool paused() const { return m_paused; }
    // The settings window records a key: the shortcuts are let go meanwhile,
    // so the keys pressed reach it.
    void setRecording(bool recording);
    int performed() const { return m_performed; }
    // A process whose windows keep their right button: the settings
    // window, where gestures are drawn to be set.
    void setOwnWindowsOf(DWORD pid) { m_ownProcess = pid; }

    // The hook thread's side (static: one hook per process).
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    struct Shared {
        Settings settings;
        Look look;
        bool corners = false; // any rule for a corner, an edge: watched then
        bool edges = false;
        bool wheel = false;
    };
    std::shared_ptr<const Shared> shared() const;

    // Where a gesture, a turn of the wheel, a corner or an edge happens.
    struct Target {
        HWND under = nullptr; // the window under the pointer (a gesture's, where it began)
        HWND root = nullptr; // its top-level window
        POINT at {}; // where it ended
        Context context;
        std::wstring words; // the words selected there, for an action on them
    };

    // The hook thread.
    void hookThread(std::promise<bool>* ready);
    bool onMouse(WPARAM message, const MSLLHOOKSTRUCT& info);
    void applySessionResult(const SessionResult& res, POINT at);
    void onHoldTimer();
    void onDwellTimer();
    void watchScreen(POINT pt);
    bool eligible(POINT pt, MouseButton button, HWND& under, std::wstring& program, Modifiers& modifiers);
    bool frontEligible(std::wstring* program);
    void endPress();

    // The window thread.
    void windowThread(std::promise<bool>* ready);
    LRESULT onWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void began(POINT down, HWND under, MouseButton button, Modifiers modifiers);
    void render();
    void hideTrail();
    void unregisterEscape();
    // What the hint by the trail says, in its parts: the arrows on a chip,
    // then the corner the rule is for, what letting go does and the words
    // it does it on.
    struct Hint {
        std::wstring arrows;
        std::wstring zone;
        std::wstring action;
        std::wstring words;
        bool known = false; // an action that does something
    };
    Hint hintFor(const std::string& gesture) const;
    void finish(const std::string& gesture, POINT end);
    void heldKeysLetGo();
    void perform(const Rule& rule, const Target& target);
    void performBuiltin(Builtin builtin, const Target& target);
    void textAction(Builtin builtin, const Target& target);
    void askFirst(Builtin builtin, const Target& target);
    void triggered(std::string_view on, POINT at);
    void wheel(bool up, POINT at, HWND root, MouseButton button);
    void registerShortcuts();
    void shortcutLetGo();
    void shortcutPressed(const std::string& on);
    void performWithWords(const Rule& rule, const Target& target);
    void post(std::function<void()> work);
    bool bringToFront(HWND root);
    void keysTo(HWND root, const std::vector<std::uint16_t>& keys);
    void sendKeys(const std::vector<std::uint16_t>& keys);
    void typeText(const std::wstring& words);
    void sendButton(DWORD down, DWORD up, DWORD data);
    void replayButton(MouseButton button, bool click);
    void note(POINT at, NoteIcon icon, const std::wstring& text, const std::wstring& lead = {});

    EngineOptions m_options;
    mutable std::mutex m_mutex;
    std::shared_ptr<const Shared> m_shared;
    std::atomic_bool m_paused {false};
    std::atomic_bool m_recording {false};
    std::atomic_int m_performed {0};
    std::atomic<DWORD> m_ownProcess {0};
    std::atomic_bool m_bubbleShown {false}; // the hook tells the window thread of presses then

    std::thread m_hookThread;
    std::thread m_windowThread;
    DWORD m_hookThreadId = 0;
    HWND m_window = nullptr; // the trail; messages from the hook thread

    // Hook thread only.
    GestureSession m_session;
    POINT m_down {};
    HWND m_under = nullptr;
    // Its top-level window, as it was then: a browser moves the window of a
    // tab's page to a hidden window of its own when the tab changes, so
    // m_under's top-level window can be another one by the next notch.
    HWND m_pressedRoot = nullptr;
    std::wstring m_program; // of the window pressed on, a file name in lower case
    Modifiers m_modifiers = 0; // Ctrl, Alt, Shift held when it was pressed
    Zone m_downZone = Zone::Anywhere;
    UINT_PTR m_holdTimer = 0;
    UINT_PTR m_dwellTimer = 0;
    CornerWatch m_cornerWatch;
    RubWatch m_rubWatch;
    HMONITOR m_monitor = nullptr; // the screen the pointer was last on
    Rect m_monitorRect;
    double m_monitorScale = 1.0;

    // Window thread only.
    std::vector<std::string> m_shortcuts; // registered: hotkey id - 1 -> its trigger ("key:Ctrl+Alt+T")
    std::vector<std::string> m_shortcutsTaken; // told last (shortcutsRegistered)
    std::string m_shortcutHeld; // pressed, its Ctrl, Alt, Shift or Win not let go yet (shortcutLetGo)
    ULONGLONG m_shortcutAt = 0; // when (GetTickCount64)
    // Gestures done while keys were still held, in order, waiting for them to be let go (heldKeysLetGo).
    std::vector<std::pair<Rule, Target>> m_heldKeysWaiting;
    ULONGLONG m_heldKeysSince = 0; // when the first began to wait (GetTickCount64)
    std::vector<POINT> m_points;
    std::string m_gesture; // what it reads as so far: directions or a circle
    Target m_target; // the gesture's, from its start
    bool m_drawing = false;
    // The words selected where the gesture began: asked once it begins,
    // answered (m_selectionKnown) on the reader's thread a moment later.
    SelectionReader m_reader;
    int m_ask = 0; // which gesture the answer is for
    bool m_selectionAsked = false;
    bool m_selectionKnown = false;
    std::wstring m_selection;
    std::string m_pendingGesture; // let go before the answer came: done once it does
    POINT m_pendingEnd {};
    bool m_pending = false;
    bool m_escape = false; // Esc registered while drawing
    bool m_escapeWithKeys = false; // and Esc with the keys it is drawn with
    double m_scale = 1.0;
    bool m_dirty = false;
    bool m_shown = false;
    bool m_timer = false;
    HDC m_memory = nullptr;
    HBITMAP m_bitmap = nullptr;
    void* m_bits = nullptr;
    SIZE m_capacity {};
    ULONG_PTR m_gdiplus = 0;
    Cards m_cards;
};

} // namespace gestures
