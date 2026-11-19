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
// a press with Ctrl, Shift, Alt or Win held, touch and pen, and input other
// programs send.
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
    std::wstring text; // Ok: the translation; Failed: why
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
    std::function<std::wstring(HWND root)> selectedText;
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
    void setPaused(bool paused) { m_paused = paused; }
    bool paused() const { return m_paused; }
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
    void onHoldTimer();
    void onDwellTimer();
    void watchScreen(POINT pt);
    bool eligible(POINT pt, HWND& under, std::wstring& program);
    bool frontEligible(std::wstring* program);
    void endPress();

    // The window thread.
    void windowThread(std::promise<bool>* ready);
    LRESULT onWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void began(POINT down, HWND under);
    void render();
    void hideTrail();
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
    void perform(const Rule& rule, const Target& target);
    void performBuiltin(Builtin builtin, const Target& target);
    void textAction(Builtin builtin, const Target& target);
    void askFirst(Builtin builtin, const Target& target);
    void triggered(std::string_view on, POINT at);
    void wheel(bool up, POINT at, HWND under);
    void performWithWords(const Rule& rule, const Target& target);
    void post(std::function<void()> work);
    bool bringToFront(HWND root);
    void keysTo(HWND root, const std::vector<std::uint16_t>& keys);
    void sendKeys(const std::vector<std::uint16_t>& keys);
    void typeText(const std::wstring& words);
    void sendButton(DWORD down, DWORD up, DWORD data);
    void note(POINT at, NoteIcon icon, const std::wstring& text, const std::wstring& lead = {});

    EngineOptions m_options;
    mutable std::mutex m_mutex;
    std::shared_ptr<const Shared> m_shared;
    std::atomic_bool m_paused {false};
    std::atomic_int m_performed {0};
    std::atomic<DWORD> m_ownProcess {0};
    std::atomic_bool m_bubbleShown {false}; // the hook tells the window thread of presses then

    std::thread m_hookThread;
    std::thread m_windowThread;
    DWORD m_hookThreadId = 0;
    HWND m_window = nullptr; // the trail; messages from the hook thread

    // Hook thread only.
    enum class State { Idle, Pressed, Gesturing, Wheeling, Through, Canceled };
    State m_state = State::Idle;
    POINT m_down {};
    HWND m_under = nullptr;
    std::wstring m_program; // of the window pressed on, a file name in lower case
    int m_startDistance = 6;
    UINT_PTR m_holdTimer = 0;
    UINT_PTR m_dwellTimer = 0;
    int m_wheelDelta = 0; // what is left of a turn smaller than a notch
    Recognizer m_recognizer;
    CornerWatch m_cornerWatch;
    RubWatch m_rubWatch;
    HMONITOR m_monitor = nullptr; // the screen the pointer was last on
    Rect m_monitorRect;
    double m_monitorScale = 1.0;

    // Window thread only.
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
