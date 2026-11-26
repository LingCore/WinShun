#include "Foreground.h"

#include "Win32Util.h"

#include <shellapi.h>
#include <tlhelp32.h>

#include <string>

namespace ws::foreground {

bool isSearchHost(QStringView program)
{
    for (const auto host : {u"SearchHost.exe", u"StartMenuExperienceHost.exe", u"SearchApp.exe", u"SearchUI.exe"}) {
        if (program.compare(QStringView(host), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool isShellFlyout(QStringView program)
{
    return isSearchHost(program) || program.compare(u"ShellExperienceHost.exe", Qt::CaseInsensitive) == 0
        || program.compare(u"ShellHost.exe", Qt::CaseInsensitive) == 0;
}

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

QString programOfProcess(DWORD pid)
{
    if (pid == 0)
        return {};
    if (const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); process.valid()) {
        std::wstring path(32768, L'\0'); // a long path too
        DWORD size = static_cast<DWORD>(path.size());
        if (::QueryFullProcessImageNameW(process.get(), 0, path.data(), &size)) {
            path.resize(size);
            return QString::fromStdWString(path.substr(path.find_last_of(L'\\') + 1));
        }
    }
    // Not to be opened, not even for its name: a game an anti-cheat guards
    // (War3.exe from a battle platform, GitHub #121). The list of processes
    // names it all the same.
    const win32::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return {};
    PROCESSENTRY32W entry {sizeof entry};
    for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more; more = ::Process32NextW(snapshot.get(), &entry)) {
        if (entry.th32ProcessID == pid)
            return QString::fromWCharArray(entry.szExeFile);
    }
    return {};
}

QString programOf(HWND hwnd)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    return programOfProcess(pid);
}

ForegroundFacts facts()
{
    ForegroundFacts facts;
    const HWND hwnd = ::GetForegroundWindow();
    DWORD pid = 0;
    const DWORD thread = hwnd ? ::GetWindowThreadProcessId(hwnd, &pid) : 0;
    if (!thread || pid == ::GetCurrentProcessId() || isDesktop(hwnd))
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
    // A game that turns the view with the mouse may keep it with SetCapture
    // rather than ClipCursor.
    GUITHREADINFO gui {sizeof gui};
    facts.cursorCaptured = ::GetGUIThreadInfo(thread, &gui) && gui.hwndCapture;
    return facts;
}

} // namespace ws::foreground
