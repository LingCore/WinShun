#include "KeyStream.h"

#include "RecordingPointer.h"

#include <QDebug>
#include <QPoint>

#include <algorithm>
#include <iterator>
#include <latch>
#include <utility>

namespace ws::capture::recording {

namespace {

// Windows calls a low-level hook on the thread that set it.
thread_local KeyHook* t_hook = nullptr;

constexpr UINT kFollowMessage = WM_APP; // to the hook thread: in or out again
// A window event can be missed: the window in front looked at this often too.
constexpr UINT kFollowMs = 250;
// A key down again after this long was let go unseen (to the secure desktop,
// an administrator's window, or while the hook was out): a press, not a repeat.
constexpr DWORD kLostReleaseMs = 1500;
// Input this long past the hooks' last call: Windows took them out.
constexpr std::uint32_t kDroppedMs = 500;
constexpr int kLoggedRefreshes = 5;

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool ownForeground()
{
    const HWND window = ::GetForegroundWindow();
    DWORD process = 0;
    if (window)
        ::GetWindowThreadProcessId(window, &process);
    return window && process == ::GetCurrentProcessId();
}

// A process's integrity level (SECURITY_MANDATORY_*_RID); one that cannot
// be looked at is taken as high (a program run as administrator, mostly).
DWORD integrityOf(DWORD processId)
{
    DWORD level = SECURITY_MANDATORY_HIGH_RID;
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return level;
    HANDLE token = nullptr;
    if (::OpenProcessToken(process, TOKEN_QUERY, &token)) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE] {};
        DWORD size = 0;
        if (::GetTokenInformation(token, TokenIntegrityLevel, buffer, DWORD(sizeof buffer), &size)) {
            const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
            level = *::GetSidSubAuthority(sid, DWORD(*::GetSidSubAuthorityCount(sid)) - 1);
        }
        ::CloseHandle(token);
    }
    ::CloseHandle(process);
    return level;
}

// Input goes where the hooks are not called: to a program above Win顺's
// integrity level (run as administrator: UIPI), or nowhere in this desktop
// (the secure one: a UAC prompt, Ctrl+Alt+Del).
bool inputUnseen(DWORD ownLevel)
{
    const HWND window = ::GetForegroundWindow();
    DWORD process = 0;
    if (!window || !::GetWindowThreadProcessId(window, &process))
        return true;
    return integrityOf(process) > ownLevel;
}

HKL foregroundLayout()
{
    const HWND window = ::GetForegroundWindow();
    return ::GetKeyboardLayout(window ? ::GetWindowThreadProcessId(window, nullptr) : 0);
}

qint64 timeOf(DWORD eventTime)
{
    const DWORD delay = ::GetTickCount() - eventTime; // the input's 32-bit clock, wraparound included
    const qint64 now = monotonicTime();
    return delay < 1000 ? now - qint64(delay) * 10000 : now;
}

// All mode: the character the key types in the program in front's layout.
std::array<char16_t, 2> character(std::uint16_t vk, DWORD scanCode, std::uint8_t mods, HKL layout)
{
    BYTE state[256] {};
    if (mods & keycaps::kShift)
        state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (mods & keycaps::kAltGr)
        state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    if (::GetKeyState(VK_CAPITAL) & 1)
        state[VK_CAPITAL] = 0x01;
    wchar_t buffer[4] {};
    // Flag 4: the keyboard's state is left alone, so a dead key waiting in
    // the program in front (^ before e) still waits (Windows 10 1607 on).
    const int count = ::ToUnicodeEx(vk, scanCode, state, buffer, int(std::size(buffer)), 4, layout);
    std::array<char16_t, 2> text {};
    if (count < 0) {
        text[0] = char16_t(buffer[0]); // a dead key: its accent
    } else {
        for (int i = 0; i < std::min(count, 2); ++i)
            text[size_t(i)] = char16_t(buffer[i]);
    }
    return text;
}

} // namespace

void KeyStream::push(const keycaps::Key& key)
{
    const std::lock_guard lock(m_mutex);
    if (key.time < m_after)
        return;
    if (m_pending.count == capacity) {
        std::move(m_pending.keys.begin() + 1, m_pending.keys.end(), m_pending.keys.begin());
        --m_pending.count;
    }
    m_pending.keys[m_pending.count++] = key;
}

KeyStream::Batch KeyStream::take()
{
    const std::lock_guard lock(m_mutex);
    Batch batch;
    if (!m_pending.count)
        return batch;
    batch = m_pending;
    std::fill_n(m_pending.keys.begin(), m_pending.count, keycaps::Key {});
    m_pending.count = 0;
    return batch;
}

