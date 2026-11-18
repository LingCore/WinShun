// The working part of the winshun.gestures plugin: a low-level mouse hook
// on a thread of its own that takes the right button over (Gesture.h reads
// the drag), and the window thread that draws the trail and does what the
// gesture says.
//
// A press of the right button is held back. Let go without moving: the
// press and the release are sent again, so the program gets its plain right
// click. Moved a few pixels: a gesture, drawn while it goes on, done when
// the button is let go. Held still a moment: the press is sent after all
// and the rest goes through, so a drag with the right button still works
// (hold, then drag). Never taken: a window of an administrator's program
// (Windows would drop the click sent again, UIPI; while one is in front
// the hook sees nothing anyway), a program in the exclusions, a full-screen
// window unless asked, a press with Ctrl, Shift, Alt or Win held, touch
// and pen, and input other programs send.
#pragma once

#include "Gesture.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gestures {

// Win顺's colours (the appearance's nativeLook), 0xAARRGGBB.
struct Look {
    std::uint32_t accent = 0xff0078d4; // trail and the hint's fill
    std::uint32_t onAccent = 0xffffffff; // the hint's words
    std::uint32_t muted = 0xf0404040; // the hint of a gesture with no action
    bool chinese = true;
};

struct EngineOptions {
    std::function<void(const std::string&)> log;
    // From the window thread.
    std::function<void()> showSearch;
    std::function<void(const std::wstring& target)> open;
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

    // The hook thread's side (static: one hook per process).
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    struct Shared {
        Settings settings;
        Look look;
    };
    std::shared_ptr<const Shared> shared() const;

    void hookThread(std::promise<bool>* ready);
    void windowThread(std::promise<bool>* ready);
    bool onMouse(WPARAM message, const MSLLHOOKSTRUCT& info);
    void onHoldTimer();
    bool eligible(POINT pt, HWND& under);

    LRESULT onWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void render();
    void hideTrail();
    void perform(const std::string& directions, POINT end);
    bool bringToFront(HWND root);
    void sendKeys(const std::vector<std::uint16_t>& keys);
    void sendButton(DWORD down, DWORD up, DWORD data);

    EngineOptions m_options;
    mutable std::mutex m_mutex;
    std::shared_ptr<const Shared> m_shared;
    std::atomic_bool m_paused {false};
    std::atomic_int m_performed {0};

    std::thread m_hookThread;
    std::thread m_windowThread;
    DWORD m_hookThreadId = 0;
    HWND m_window = nullptr; // the trail; messages from the hook thread

    // Hook thread only.
    enum class State { Idle, Pressed, Gesturing, Through, Canceled };
    State m_state = State::Idle;
    POINT m_down {};
    HWND m_under = nullptr;
    int m_startDistance = 6;
    UINT_PTR m_holdTimer = 0;
    Recognizer m_recognizer;
    std::string m_sentDirections;

    // Window thread only.
    std::vector<POINT> m_points;
    std::string m_directions;
    HWND m_target = nullptr; // the window under the press
    double m_scale = 1.0;
    bool m_dirty = false;
    bool m_shown = false;
    bool m_timer = false;
    HDC m_memory = nullptr;
    HBITMAP m_bitmap = nullptr;
    void* m_bits = nullptr;
    SIZE m_capacity {};
    ULONG_PTR m_gdiplus = 0;
};

} // namespace gestures
