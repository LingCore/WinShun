#include "KeyListener.h"

#include <algorithm>
#include <latch>

namespace ws {

namespace {

constexpr wchar_t kClassName[] = L"WinShun.KeyListener";
constexpr USHORT kGenericDesktop = 0x01; // HID usage page
constexpr USHORT kMouse = 0x02; // HID usage
constexpr USHORT kKeyboard = 0x06;
constexpr USHORT kAnyButtonDown = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN
    | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
// Ctrl+wheel zooms. A touchpad pinch is sent as Ctrl+wheel too, by Windows.
constexpr USHORT kWheel = RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL;
constexpr UINT_PTR kMouseOffTimer = 1;

// Ctrl pressed while dragging switches between moving and copying.
bool mouseButtonHeld()
{
    for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        if (::GetAsyncKeyState(button) < 0)
            return true;
    }
    return false;
}

// As long as a double click may take (Control Panel → Mouse), so a slow
// setting there is honoured; within limits, so two separate Ctrl presses do
// not make one.
std::uint32_t maxGapMs()
{
    return std::clamp<std::uint32_t>(::GetDoubleClickTime(), 400, 900);
}

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
        // Every keyboard, also while other programs have the focus; the mouse
        // only around a Ctrl press (listenToMouse).
        const RAWINPUTDEVICE keyboard {kGenericDesktop, kKeyboard, RIDEV_INPUTSINK, hwnd};
        m_active = hwnd && ::RegisterRawInputDevices(&keyboard, 1, sizeof keyboard);
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
    case WM_TIMER:
        if (wParam == kMouseOffTimer && self) {
            ::KillTimer(hwnd, kMouseOffTimer);
            if (::GetAsyncKeyState(VK_CONTROL) < 0) // still down: wait for its release
                ::SetTimer(hwnd, kMouseOffTimer, maxGapMs(), nullptr);
            else
                self->listenToMouse(false);
        }
        return 0;
    case WM_CLOSE: {
        const RAWINPUTDEVICE keyboard {kGenericDesktop, kKeyboard, RIDEV_REMOVE, nullptr};
        ::RegisterRawInputDevices(&keyboard, 1, sizeof keyboard);
        if (self) {
            self->listenToMouse(false);
            self->m_active = false;
        }
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
    if (::GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;
    if (raw.header.dwType == RIM_TYPEMOUSE) { // mostly moves, which tell nothing
        if (raw.data.mouse.usButtonFlags & (kAnyButtonDown | kWheel))
            m_detector.mouseUsed();
        return;
    }
    if (raw.header.dwType != RIM_TYPEKEYBOARD)
        return;
    const RAWKEYBOARD& key = raw.data.keyboard;
    if (key.VKey == 0xFF)
        return; // the extra part of an escaped key sequence, not a key
    const bool up = key.Flags & RI_KEY_BREAK;
    const bool ctrl = DoubleTapDetector::isCtrl(key.VKey);
    POINT pt {};
    if (ctrl) {
        ::GetCursorPos(&pt);
        if (!up) {
            m_detector.setMaxGap(maxGapMs());
            listenToMouse(true);
        }
        // Until a second tap can no longer follow. Armed on the press too: a
        // release can be lost (Ctrl+Alt+Del to lock the screen), and the timer
        // then finds Ctrl up.
        ::SetTimer(m_window.load(), kMouseOffTimer, maxGapMs() + 100, nullptr);
    }
    const auto time = static_cast<std::uint32_t>(::GetMessageTime()); // when it was pressed, not when we got to it
    const bool fire = up ? m_detector.keyUp(key.VKey, time, pt.x, pt.y) : m_detector.keyDown(key.VKey, time, pt.x, pt.y);
    if (ctrl && !up && mouseButtonHeld())
        m_detector.mouseUsed();
    if (fire && m_callback)
        m_callback();
}

// The mouse only while it can spoil a tap: from a Ctrl press until a second
// tap can no longer follow. Listened to all the time, every move woke this
// thread, up to thousands of times a second with a gaming mouse.
void KeyListener::listenToMouse(bool on)
{
    if (on == m_mouseOn)
        return;
    const RAWINPUTDEVICE mouse {kGenericDesktop, kMouse, static_cast<DWORD>(on ? RIDEV_INPUTSINK : RIDEV_REMOVE),
        on ? m_window.load() : nullptr};
    if (::RegisterRawInputDevices(&mouse, 1, sizeof mouse) || !on)
        m_mouseOn = on;
}

} // namespace ws