void KeyStream::discardBefore(qint64 time)
{
    const std::lock_guard lock(m_mutex);
    std::fill_n(m_pending.keys.begin(), m_pending.count, keycaps::Key {});
    m_pending.count = 0;
    m_after = time;
}

bool inputWentPast(std::uint32_t lastInput, std::uint32_t lastCall) noexcept
{
    const std::uint32_t gap = lastInput - lastCall;
    return gap > kDroppedMs && gap < 0x80000000u;
}

KeyBook::Seen KeyBook::key(std::uint32_t vk, std::uint32_t scanCode, bool press, std::uint32_t time, Held held)
{
    settle(held);
    Seen seen;
    // AltGr: the left Ctrl Windows sends ahead of the right Alt is none.
    if (vk == VK_LCONTROL && (scanCode & 0x200)) {
        m_fakeCtrl = press;
        seen.skip = true;
        return seen;
    }
    seen.mods = modifiers(held);
    seen.vk = static_cast<std::uint16_t>(vk);
    if (vk == VK_RMENU) {
        if (press && m_fakeCtrl)
            m_altGr = true;
        if (m_altGr)
            seen.vk = keycaps::kAltGrKey;
        if (!press)
            m_altGr = false;
    }
    const size_t index = vk & 0xFF;
    if (press) {
        // A modifier down already as the system has it (held from before
        // the hook went in; its auto-repeat) is no press: let go, it would
        // be a tap of it. Another key: seen down and not let go since, lately.
        seen.repeat = keycaps::modifierOf(seen.vk) ? held(int(index))
                                                   : m_down.test(index) && time - m_downAt[index] < kLostReleaseMs;
        m_down.set(index);
        m_downAt[index] = time;
    } else {
        m_down.reset(index);
    }
    return seen;
}

std::uint8_t KeyBook::modifiers(Held held)
{
    settle(held);
    std::uint8_t mods = 0;
    if (held(VK_CONTROL) && !m_fakeCtrl)
        mods |= keycaps::kCtrl;
    if (held(VK_SHIFT))
        mods |= keycaps::kShift;
    if (held(VK_MENU))
        mods |= keycaps::kAlt;
    if (held(VK_LWIN) || held(VK_RWIN))
        mods |= keycaps::kWin;
    if (m_altGr) {
        mods &= ~keycaps::kAlt;
        mods |= keycaps::kAltGr;
    }
    return mods;
}

void KeyBook::resume(Held held)
{
    for (size_t index = 0; index < m_down.size(); ++index) {
        if (m_down.test(index) && !held(int(index)))
            m_down.reset(index); // let go while it was out
    }
    settle(held);
    // AltGr pressed while it was out: both its keys down, the left Ctrl not
    // seen down on its own. (Without AltGr in the layout, the right Alt
    // and the left Ctrl held over the switch are taken for it too.)
    if (!m_altGr && held(VK_RMENU) && held(VK_LCONTROL) && !m_down.test(VK_LCONTROL))
        m_altGr = m_fakeCtrl = true;
}

// Let go where the hook did not see it (the secure desktop: AltGr+Del, a
// UAC prompt; a program run as administrator): as the system has it now.
void KeyBook::settle(Held held)
{
    if (m_fakeCtrl && !held(VK_LCONTROL))
        m_fakeCtrl = false;
    if (m_altGr && !held(VK_RMENU))
        m_altGr = false;
}

KeyHook::KeyHook(Setup setup)
    : m_setup(std::move(setup))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_hook = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // Every key and click in the system waits for the hooks' answer.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        m_ownLevel = integrityOf(::GetCurrentProcessId());
        // Once at the start, whatever is in front: whether they can go in at all.
        m_failed = !install();
        if (m_failed) {
            remove();
            ready.count_down();
            return;
        }
        const HWINEVENTHOOK foreground = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            &KeyHook::foregroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        const UINT_PTR timer = ::SetTimer(nullptr, 0, kFollowMs, nullptr);
        follow();
        ready.count_down();
        // The hooks and the window event are called from inside GetMessage.
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == nullptr && (msg.message == kFollowMessage || msg.message == WM_TIMER)) {
                follow();
                if (msg.message == WM_TIMER)
                    checkDropped();
                continue;
            }
            ::DispatchMessageW(&msg);
        }
        if (timer)
            ::KillTimer(nullptr, timer);
        if (foreground)
            ::UnhookWinEvent(foreground);
        remove();
    });
    ready.wait();
}

KeyHook::~KeyHook()
{
    if (const DWORD thread = m_threadId.load())
        ::PostThreadMessageW(thread, WM_QUIT, 0, 0);
    if (m_thread.joinable())
        m_thread.join();
    if (m_refreshes > kLoggedRefreshes)
        qInfo() << "Recording keycaps: hooks put in again" << m_refreshes << "times in all";
}

