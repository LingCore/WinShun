#include "NumberKeys.h"

#include "KeyRouter.h" // kOwnInput

#include <algorithm>
#include <array>
#include <latch>

namespace ws {

namespace {

thread_local NumberKeys* t_keys = nullptr; // a low-level hook is called on the thread that set it

constexpr WORD kMaskKey = 0xE8; // unassigned: no program acts on it (KeyRouter's too)

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Between the modifier's press and release, in place of the key taken.
void sendMask()
{
    std::array<INPUT, 2> keys {};
    for (INPUT& input : keys) {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = kMaskKey;
        input.ki.dwExtraInfo = kOwnInput;
    }
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
}

} // namespace

NumberKeys::NumberKeys(std::vector<HWND> windows, Modifier modifier, std::function<void(HWND, int)> onNumber)
    : m_windows(std::move(windows))
    , m_modifier(modifier)
    , m_onNumber(std::move(onNumber))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_keys = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // Every key press in the system waits for the hook's answer.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &NumberKeys::hookProc, ::GetModuleHandleW(nullptr), 0);
        m_active = hook != nullptr;
        ready.count_down();
        if (!hook)
            return;
        // The hook is called from inside GetMessage; there is nothing to dispatch.
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        }
        ::UnhookWindowsHookEx(hook);
    });
    ready.wait();
}

NumberKeys::~NumberKeys()
{
    ::PostThreadMessageW(m_threadId.load(), WM_QUIT, 0, 0); // ends the message loop
    if (m_thread.joinable())
        m_thread.join();
}

LRESULT CALLBACK NumberKeys::hookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && t_keys && t_keys->handle(wParam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)))
        return 1; // seen by nobody else: not other programs' hotkeys or hooks, not the window
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

std::vector<int> NumberKeys::heldAsHotkeys(Modifier modifier)
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

int NumberKeys::numberOf(UINT vk) const noexcept
{
    if (vk >= '1' && vk <= '9')
        return static_cast<int>(vk - '0');
    if (m_modifier == Modifier::Ctrl && vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)
        return static_cast<int>(vk - VK_NUMPAD0);
    return 0;
}

bool NumberKeys::handle(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    const UINT vk = event.vkCode;
    if (vk >= m_taken.size() || event.dwExtraInfo == kOwnInput)
        return false;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (!press) {
        if (!m_taken.test(vk))
            return false;
        m_taken.reset(vk); // its release is ours too: the window never saw it go down
        return true;
    }
    if (m_taken.test(vk))
        return true; // held down: its auto-repeat
    const int number = numberOf(vk);
    if (number == 0 || m_composing.load())
        return false;
    const bool ctrl = held(VK_CONTROL);
    const bool alt = held(VK_MENU);
    const bool wanted = m_modifier == Modifier::Ctrl ? ctrl && !alt : alt && !ctrl;
    if (!wanted || held(VK_SHIFT) || held(VK_LWIN) || held(VK_RWIN) || held(static_cast<int>(vk)))
        return false; // (the last: down since before the hook came, so its release is not ours)
    const HWND front = ::GetForegroundWindow();
    if (std::find(m_windows.begin(), m_windows.end(), front) == m_windows.end())
        return false;
    m_taken.set(vk);
    sendMask();
    m_onNumber(front, number);
    return true;
}

} // namespace ws
