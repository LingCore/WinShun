#include "Foreground.h"

#include <QFileInfo>

#include <shellapi.h>

namespace ws::foreground {

namespace {

bool isDesktop(HWND hwnd)
{
    wchar_t className[32] {};
    ::GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return hwnd == ::GetShellWindow() || ::wcscmp(className, L"Progman") == 0 || ::wcscmp(className, L"WorkerW") == 0;
}

// All of its monitor, as a full-screen program covers it. Not a maximized
// window: with the taskbar set to hide itself, that one does too.
bool coversMonitor(HWND hwnd)
{
    if (::IsZoomed(hwnd) || ::IsIconic(hwnd))
        return false;
    MONITORINFO monitor {sizeof monitor};
    RECT window {};
    if (!::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)
        || !::GetWindowRect(hwnd, &window))
        return false;
    const RECT& screen = monitor.rcMonitor;
    return window.left <= screen.left && window.top <= screen.top && window.right >= screen.right
        && window.bottom >= screen.bottom;
}

} // namespace

QString programOf(HWND hwnd)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    QString program;
    wchar_t path[MAX_PATH] {};
    DWORD size = MAX_PATH;
    if (::QueryFullProcessImageNameW(process, 0, path, &size))
        program = QFileInfo(QString::fromWCharArray(path, static_cast<int>(size))).fileName();
    ::CloseHandle(process);
    return program;
}

ForegroundFacts facts()
{
    ForegroundFacts facts;
    const HWND hwnd = ::GetForegroundWindow();
    DWORD pid = 0;
    if (!hwnd || !::GetWindowThreadProcessId(hwnd, &pid) || pid == ::GetCurrentProcessId() || isDesktop(hwnd))
        return facts;
    facts.program = programOf(hwnd);
    facts.coversMonitor = coversMonitor(hwnd);
    QUERY_USER_NOTIFICATION_STATE state {};
    facts.exclusiveFullScreen
        = SUCCEEDED(::SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
    // Hidden with ShowCursor(FALSE), or with SetCursor(nullptr). Windows
    // hides it too while a touch screen or pen is used: that one is not.
    CURSORINFO cursor {sizeof cursor};
    if (::GetCursorInfo(&cursor) && !(cursor.flags & CURSOR_SUPPRESSED))
        facts.cursorHidden = !(cursor.flags & CURSOR_SHOWING) || !cursor.hCursor;
    RECT clip {};
    if (::GetClipCursor(&clip)) {
        const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int right = left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int bottom = top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        facts.cursorConfined = clip.left > left || clip.top > top || clip.right < right || clip.bottom < bottom;
    }
    return facts;
}

} // namespace ws::foreground