void KeyHook::setPaused(bool paused)
{
    m_paused = paused;
    if (const DWORD thread = m_threadId.load())
        ::PostThreadMessageW(thread, kFollowMessage, 0, 0);
}

bool KeyHook::install()
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (m_setup.mode != keycaps::Mode::Off && !m_keyboard) {
        m_keyboard = ::SetWindowsHookExW(WH_KEYBOARD_LL, &KeyHook::keyboardProc, instance, 0);
        if (!m_keyboard)
            return false;
        m_book.resume(&held); // keys let go while it was out, AltGr pressed meanwhile
        m_calledAt = ::GetTickCount();
    }
    // The buttons, and for the modifiers' taps the clicks and the wheel with
    // them (Ctrl+click is no tap of Ctrl): wanted in every mode, so in with
    // the keyboard's. A move goes through it at once.
    if (!m_mouse) {
        m_mouse = ::SetWindowsHookExW(WH_MOUSE_LL, &KeyHook::mouseProc, instance, 0);
        if (!m_mouse)
            return false;
        m_calledAt = ::GetTickCount();
    }
    return true;
}

void KeyHook::remove()
{
    if (m_keyboard) {
        ::UnhookWindowsHookEx(m_keyboard);
        m_keyboard = nullptr;
    }
    if (m_mouse) {
        ::UnhookWindowsHookEx(m_mouse);
        m_mouse = nullptr;
    }
}

void KeyHook::follow(HWND forward)
{
    if (m_paused || ownForeground()) {
        remove();
        return;
    }
    const bool wasOut = !m_mouse;
    install();
    if (wasOut && forward && m_mouse)
        pressHeld(forward);
}

// Windows takes a low-level hook out without a word when it answers late
// (LowLevelHooksTimeout, a second at most: the thread starved, its pages
// on disk) and never calls it again. Input went on well past the hooks'
// last call: new ones in, the old ones out (KeyRouter does the same). Not
// while input goes where they are not called anyway.
void KeyHook::checkDropped()
{
    if (!m_mouse || m_paused)
        return;
    LASTINPUTINFO last {sizeof last, 0};
    if (!::GetLastInputInfo(&last) || !inputWentPast(last.dwTime, m_calledAt))
        return;
    if (inputUnseen(m_ownLevel)) {
        m_calledAt = last.dwTime; // looked at: again for input after it
        return;
    }
    refresh(long(last.dwTime - m_calledAt));
}

void KeyHook::refresh(long gapMs)
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    const HHOOK keyboard = m_keyboard ? ::SetWindowsHookExW(WH_KEYBOARD_LL, &KeyHook::keyboardProc, instance, 0) : nullptr;
    const HHOOK mouse = ::SetWindowsHookExW(WH_MOUSE_LL, &KeyHook::mouseProc, instance, 0);
    const DWORD error = ::GetLastError();
    m_calledAt = ::GetTickCount();
    if ((m_keyboard && !keyboard) || !mouse) {
        // The old ones kept: they may still be in.
        if (keyboard)
            ::UnhookWindowsHookEx(keyboard);
        if (mouse)
            ::UnhookWindowsHookEx(mouse);
        if (++m_refreshFailures <= kLoggedRefreshes)
            qWarning() << "Recording keycaps: hooks not put in again, error" << error;
        return;
    }
    // Out already, or behind the new ones until now: this thread is handed
    // no input in between.
    if (m_keyboard) {
        ::UnhookWindowsHookEx(m_keyboard);
        m_keyboard = keyboard;
        m_book.resume(&held);
    }
    ::UnhookWindowsHookEx(m_mouse);
    m_mouse = mouse;
    if (++m_refreshes <= kLoggedRefreshes)
        qInfo() << "Recording keycaps: input" << gapMs << "ms past the hooks' last call; put in again";
}

// The click that brought a program forward from Win顺's own window went
// by while the hooks were out: they go in on the switch it made, after the
// low-level hooks had it. A button still down then, over the window that
// came forward, is that click's (a click let go sooner is missed).
void KeyHook::pressHeld(HWND forward)
{
    POINT point {};
    if (!::GetPhysicalCursorPos(&point))
        return;
    const HWND under = ::WindowFromPhysicalPoint(point);
    if (!under || ::GetAncestor(under, GA_ROOT) != ::GetAncestor(forward, GA_ROOT))
        return; // focus came back some other way, a button held over another window
    // GetAsyncKeyState has the device's buttons; the hook, the system's
    // (swapped for the left-handed: the device's right one is the left).
    const bool swapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const std::array<std::pair<int, std::uint16_t>, 5> buttons {{
        {VK_LBUTTON, std::uint16_t(swapped ? VK_RBUTTON : VK_LBUTTON)},
        {VK_RBUTTON, std::uint16_t(swapped ? VK_LBUTTON : VK_RBUTTON)},
        {VK_MBUTTON, std::uint16_t(VK_MBUTTON)},
        {VK_XBUTTON1, std::uint16_t(VK_XBUTTON1)},
        {VK_XBUTTON2, std::uint16_t(VK_XBUTTON2)},
    }};
    const qint64 now = monotonicTime();
    for (const auto& [device, shown] : buttons) {
        if (held(device))
            pressButton(shown, now, point);
    }
}

