#include "ShortcutCapture.h"

#include "KeyListener.h" // KeyboardRawInputAside

#include <QCoreApplication>
#include <QtLogging>

#include <latch>
#include <optional>
#include <utility>

namespace ws {

namespace {

thread_local ShortcutCapture* t_capture = nullptr; // a low-level hook is called on the thread that set it

// KeyboardRawInputAside's lines: the set-aside itself with WINSHUN_LOG_INFO
// (main.cpp's logToFile drops info lines without it), what Windows refused
// always. Written on the GUI thread, as TypeToSearch's are (that one's come
// from inside its hook): the capture's thread only sets the hook up and takes
// it down around them, and the settings window waits for it. With no
// application (a test), here.
void logRawInput(bool warning, const std::string& line)
{
    static const bool info = qEnvironmentVariableIsSet("WINSHUN_LOG_INFO");
    if (!warning && !info)
        return;
    auto write = [warning, line] {
        if (warning)
            qWarning("ShortcutCapture: %s", line.c_str());
        else
            qInfo("ShortcutCapture: %s", line.c_str());
    };
    if (QCoreApplication* app = QCoreApplication::instance())
        QMetaObject::invokeMethod(app, std::move(write), Qt::QueuedConnection);
    else
        write();
}

// The key as the settings window's recorders know it (see HotkeyRecorder.qml),
// as Qt names it: the number pad's with Qt::KeypadModifier (`keypad`), the
// signs by the US keyboard's keys whatever the layout (a favorite's shortcut
// presses them by key, KeyChord.h). Qt::Key_unknown: the recorder says it
// cannot be used.
struct QtKey {
    int key;
    bool keypad = false;
};

QtKey qtKey(UINT vk, Qt::KeyboardModifiers modifiers, bool extended)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return {static_cast<int>(vk)}; // Qt::Key_A… and Qt::Key_0… have the same codes
    if (vk >= VK_F1 && vk <= VK_F24)
        return {Qt::Key_F1 + static_cast<int>(vk - VK_F1)};
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return {Qt::Key_0 + static_cast<int>(vk - VK_NUMPAD0), true};
    switch (vk) {
    case VK_CONTROL: // sent by programs; the keyboard says which one
    case VK_LCONTROL:
    case VK_RCONTROL:
        return {Qt::Key_Control};
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return {Qt::Key_Alt};
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return {Qt::Key_Shift};
    case VK_LWIN:
    case VK_RWIN:
        return {Qt::Key_Meta};
    case VK_SPACE: return {Qt::Key_Space};
    case VK_TAB: return {modifiers.testFlag(Qt::ShiftModifier) ? Qt::Key_Backtab : Qt::Key_Tab}; // as Qt spells Shift+Tab
    case VK_ESCAPE: return {Qt::Key_Escape};
    case VK_BACK: return {Qt::Key_Backspace};
    case VK_DELETE: return {Qt::Key_Delete};
    case VK_RETURN: return extended ? QtKey {Qt::Key_Enter, true} : QtKey {Qt::Key_Return};
    case VK_INSERT: return {Qt::Key_Insert};
    case VK_HOME: return {Qt::Key_Home};
    case VK_END: return {Qt::Key_End};
    case VK_PRIOR: return {Qt::Key_PageUp};
    case VK_NEXT: return {Qt::Key_PageDown};
    case VK_LEFT: return {Qt::Key_Left};
    case VK_RIGHT: return {Qt::Key_Right};
    case VK_UP: return {Qt::Key_Up};
    case VK_DOWN: return {Qt::Key_Down};
    case VK_SNAPSHOT: return {Qt::Key_Print};
    case VK_PAUSE: return {Qt::Key_Pause};
    case VK_CAPITAL: return {Qt::Key_CapsLock};
    case VK_NUMLOCK: return {Qt::Key_NumLock};
    case VK_SCROLL: return {Qt::Key_ScrollLock};
    case VK_APPS: return {Qt::Key_Menu};
    case VK_MULTIPLY: return {Qt::Key_Asterisk, true};
    case VK_ADD: return {Qt::Key_Plus, true};
    case VK_SUBTRACT: return {Qt::Key_Minus, true};
    case VK_DECIMAL: return {Qt::Key_Period, true};
    case VK_DIVIDE: return {Qt::Key_Slash, true};
    case VK_OEM_3: return {Qt::Key_QuoteLeft};
    case VK_OEM_MINUS: return {Qt::Key_Minus};
    case VK_OEM_PLUS: return {Qt::Key_Equal};
    case VK_OEM_4: return {Qt::Key_BracketLeft};
    case VK_OEM_6: return {Qt::Key_BracketRight};
    case VK_OEM_5: return {Qt::Key_Backslash};
    case VK_OEM_1: return {Qt::Key_Semicolon};
    case VK_OEM_7: return {Qt::Key_Apostrophe};
    case VK_OEM_COMMA: return {Qt::Key_Comma};
    case VK_OEM_PERIOD: return {Qt::Key_Period};
    case VK_OEM_2: return {Qt::Key_Slash};
    default: return {Qt::Key_unknown};
    }
}

} // namespace

