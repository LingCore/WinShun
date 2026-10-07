#include "KeyboardHook.h"

#include <latch>

namespace qf {

KeyboardHook::KeyboardHook(std::function<void()> onDoubleCtrl)
    : m_callback(std::move(onDoubleCtrl))
{
    s_instance.store(this);
    std::latch ready(1);
    m_thread = std::jthread([this, &ready](std::stop_token stop) {
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create the message queue
        m_threadId = ::GetCurrentThreadId();
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &KeyboardHook::hookProc, ::GetModuleHandleW(nullptr), 0);
        m_active = hook != nullptr;
        ready.count_down();
        run(stop);
        if (hook)
            ::UnhookWindowsHookEx(hook);
        m_active = false;
    });
    ready.wait();
}

KeyboardHook::~KeyboardHook()
{
    m_thread.request_stop();
    if (const DWORD id = m_threadId.load())
        ::PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (m_thread.joinable())
        m_thread.join();
    s_instance.store(nullptr);
}

void KeyboardHook::run(std::stop_token stop)
{
    MSG msg;
    while (!stop.stop_requested() && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK KeyboardHook::hookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        if (KeyboardHook* self = s_instance.load()) {
            const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
            if (down || up) {
                POINT pt {};
                if (DoubleTapDetector::isCtrl(key->vkCode))
                    ::GetCursorPos(&pt);
                const bool fire = down ? self->m_detector.keyDown(key->vkCode, key->time, pt.x, pt.y)
                                       : self->m_detector.keyUp(key->vkCode, key->time, pt.x, pt.y);
                if (fire && self->m_callback)
                    self->m_callback();
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace qf
