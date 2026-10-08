#pragma once

#include "DoubleTapDetector.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>

namespace ws {

// Global "double-tap Ctrl" detection through Raw Input.
//
// Key presses reach a hidden window on its own thread as WM_INPUT messages,
// after the fact. Unlike a low-level keyboard hook, which every key press in
// the system waits on, this never slows anyone's typing, and Windows cannot
// quietly drop it the way it drops a hook that answers too slowly.
class KeyListener {
public:
    // `onDoubleCtrl` runs on the listener thread; post to the GUI thread from it.
    explicit KeyListener(std::function<void()> onDoubleCtrl);
    ~KeyListener();

    KeyListener(const KeyListener&) = delete;
    KeyListener& operator=(const KeyListener&) = delete;

    bool isActive() const noexcept { return m_active.load(); }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void onInput(HRAWINPUT input);

    std::function<void()> m_callback;
    DoubleTapDetector m_detector; // only touched on the listener thread
    std::atomic<HWND> m_window {nullptr};
    std::atomic<bool> m_active {false};
    std::jthread m_thread;
};

} // namespace ws
