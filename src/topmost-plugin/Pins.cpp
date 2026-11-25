#include "Pins.h"

#include <dwmapi.h>

#include <array>

namespace topmost {

namespace {

constexpr DWORD kWait = 400; // ms for a program to take the change

bool isShell(HWND hwnd)
{
    std::array<wchar_t, 64> name {};
    ::GetClassNameW(hwnd, name.data(), static_cast<int>(name.size()));
    const std::wstring_view cls(name.data());
    return hwnd == ::GetShellWindow() || cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd"
        || cls == L"Shell_SecondaryTrayWnd";
}

std::wstring titleOf(HWND hwnd)
{
    // Not GetWindowText: that one sends a message to a window of another
    // program, and waits for one that does not answer.
    std::array<wchar_t, 512> text {};
    const int length = ::InternalGetWindowText(hwnd, text.data(), static_cast<int>(text.size()));
    return std::wstring(text.data(), static_cast<std::size_t>(length > 0 ? length : 0));
}

} // namespace

bool listed(const Facts& f) noexcept
{
    if (!f.visible || f.cloaked || f.shell || f.ours || !f.titled || f.noActivate)
        return false;
    if (f.appWindow)
        return true;
    return !f.toolWindow && !f.owned;
}

Facts factsOf(HWND hwnd)
{
    Facts f;
    if (!hwnd || !::IsWindow(hwnd))
        return f;
    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    f.visible = ::IsWindowVisible(hwnd) != FALSE;
    DWORD cloaked = 0;
    f.cloaked = SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0;
    f.owned = ::GetWindow(hwnd, GW_OWNER) != nullptr;
    f.toolWindow = (ex & WS_EX_TOOLWINDOW) != 0;
    f.appWindow = (ex & WS_EX_APPWINDOW) != 0;
    f.noActivate = (ex & WS_EX_NOACTIVATE) != 0;
    f.titled = ::GetWindowTextLengthW(hwnd) > 0 || !titleOf(hwnd).empty();
    f.shell = isShell(hwnd);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    f.ours = pid == ::GetCurrentProcessId();
    return f;
}

bool pinned(HWND hwnd)
{
    return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

Window describe(HWND hwnd)
{
    Window w;
    w.hwnd = hwnd;
    w.title = titleOf(hwnd);
    w.pinned = pinned(hwnd);
    ::GetWindowThreadProcessId(hwnd, &w.pid);
    if (const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, w.pid)) {
        std::array<wchar_t, 1024> path {};
        DWORD size = static_cast<DWORD>(path.size());
        if (::QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
            w.path.assign(path.data(), size);
            const std::size_t slash = w.path.find_last_of(L'\\');
            w.program = slash == std::wstring::npos ? w.path : w.path.substr(slash + 1);
        }
        ::CloseHandle(process);
    }
    return w;
}

std::vector<Window> windows()
{
    std::vector<Window> found;
    ::EnumWindows(
        [](HWND hwnd, LPARAM out) -> BOOL {
            if (listed(factsOf(hwnd)))
                reinterpret_cast<std::vector<Window>*>(out)->push_back(describe(hwnd));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

HWND current()
{
    const HWND front = ::GetForegroundWindow();
    if (!front)
        return nullptr;
    for (const UINT which : {GA_ROOT, GA_ROOTOWNER}) {
        const HWND hwnd = ::GetAncestor(front, which);
        if (hwnd && listed(factsOf(hwnd)))
            return hwnd;
    }
    return nullptr;
}

Result setPinned(HWND hwnd, bool on)
{
    if (!hwnd || !::IsWindow(hwnd))
        return Result::Gone;
    if (pinned(hwnd) == on)
        return Result::Done;
    // Asked without waiting for its program (SWP_ASYNCWINDOWPOS): one that
    // does not answer would hold this plugin up with it.
    if (!::SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS))
        return ::IsWindow(hwnd) ? Result::Refused : Result::Gone;
    const ULONGLONG until = ::GetTickCount64() + kWait;
    while (pinned(hwnd) != on) {
        if (!::IsWindow(hwnd))
            return Result::Gone;
        if (::GetTickCount64() >= until)
            return ::IsHungAppWindow(hwnd) ? Result::Hung : Result::Refused;
        ::Sleep(5);
    }
    return Result::Done;
}

std::wstring lowered(std::wstring_view text)
{
    std::wstring out(text);
    if (!out.empty())
        ::CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

bool matches(const Window& window, std::wstring_view needle)
{
    return needle.empty() || lowered(window.title).find(needle) != std::wstring::npos
        || lowered(window.program).find(needle) != std::wstring::npos;
}

} // namespace topmost
