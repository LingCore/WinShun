#include "NumberKeys.h"

#include "KeyListener.h" // KeyboardRawInputAside, hookedKey
#include "OwnInput.h"

#include <QCoreApplication>
#include <QtLogging>

#include <array>
#include <latch>
#include <optional>
#include <string>

namespace ws::numberkeys {

namespace {

thread_local Hook* t_hook = nullptr; // a low-level hook is called on the thread that set it

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

Qt::KeyboardModifiers heldModifiers()
{
    Qt::KeyboardModifiers modifiers;
    if (held(VK_SHIFT))
        modifiers |= Qt::ShiftModifier;
    if (held(VK_CONTROL))
        modifiers |= Qt::ControlModifier;
    if (held(VK_MENU))
        modifiers |= Qt::AltModifier;
    if (held(VK_LWIN) || held(VK_RWIN))
        modifiers |= Qt::MetaModifier;
    return modifiers;
}

// In place of a digit taken: the modifier held was not pressed alone.
void sendMask()
{
    std::array<INPUT, 2> keys {};
    for (INPUT& key : keys) {
        key.type = INPUT_KEYBOARD;
        key.ki.wVk = kMaskKey;
        key.ki.dwExtraInfo = kOwnInput;
    }
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
}

// KeyboardRawInputAside's lines, on the GUI thread (see ShortcutCapture's).
void logRawInput(bool warning, const std::string& line)
{
    static const bool info = qEnvironmentVariableIsSet("WINSHUN_LOG_INFO");
    if (!warning && !info)
        return;
    auto write = [warning, line] {
        if (warning)
            qWarning("Number keys: %s", line.c_str());
        else
            qInfo("Number keys: %s", line.c_str());
    };
    if (QCoreApplication* app = QCoreApplication::instance())
        QMetaObject::invokeMethod(app, std::move(write), Qt::QueuedConnection);
    else
        write();
}

} // namespace

std::vector<int> heldAsHotkeys(Modifier modifier)
{
    constexpr int kProbeId = 0xBF00; // a thread's own hotkey ids go up to 0xBFFF
    const UINT modifiers = (modifier == Modifier::Alt ? MOD_ALT : MOD_CONTROL) | MOD_NOREPEAT;
    std::vector<int> held;
    for (int n = 1; n <= 9; ++n) {
        if (::RegisterHotKey(nullptr, kProbeId, modifiers, static_cast<UINT>('0' + n)))
            ::UnregisterHotKey(nullptr, kProbeId); // free: let go at once
        else if (::GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED)
            held.push_back(n);
    }
    return held;
}

unsigned heldMask(Modifier modifier)
{
    unsigned mask = 0;
    for (const int n : heldAsHotkeys(modifier))
        mask |= 1u << n;
    return mask;
}

Hook::Hook(Modifier modifier, unsigned digits, std::function<void(const Key&)> onKey)
    : m_modifier(modifier)
    , m_digits(digits)
    , m_process(::GetCurrentProcessId())
    , m_onKey(std::move(onKey))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_hook = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // Every key press in the system waits for the hook's answer.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        // Set aside before the hook comes and until after it went (see the
        // header); shared with a hold of TypeToSearch's still on.
        std::optional<KeyboardRawInputAside> rawInputAside(std::in_place, &logRawInput);
        const HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &Hook::keyboardProc, ::GetModuleHandleW(nullptr), 0);
        m_active = hook != nullptr;
        if (!hook)
            qWarning("Number keys: no keyboard hook (error %lu)", ::GetLastError());
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

Hook::~Hook()
{
    ::PostThreadMessageW(m_threadId.load(), WM_QUIT, 0, 0); // ends the message loop
    if (m_thread.joinable())
        m_thread.join();
}

LRESULT CALLBACK Hook::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && t_hook)
        return t_hook->handle(code, wParam, lParam);
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT Hook::handle(int code, WPARAM message, LPARAM lParam)
{
    const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    // What goes on is what KeyListener's Raw Input would have heard.
    const auto next = [&] {
        const LRESULT taken = ::CallNextHookEx(nullptr, code, message, lParam);
        if (taken == 0)
            KeyListener::hookedKey(event);
        return taken;
    };
    const UINT vk = event.vkCode;
    if (vk >= m_taken.size() || isOwnInput(event.dwExtraInfo))
        return next();
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (m_taken.test(vk)) {
        // Its release and repeats are taken too, wherever the focus went; the
        // hooks after this one saw its press, and see these as well.
        ::CallNextHookEx(nullptr, code, message, lParam);
        m_taken.set(vk, press);
        m_onKey({static_cast<int>(vk - '0'), heldModifiers() & ~Qt::MetaModifier, press, press, vk, event.scanCode});
        return 1;
    }
    if (!press || held(static_cast<int>(vk))) // down since before: its repeats go where its press went
        return next();
    const Qt::KeyboardModifiers modifiers = heldModifiers();
    const int digit = takenDigit(vk, modifiers, m_modifier, m_digits);
    if (!digit)
        return next();
    DWORD process = 0;
    ::GetWindowThreadProcessId(::GetForegroundWindow(), &process);
    if (process != m_process)
        return next();
    if (const LRESULT taken = ::CallNextHookEx(nullptr, code, message, lParam); taken != 0)
        return taken; // a remapper's
    m_taken.set(vk);
    sendMask();
    m_onKey({digit, modifiers, true, false, vk, event.scanCode});
    return 1; // not the other program's hotkey
}

} // namespace ws::numberkeys
