#include "WindowEffects.h"

#include "Foreground.h"

#include <QDebug>
#include <QOperatingSystemVersion>
#include <QWindow>

#include <dwmapi.h>
#include <windows.h>
#include <msctf.h>

using namespace Qt::StringLiterals;

namespace ws::win {

namespace {

// Values from the Windows 11 SDK; spelled out so older SDKs still build.
constexpr DWORD kUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kBorderColor = 34; // DWMWA_BORDER_COLOR
constexpr DWORD kSystemBackdropType = 38; // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kCornerRound = 2; // DWMWCP_ROUND
constexpr int kBackdropMica = 2; // DWMSBT_MAINWINDOW

HWND handleOf(QWindow* window)
{
    return window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
}

using foreground::programOf;

// "Notepad (notepad.exe)", for the log.
QString describe(HWND hwnd)
{
    if (!hwnd)
        return u"no window"_s;
    wchar_t className[256] {};
    ::GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return QString::fromWCharArray(className) + u" ("_s + programOf(hwnd) + u')';
}

bool activate(HWND hwnd)
{
    ::SetForegroundWindow(hwnd);
    ::BringWindowToTop(hwnd);
    ::SetFocus(hwnd);
    return ::GetForegroundWindow() == hwnd;
}

// Windows' Start menu, search, notification center and quick settings keep
// the foreground while they are open: no SetForegroundWindow gets past them,
// not with the input shared, after an input sent, or after Alt (measured
// 2026-10-09). Esc closes them as it would for the user, and the window that
// was in front before gets the foreground back. Returns the window in front then.
HWND closeShellFlyout(HWND foreground)
{
    static const QStringList flyouts {u"SearchHost.exe"_s, u"StartMenuExperienceHost.exe"_s,
        u"ShellExperienceHost.exe"_s, u"ShellHost.exe"_s};
    if (!foreground || !flyouts.contains(programOf(foreground), Qt::CaseInsensitive))
        return foreground;
    INPUT esc[2] {};
    esc[0].type = esc[1].type = INPUT_KEYBOARD;
    esc[0].ki.wVk = esc[1].ki.wVk = VK_ESCAPE;
    esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, esc, sizeof esc[0]);
    for (int waited = 0; waited < 500 && ::GetForegroundWindow() == foreground; waited += 10)
        ::Sleep(10); // closing takes some 150 ms
    return ::GetForegroundWindow();
}

} // namespace

void styleFramelessWindow(QWindow* window, QColor border, bool backdrop)
{
    const HWND hwnd = handleOf(window);
    if (!hwnd)
        return;
    ::DwmSetWindowAttribute(hwnd, kCornerPreference, &kCornerRound, sizeof kCornerRound);
    const COLORREF color = RGB(border.red(), border.green(), border.blue());
    ::DwmSetWindowAttribute(hwnd, kBorderColor, &color, sizeof color);
    // A one-pixel frame extension makes DWM draw its shadow around a
    // borderless window (and keeps working on Windows 10).
    const MARGINS margins {0, 0, 1, 0};
    ::DwmExtendFrameIntoClientArea(hwnd, &margins);
    // The backdrop fills the whole window, frame extended or not; extended,
    // DWM would also draw its caption buttons on it, next to ours.
    if (backdrop && backdropSupported())
        ::DwmSetWindowAttribute(hwnd, kSystemBackdropType, &kBackdropMica, sizeof kBackdropMica);
}

void setDarkFrame(QWindow* window, bool dark)
{
    if (const HWND hwnd = handleOf(window)) {
        const BOOL value = dark;
        ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value, sizeof value);
    }
}

bool isDarkFrame(QWindow* window)
{
    BOOL value = FALSE;
    if (const HWND hwnd = handleOf(window))
        ::DwmGetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value, sizeof value);
    return value;
}

bool backdropSupported()
{
    static const bool supported = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11_22H2;
    return supported;
}

bool materialsEnabled()
{
    HIGHCONTRASTW contrast {sizeof contrast};
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON))
        return false;
    DWORD transparency = 1;
    DWORD size = sizeof transparency;
    ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    return transparency != 0;
}

void setCloaked(QWindow* window, bool cloaked)
{
    if (const HWND hwnd = handleOf(window)) {
        const BOOL value = cloaked;
        ::DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof value);
    }
}

void bringToFront(QWindow* window)
{
    const HWND hwnd = handleOf(window);
    if (!hwnd)
        return;
    HWND foreground = ::GetForegroundWindow();
    if (foreground == hwnd)
        return;
    foreground = closeShellFlyout(foreground);
    const DWORD self = ::GetCurrentThreadId();
    const DWORD other = foreground ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;
    // Sharing the input state with the foreground thread lifts the
    // focus-stealing restriction for this one call. Not with a hung one:
    // its input stalls, and ours would with it while they are shared.
    const bool attached
        = other != 0 && other != self && !::IsHungAppWindow(foreground) && ::AttachThreadInput(other, self, TRUE);
    const bool done = activate(hwnd);
    if (attached)
        ::AttachThreadInput(other, self, FALSE);
    if (done)
        return;
    // Refused. The process that sent the latest input may change the
    // foreground: send one that does nothing (no move, no button) and try again.
    INPUT nothing {};
    nothing.type = INPUT_MOUSE;
    ::SendInput(1, &nothing, sizeof nothing);
    if (!activate(hwnd))
        qWarning().noquote() << "Could not bring the window to the front over" << describe(::GetForegroundWindow());
}

void setMenuTheme(bool dark)
{
    // uxtheme exports these by ordinal only (stable since Windows 10 1903).
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    static const HMODULE uxtheme = ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme)
        return;
    static const auto setMode = reinterpret_cast<SetPreferredAppMode>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    static const auto flush = reinterpret_cast<FlushMenuThemes>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (setMode)
        setMode(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush)
        flush();
}

void prepareTextInput()
{
    // Kept active for the life of the thread, and never released: COM is
    // gone by the time statics are destroyed.
    static ITfThreadMgr* manager = nullptr;
    if (manager)
        return;
    ITfThreadMgr* created = nullptr;
    TfClientId client = TF_CLIENTID_NULL;
    if (FAILED(::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created))))
        return;
    if (FAILED(created->Activate(&client))) {
        created->Release();
        return;
    }
    manager = created;
}

} // namespace ws::win