void CALLBACK KeyHook::foregroundProc(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD)
{
    if (t_hook)
        t_hook->follow(window);
}

LRESULT CALLBACK KeyHook::keyboardProc(int code, WPARAM message, LPARAM lParam)
{
    if (t_hook)
        t_hook->m_calledAt = ::GetTickCount(); // still in (checkDropped)
    // The hooks set before this one first: a key one of them takes (a
    // remapper's) reached no program; its replacement comes as a key of its own.
    const LRESULT next = ::CallNextHookEx(nullptr, code, message, lParam);
    if (code == HC_ACTION && next == 0 && t_hook && t_hook->m_keyboard)
        t_hook->key(message, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam));
    return next;
}

LRESULT CALLBACK KeyHook::mouseProc(int code, WPARAM message, LPARAM lParam)
{
    if (t_hook)
        t_hook->m_calledAt = ::GetTickCount();
    const LRESULT next = ::CallNextHookEx(nullptr, code, message, lParam);
    if (code == HC_ACTION && next == 0 && message != WM_MOUSEMOVE && t_hook && t_hook->m_mouse)
        t_hook->button(message, *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam));
    return next;
}

std::uint8_t KeyHook::modifiers()
{
    return m_book.modifiers(&held);
}

void KeyHook::key(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    if (keycaps::hidden(event.vkCode, event.dwExtraInfo, event.flags))
        return;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const KeyBook::Seen seen = m_book.key(event.vkCode, event.scanCode, press, event.time, &held);
    if (seen.skip)
        return;
    const std::uint16_t vk = seen.vk;
    const std::uint8_t mods = seen.mods;
    const bool repeat = seen.repeat;
    keycaps::Key key;
    key.time = timeOf(event.time);
    key.down = press;
    key.repeat = repeat;
    key.mods = mods;
    if (!keycaps::passes(m_setup.mode, vk, mods)) {
        // Typing in Shortcuts mode: only that a key went down (a modifier
        // held over it is no tap), never which.
        if (!press || repeat)
            return;
        key.mods = 0;
        m_setup.output->push(key);
        return;
    }
    if (!press && !keycaps::modifierOf(vk))
        return; // of the releases, only the modifiers' matter
    key.vk = vk;
    if (press && !keycaps::modifierOf(vk)) {
        const HKL layout = foregroundLayout();
        if (vk >= VK_OEM_1 && vk <= VK_OEM_102)
            key.legend = char16_t(::MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, layout) & 0x7FFF);
        if (m_setup.mode == keycaps::Mode::All && !(mods & keycaps::kCommandMods))
            key.text = character(vk, event.scanCode, mods, layout);
    }
    m_setup.output->push(key);
}

void KeyHook::button(WPARAM message, const MSLLHOOKSTRUCT& event)
{
    if (keycaps::hidden(VK_LBUTTON, event.dwExtraInfo, event.flags))
        return; // Win顺's own clicks
    keycaps::Key key;
    switch (message) {
    case WM_LBUTTONDOWN:
        key.vk = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        key.vk = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        key.vk = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN:
        key.vk = HIWORD(event.mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        // Ctrl+wheel (zoom, a touchpad's pinch) is no tap of Ctrl.
        key.mods = modifiers();
        if (!key.mods)
            return;
        key.time = timeOf(event.time);
        m_setup.output->push(key); // kHiddenKey
        return;
    default:
        return;
    }
    pressButton(key.vk, timeOf(event.time), event.pt);
}

// point: physical, as Win顺 is per-monitor DPI aware.
void KeyHook::pressButton(std::uint16_t vk, qint64 time, POINT point)
{
    keycaps::Key key;
    key.vk = vk;
    key.time = time;
    key.mods = modifiers();
    key.inside = m_setup.region.contains(QPoint(point.x, point.y));
    if (key.inside && m_setup.excludedWindow) {
        const HWND panel = reinterpret_cast<HWND>(m_setup.excludedWindow);
        RECT rect {};
        if (::IsWindowVisible(panel) && ::GetWindowRect(panel, &rect) && ::PtInRect(&rect, point))
            key.inside = false;
    }
    m_setup.output->push(key);
}

} // namespace ws::capture::recording
