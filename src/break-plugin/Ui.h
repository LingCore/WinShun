// The windows of the winshun.break plugin, on a thread of their own with a
// message loop: the notice before a break (a pill at the top of the screen
// the user works on, with 推迟 / 跳过) and the break screen (every monitor
// dimmed, a countdown in the middle). Neither takes the focus. While the
// break screen is up a low-level keyboard hook keeps keys from the program
// underneath; Esc skips (strict: held for 3 seconds).
#pragma once

#include "Core.h"

#include <thread>

namespace brk {

class Ui {
public:
    explicit Ui(Core& core) : m_core(core) {}
    ~Ui() { stop(); }
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    void start();
    void stop();
    // Something changed outside (Enter in the launcher, new settings): look
    // again now instead of at the next tick.
    void wake();

private:
    Core& m_core;
    std::thread m_thread;
    void* m_window = nullptr; // the thread's hidden window (HWND), set before start() returns
};

} // namespace brk