ShortcutCapture::ShortcutCapture(HWND window, std::function<void(const Key&)> onKey)
    : m_window(window)
    , m_onKey(std::move(onKey))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_capture = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // Every key press in the system waits for the hook's answer.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        // Set aside before the hook comes and until after it went (see the
        // header); shared with a hold of TypeToSearch's still on.
        std::optional<KeyboardRawInputAside> rawInputAside(std::in_place, &logRawInput);
        const HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &ShortcutCapture::hookProc, ::GetModuleHandleW(nullptr), 0);
        m_active = hook != nullptr;
        if (!hook)
            qWarning("ShortcutCapture: no keyboard hook (error %lu)", ::GetLastError());
        ready.count_down();
        if (hook) {
            // The hook is called from inside GetMessage; there is nothing to dispatch.
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            }
            ::UnhookWindowsHookEx(hook);
        }
        rawInputAside.reset();
    });
    ready.wait();
}

ShortcutCapture::~ShortcutCapture()
{
    ::PostThreadMessageW(m_threadId.load(), WM_QUIT, 0, 0); // ends the message loop
    if (m_thread.joinable())
        m_thread.join();
}

LRESULT CALLBACK ShortcutCapture::hookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && t_capture
        && t_capture->handle(wParam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)))
        return 1; // seen by nobody else: not Windows, not the window, not other programs' hotkeys
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

bool ShortcutCapture::handle(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    const UINT vk = event.vkCode;
    if (vk >= m_taken.size())
        return false;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    bool autoRepeat = false;
    if (press) {
        autoRepeat = m_taken.test(vk); // a taken key stays taken, wherever the focus went
        if (!autoRepeat) {
            if (::GetForegroundWindow() != m_window)
                return false;
            if (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2)
                return false; // volume, media and browser keys keep working
            // Windows saw this key go down before the capture began (this is
            // its auto-repeat): it must see all of it, up to the release.
            if (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)
                return false;
        }
    } else if (!m_taken.test(vk)) {
        return false; // likewise
    }
    m_taken.set(vk, !press); // a press is not counted yet, a release still is (see Key::modifiers)
    const Qt::KeyboardModifiers held = modifiers();
    m_taken.set(vk, press);
    const QtKey key = qtKey(vk, held, (event.flags & LLKHF_EXTENDED) != 0);
    m_onKey({key.key, key.keypad ? held | Qt::KeypadModifier : held, press, autoRepeat});
    return true;
}

bool ShortcutCapture::isDown(UINT vk) const
{
    return m_taken.test(vk) || (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000); // or held from before
}

Qt::KeyboardModifiers ShortcutCapture::modifiers() const
{
    Qt::KeyboardModifiers held;
    if (isDown(VK_CONTROL) || isDown(VK_LCONTROL) || isDown(VK_RCONTROL))
        held |= Qt::ControlModifier;
    if (isDown(VK_MENU) || isDown(VK_LMENU) || isDown(VK_RMENU))
        held |= Qt::AltModifier;
    if (isDown(VK_SHIFT) || isDown(VK_LSHIFT) || isDown(VK_RSHIFT))
        held |= Qt::ShiftModifier;
    if (isDown(VK_LWIN) || isDown(VK_RWIN))
        held |= Qt::MetaModifier;
    return held;
}

} // namespace ws
