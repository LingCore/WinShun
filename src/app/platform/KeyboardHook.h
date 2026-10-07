#pragma once

#include "DoubleTapDetector.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>

namespace qf {

// Global "double-tap Ctrl" detection with a low-level keyboard hook.
//
// The hook lives on its own thread with its own message loop. Windows calls
// a low-level hook synchronously for every key press system-wide, so running
// it on the GUI thread would make typing everywhere stall whenever the UI is
// busy. Here the callback does a few comparisons and returns immediately.
class KeyboardHook {
public:
    // `onDoubleCtrl` runs on the hook thread; post to the GUI thread from it.
    explicit KeyboardHook(std::function<void()> onDoubleCtrl);
    ~KeyboardHook();

    KeyboardHook(const KeyboardHook&) = delete;
    KeyboardHook& operator=(const KeyboardHook&) = delete;

    bool isActive() const noexcept { return m_active.load(); }

private:
    void run(std::stop_token stop);
    static LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam);

    std::function<void()> m_callback;
    DoubleTapDetector m_detector; // only touched on the hook thread
    std::atomic<DWORD> m_threadId {0};
    std::atomic<bool> m_active {false};
    std::jthread m_thread;

    static inline std::atomic<KeyboardHook*> s_instance {nullptr};
};

} // namespace qf
