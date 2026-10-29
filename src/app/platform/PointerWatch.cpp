#include "PointerWatch.h"

#include "GameGuard.h"

namespace ws {

PointerWatch* PointerWatch::s_watch = nullptr;

PointerWatch::PointerWatch(KeyAt keyAt)
    : m_keyAt(std::move(keyAt))
{
    s_watch = this;
    m_foregroundHook = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        &PointerWatch::onEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // In front since some time unknown: keys pressed before now are not
    // known to have gone to it.
    foregroundChanged(::GetForegroundWindow(), ::GetTickCount());
}

PointerWatch::~PointerWatch()
{
    if (m_cursorHook)
        ::UnhookWinEvent(m_cursorHook);
    if (m_foregroundHook)
        ::UnhookWinEvent(m_foregroundHook);
    if (s_watch == this)
        s_watch = nullptr;
}

void CALLBACK PointerWatch::onEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD time)
{
    PointerWatch* watch = s_watch;
    if (!watch)
        return;
    if (hook == watch->m_foregroundHook && event == EVENT_SYSTEM_FOREGROUND)
        watch->foregroundChanged(hwnd, time);
    else if (hook == watch->m_cursorHook && object == OBJID_CURSOR)
        watch->cursorChanged(event, time);
}

void PointerWatch::foregroundChanged(HWND window, std::uint32_t time)
{
    m_foreground = window;
    m_foregroundAt = time;
    m_hide.reset(); // what this window does to the pointer from here on
    DWORD pid = 0;
    if (window)
        ::GetWindowThreadProcessId(window, &pid);
    watchProcess(pid == ::GetCurrentProcessId() ? 0 : pid);
}

void PointerWatch::watchProcess(DWORD pid)
{
    if (pid == m_pid && (m_cursorHook || !pid))
        return;
    if (m_cursorHook) {
        ::UnhookWinEvent(m_cursorHook);
        m_cursorHook = nullptr;
    }
    m_pid = pid;
    // SHOW and HIDE only (0x8002, 0x8003); for that process's windows too,
    // which the callback passes over.
    if (pid)
        m_cursorHook = ::SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, &PointerWatch::onEvent, pid,
            0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void PointerWatch::cursorChanged(DWORD event, std::uint32_t time)
{
    if (event == EVENT_OBJECT_SHOW) {
        m_hide.reset();
        return;
    }
    if (event != EVENT_OBJECT_HIDE)
        return;
    Hide hide;
    hide.at = time;
    ::GetCursorPos(&hide.where);
    BOOL vanish = FALSE;
    ::SystemParametersInfoW(SPI_GETMOUSEVANISH, 0, &vanish, 0);
    // A button held: hidden at a click (a game turning the view while one is
    // held), whatever key came before.
    const bool button = ::GetAsyncKeyState(VK_LBUTTON) < 0 || ::GetAsyncKeyState(VK_RBUTTON) < 0
        || ::GetAsyncKeyState(VK_MBUTTON) < 0;
    hide.vanish = vanish != FALSE && !button;
    // Found now, while the presses before it are surely kept
    // (KeyPresses::kKeptMs; typing on long after the hide pushes them out).
    hide.typing = hide.vanish && hiddenForTyping(true, time, m_keyAt ? m_keyAt(time) : std::nullopt, m_foregroundAt);
    m_hide = hide;
}

bool PointerWatch::hiddenByTyping()
{
    if (!m_hide || ::GetForegroundWindow() != m_foreground)
        return false;
    POINT now {};
    if (!::GetCursorPos(&now) || now.x != m_hide->where.x || now.y != m_hide->where.y)
        return false;
    // The press before the hide, seen only after this thread got to the
    // hide: this thread or the listener's was behind. Presses only add up,
    // so one found before stands.
    if (!m_hide->typing && m_hide->vanish && m_keyAt)
        m_hide->typing = hiddenForTyping(true, m_hide->at, m_keyAt(m_hide->at), m_foregroundAt);
    return m_hide->typing;
}

} // namespace ws
