#include "KeyListener.h"

#include <latch>

namespace qf {

namespace {

constexpr wchar_t kClassName[] = L"QuickFind.KeyListener";
constexpr USHORT kGenericDesktop = 0x01; // HID usage page
constexpr USHORT kKeyboard = 0x06; // HID usage

} // namespace

KeyListener::KeyListener(std::function<void()> onDoubleCtrl)
    : m_callback(std::move(onDoubleCtrl))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = &KeyListener::wndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        ::RegisterClassExW(&wc);
        // Message-only: never shown, and still the target of raw input.
        const HWND hwnd = ::CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        // Every keyboard, also while other programs have the focus.
        RAWINPUTDEVICE device {kGenericDesktop, kKeyboard, RIDEV_INPUTSINK, hwnd};
        m_active = hwnd && ::RegisterRawInputDevices(&device, 1, sizeof device);
        m_window = hwnd;
        ready.count_down();
        if (!hwnd)
            return;
        // Above normal: the second tap is timed from the message's own clock,
        // but it should not wait behind other work for long either.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
    });
    ready.wait();
}

KeyListener::~KeyListener()
{
    if (const HWND hwnd = m_window.load())
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0); // the window ends the thread's message loop
    if (m_thread.joinable())
        m_thread.join();
}

LRESULT CALLBACK KeyListener::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<KeyListener*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_INPUT:
        if (self)
            self->onInput(reinterpret_cast<HRAWINPUT>(lParam));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam); // lets Windows free the input
    case WM_CLOSE: {
        RAWINPUTDEVICE device {kGenericDesktop, kKeyboard, RIDEV_REMOVE, nullptr};
        ::RegisterRawInputDevices(&device, 1, sizeof device);
        if (self)
            self->m_active = false;
        ::DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void KeyListener::onInput(HRAWINPUT input)
{
    RAWINPUT raw {};
    UINT size = sizeof raw;
    if (::GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)
        || raw.header.dwType != RIM_TYPEKEYBOARD)
        return;
    const RAWKEYBOARD& key = raw.data.keyboard;
    if (key.VKey == 0xFF)
        return; // the extra part of an escaped key sequence, not a key
    const bool up = key.Flags & RI_KEY_BREAK;
    POINT pt {};
    if (DoubleTapDetector::isCtrl(key.VKey))
        ::GetCursorPos(&pt);
    const auto time = static_cast<std::uint32_t>(::GetMessageTime()); // when it was pressed, not when we got to it
    const bool fire = up ? m_detector.keyUp(key.VKey, time, pt.x, pt.y) : m_detector.keyDown(key.VKey, time, pt.x, pt.y);
    if (fire && m_callback)
        m_callback();
}

} // namespace qf
