#include "StartMenuTyping.h"

#include "KeyRouter.h" // kOwnInput

#include <cwchar>
#include <latch>

namespace ws {

namespace {

// Low-level hooks and out-of-context events are called on the thread that set them.
thread_local StartMenuTyping* t_typing = nullptr;

constexpr UINT kDeliverMessage = WM_APP + 1; // to the hook's thread (deliver)
constexpr UINT kHoldTimeoutMs = 3000; // the launcher comes in well under a second; keys held longer are dropped

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool isModifier(UINT vk)
{
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_CAPITAL:
    case VK_NUMLOCK:
        return true;
    default:
        return false;
    }
}

// Keys that start a search: letters, digits, punctuation, the number pad's
// digits and signs. Not Space or Backspace, which do nothing in an empty
// search box. Once a search has started, every key is held back.
bool startsTyping(UINT vk)
{
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)
        || (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102;
}

// The Start menu, or Windows' search, which takes it over when typed in.
// Windows 11 has them in SearchHost.exe (the Start menu in front, on 25H2)
// and StartMenuExperienceHost.exe, Windows 10 in SearchApp.exe or SearchUI.exe.
bool isStartMenu(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid) || pid == 0)
        return false;
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    wchar_t path[MAX_PATH] {};
    DWORD size = MAX_PATH;
    const bool named = ::QueryFullProcessImageNameW(process, 0, path, &size);
    ::CloseHandle(process);
    if (!named)
        return false;
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    const wchar_t* file = slash ? slash + 1 : path;
    for (const wchar_t* host : {L"SearchHost.exe", L"StartMenuExperienceHost.exe", L"SearchApp.exe", L"SearchUI.exe"}) {
        if (_wcsicmp(file, host) == 0)
            return true;
    }
    return false;
}

INPUT keyInput(WORD vk, WORD scanCode, DWORD flags)
{
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = flags;
    input.ki.dwExtraInfo = kOwnInput;
    return input;
}

} // namespace

StartMenuTyping::StartMenuTyping(Callbacks callbacks)
    : m_callbacks(std::move(callbacks))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_typing = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // While the hook is there, every key press in the system waits for
        // it: at once, and not slowed down when the system saves power
        // (Windows drops a hook that keeps it waiting too long).
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        THREAD_POWER_THROTTLING_STATE throttling {THREAD_POWER_THROTTLING_CURRENT_VERSION,
            THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0};
        ::SetThreadInformation(::GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof throttling);
        const HWINEVENTHOOK foreground = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            &StartMenuTyping::foregroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        ready.count_down();
        foregroundChanged(::GetForegroundWindow()); // open already
        // The hook and the events are called from inside GetMessage.
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == nullptr && msg.message == kDeliverMessage)
                typeHeld();
            else if (msg.hwnd == nullptr && msg.message == WM_TIMER && msg.wParam == m_timer)
                dropHeld();
            else
                ::DispatchMessageW(&msg);
        }
        setHooked(false);
        if (foreground)
            ::UnhookWinEvent(foreground);
    });
    ready.wait();
}

StartMenuTyping::~StartMenuTyping()
{
    ::PostThreadMessageW(m_threadId.load(), WM_QUIT, 0, 0); // ends the message loop
    if (m_thread.joinable())
        m_thread.join();
}

void StartMenuTyping::deliver()
{
    ::PostThreadMessageW(m_threadId.load(), kDeliverMessage, 0, 0);
}

LRESULT CALLBACK StartMenuTyping::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && t_typing && t_typing->handleKey(wParam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)))
        return 1; // seen by nobody else
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

void CALLBACK StartMenuTyping::foregroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
    // The window really in front now, not the one the event names: that
    // can be a part of it, or already gone.
    if (t_typing)
        t_typing->foregroundChanged(::GetForegroundWindow());
}

void StartMenuTyping::foregroundChanged(HWND window)
{
    m_startInFront = isStartMenu(window);
    if (!m_startInFront)
        m_windowsSearch = false; // Left Alt counts for one opening
    setHooked(m_startInFront || m_holding);
}

void StartMenuTyping::setHooked(bool hooked)
{
    if (hooked == (m_hook != nullptr))
        return;
    if (hooked) {
        m_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &StartMenuTyping::keyboardProc, ::GetModuleHandleW(nullptr), 0);
    } else {
        ::UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
}

bool StartMenuTyping::handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    // Ours: being typed again, the Esc that closes the Start menu for the
    // launcher (win::bringToFront). Other programs' are typing all the same
    // (keys remapped by AutoHotkey or PowerToys, the on-screen keyboard).
    if (event.dwExtraInfo == kOwnInput)
        return false;
    const UINT vk = event.vkCode;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (isModifier(vk)) {
        if (press && vk == VK_LMENU && !m_holding)
            m_windowsSearch = true;
        return false; // seen as they are; the keys held back carry Shift with them
    }
    const bool combination = held(VK_CONTROL) || held(VK_MENU) || held(VK_LWIN) || held(VK_RWIN);
    if (!m_holding) {
        if (!press || combination || m_windowsSearch || !m_startInFront || !startsTyping(vk))
            return false;
        m_holding = true;
        m_held.clear();
        m_timer = ::SetTimer(nullptr, 0, kHoldTimeoutMs, nullptr);
        if (m_callbacks.typed)
            m_callbacks.typed();
    } else if (combination) {
        return false;
    }
    m_held.push_back({static_cast<WORD>(vk), static_cast<WORD>(event.scanCode), (event.flags & LLKHF_EXTENDED) != 0,
        !press, press && held(VK_SHIFT)});
    return true;
}

void StartMenuTyping::typeHeld()
{
    if (!m_holding)
        return;
    m_holding = false;
    ::KillTimer(nullptr, m_timer);
    m_timer = 0;
    setHooked(m_startInFront); // the launcher is in front: gone
    // In order, with Shift around the keys pressed with it (unless it is
    // held now).
    const bool shiftNow = held(VK_SHIFT);
    std::vector<INPUT> inputs;
    for (const Held& key : m_held) {
        const DWORD flags = (key.extended ? KEYEVENTF_EXTENDEDKEY : 0) | (key.release ? KEYEVENTF_KEYUP : 0);
        const bool shifted = key.shift && !key.release && !shiftNow;
        if (shifted)
            inputs.push_back(keyInput(VK_LSHIFT, static_cast<WORD>(::MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC)), 0));
        inputs.push_back(keyInput(key.vk, key.scanCode, flags));
        if (shifted)
            inputs.push_back(keyInput(VK_LSHIFT, static_cast<WORD>(::MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC)),
                KEYEVENTF_KEYUP));
    }
    m_held.clear();
    if (!inputs.empty())
        ::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void StartMenuTyping::dropHeld()
{
    m_holding = false;
    m_held.clear();
    ::KillTimer(nullptr, m_timer);
    m_timer = 0;
    setHooked(m_startInFront);
}

} // namespace ws
