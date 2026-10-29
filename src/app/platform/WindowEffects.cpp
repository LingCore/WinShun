#include "WindowEffects.h"

#include "Foreground.h"
#include "OwnInput.h"

#include <QDebug>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QStyleHints>
#include <QWindow>

#include <dwmapi.h>
#include <commctrl.h>
#include <windows.h>
#include <msctf.h>

#include <atomic>
#include <mutex>

using namespace Qt::StringLiterals;

namespace ws::win {

namespace {

// Values from the Windows 11 SDK; spelled out so older SDKs still build.
constexpr DWORD kUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kBorderColor = 34; // DWMWA_BORDER_COLOR
constexpr DWORD kSystemBackdropType = 38; // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kCornerRound = 2; // DWMWCP_ROUND
constexpr int kCornerSquare = 1; // DWMWCP_DONOTROUND
constexpr int kCornerSmall = 3; // DWMWCP_ROUNDSMALL
constexpr wchar_t kCornerStyle[] = L"WinShun.CornerStyle";
std::atomic<bool> g_round {true};
std::mutex g_cornerMutex; // preview HWNDs are created on another thread
constexpr COLORREF kColorNone = 0xFFFFFFFE; // DWMWA_COLOR_NONE
constexpr int kBackdropMica = 2; // DWMSBT_MAINWINDOW
constexpr UINT kUpdateCornerRegion = WM_APP + 0x3C4;

struct CornerRegion {
    int width = 0;
    int height = 0;
    int radius = -1;
};

void updateCornerRegion(HWND window, CornerRegion& region)
{
    RECT rect {};
    if (!::GetWindowRect(window, &rect))
        return;
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const bool smallCorners = reinterpret_cast<INT_PTR>(::GetPropW(window, kCornerStyle)) == kCornerSmall;
    const int radius = g_round.load() && !::IsZoomed(window)
        ? ::MulDiv(smallCorners ? 4 : 8, static_cast<int>(::GetDpiForWindow(window)), 96)
        : 0;
    if (region.width == width && region.height == height && region.radius == radius)
        return;
    region = {width, height, radius}; // SetWindowRgn can reenter the window procedure.
    const HRGN mask = radius ? ::CreateRoundRectRgn(0, 0, width + 1, height + 1, 2 * radius, 2 * radius) : nullptr;
    if (!::SetWindowRgn(window, mask, TRUE) && mask)
        ::DeleteObject(mask); // ownership transfers only on success
}

LRESULT CALLBACK cornerRegionProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    auto* region = reinterpret_cast<CornerRegion*>(data);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(window, cornerRegionProc, id);
        delete region;
    } else if (message == WM_SIZE || message == WM_DPICHANGED || message == kUpdateCornerRegion) {
        updateCornerRegion(window, *region);
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

void applyWindowCorners(HWND window, int style)
{
    const int value = g_round.load() ? style : kCornerSquare;
    if (SUCCEEDED(::DwmSetWindowAttribute(window, kCornerPreference, &value, sizeof value)))
        return;
    if (::GetWindowThreadProcessId(window, nullptr) != ::GetCurrentThreadId()) {
        ::PostMessageW(window, kUpdateCornerRegion, 0, 0);
        return;
    }
    // Windows 10 has no DWM corner preference. Keep the same setting useful
    // there, including after resizing or moving to a monitor with another DPI.
    DWORD_PTR data = 0;
    if (!::GetWindowSubclass(window, cornerRegionProc, 0, &data)) {
        auto* region = new CornerRegion;
        if (!::SetWindowSubclass(window, cornerRegionProc, 0, reinterpret_cast<DWORD_PTR>(region))) {
            delete region;
            return;
        }
        data = reinterpret_cast<DWORD_PTR>(region);
    }
    updateCornerRegion(window, *reinterpret_cast<CornerRegion*>(data));
}

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
    if (!isShellFlyout(foreground))
        return foreground;
    INPUT esc[2] {};
    esc[0].type = esc[1].type = INPUT_KEYBOARD;
    esc[0].ki.wVk = esc[1].ki.wVk = VK_ESCAPE;
    esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
    esc[0].ki.dwExtraInfo = esc[1].ki.dwExtraInfo = kOwnInput; // TypeToSearch lets it through to the Start menu
    ::SendInput(2, esc, sizeof esc[0]);
    for (int waited = 0; waited < 500 && ::GetForegroundWindow() == foreground; waited += 10)
        ::Sleep(10); // closing takes some 150 ms
    return ::GetForegroundWindow();
}

} // namespace

bool isShellFlyout(HWND window)
{
    static const QStringList flyouts {
        u"SearchHost.exe"_s, u"StartMenuExperienceHost.exe"_s, u"ShellExperienceHost.exe"_s, u"ShellHost.exe"_s};
    return window && flyouts.contains(programOf(window), Qt::CaseInsensitive);
}

void styleFramelessWindow(QWindow* window, bool backdrop)
{
    const HWND hwnd = handleOf(window);
    if (!hwnd)
        return;
    setWindowCorners(hwnd);
    ::DwmSetWindowAttribute(hwnd, kBorderColor, &kColorNone, sizeof kColorNone);
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

void keepDarkFrameOnShow(QWindow* window)
{
    // visibleChanged comes before the native window is shown. Read first: set
    // only when it is wrong. Not created yet, there is nothing to keep (ours
    // are created, and set, before their first show).
    QObject::connect(window, &QWindow::visibleChanged, window, [window](bool visible) {
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        if (visible && window->handle() && isDarkFrame(window) != dark)
            setDarkFrame(window, dark);
    });
}

bool roundedCorners()
{
    return g_round.load();
}

void setWindowCorners(HWND window, bool smallCorners)
{
    if (!window)
        return;
    std::lock_guard lock(g_cornerMutex);
    const int style = smallCorners ? kCornerSmall : kCornerRound;
    ::SetPropW(window, kCornerStyle, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(style)));
    applyWindowCorners(window, style);
}

bool setRoundedCorners(bool on)
{
    std::lock_guard lock(g_cornerMutex);
    if (g_round.exchange(on) == on)
        return false;
    // Includes hidden menus and the preview worker's non-Qt popup. Window
    // properties die with the HWND, so destroyed windows cannot stay registered.
    ::EnumWindows(
        [](HWND window, LPARAM) -> BOOL {
            DWORD process = 0;
            ::GetWindowThreadProcessId(window, &process);
            if (process == ::GetCurrentProcessId()) {
                const auto style = reinterpret_cast<INT_PTR>(::GetPropW(window, kCornerStyle));
                if (style == kCornerRound || style == kCornerSmall) {
                    applyWindowCorners(window, static_cast<int>(style));
                }
            }
            return TRUE;
        },
        0);
    return true;
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

Front bringToFront(QWindow* window)
{
    const HWND hwnd = handleOf(window);
    if (!hwnd)
        return Front::Refused;
    // STARTF_USESHOWWINDOW/SW_HIDE can hide the first normal window even
    // after QWindow::show() marked it visible. A later show() then does
    // nothing. Honour this explicit request at the native boundary too.
    if (::IsIconic(hwnd))
        ::ShowWindow(hwnd, SW_RESTORE);
    else if (!::IsWindowVisible(hwnd))
        ::ShowWindow(hwnd, SW_SHOW);
    if (::GetForegroundWindow() == hwnd)
        return Front::Already;
    closeShellFlyout(::GetForegroundWindow());
    // Directly: Windows allows it to the process that got the latest input
    // (a hotkey, a click on its window or its tray icon) or that one let
    // (the UIAccess listener's AllowSetForegroundWindow, a command line's).
    // The window in front learns that it lost the focus on its own thread,
    // in its own time: busy, slow to deactivate or hanging, ours is in front
    // within 15 ms all the same (measured, GitHub #111). Never with the
    // input attached to its thread (AttachThreadInput), as before: that made
    // its deactivation ours to wait for, the 2 s of a busy program, 8.8 s of
    // a hanging one (#111), 6.9 s in #95, the GUI thread frozen meanwhile;
    // and it got the window to the front in no case this does not (#111).
    //
    // Otherwise (the latest input went elsewhere: a double Ctrl seen through
    // Raw Input, keys routed to the clipboard) the process that sent the
    // latest input may set the foreground: one that does nothing, a mouse
    // event without a move, a button or the wheel. PowerToys Run does the
    // same ("Send empty mouse event. This makes this thread the last to send
    // input, and hence allows it to pass foreground permission checks",
    // microsoft/PowerToys#4270). Not a key: a lone Shift, Alt or Ctrl
    // switches an input method or opens a menu bar, and a game sees any key.
    // It reaches Raw Input alone, as no move from no device, marked as ours
    // for Win顺's listeners; no low-level hook, no pointer move or shape
    // (measured 2026-10-27). Over an administrator's window, or the ghost of
    // a hung one, UIPI drops it and Windows refuses: only the UIAccess
    // listener's grant gets past those.
    //
    // Which of the two, asked before trying: AllowSetForegroundWindow for
    // this process succeeds only when it may set the foreground, and gives
    // it nothing it did not have. Trying is not free: a refused
    // SetForegroundWindow still makes the window the active one of its own
    // thread, in the background (WM_ACTIVATE and WM_SETFOCUS inside the
    // refused call, measured 2026-10-28), and Qt's work for that came before
    // the window got to the front: the first double Ctrl after the start,
    // without the UIAccess listener, took 10–26 ms instead of 5–10 (#111);
    // asked first, 7–8. A wrong answer costs nothing more: a wrong "no"
    // sends one empty input more (without the UIAccess listener it says no
    // every time, though Windows lets a direct try through after the first:
    // in front in 6–13 ms all the same, measured), a wrong "yes" ends up
    // sending it after all.
    bool sent = false;
    const auto sendNothing = [&sent] {
        INPUT nothing {};
        nothing.type = INPUT_MOUSE;
        nothing.mi.dwExtraInfo = kOwnInput;
        ::SendInput(1, &nothing, sizeof nothing); // a UIPI drop is not told (SendInput's documentation)
        sent = true;
    };
    if (!::AllowSetForegroundWindow(::GetCurrentProcessId()))
        sendNothing();
    if (activate(hwnd))
        return sent ? Front::Input : Front::Direct;
    if (!sent) {
        sendNothing();
        if (activate(hwnd))
            return Front::Input;
    }
    qWarning().noquote() << "Could not bring the window to the front over" << describe(::GetForegroundWindow());
    return Front::Refused;
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

bool isChineseLayout(HKL layout)
{
    // Its low word is the input language.
    return PRIMARYLANGID(static_cast<LANGID>(reinterpret_cast<UINT_PTR>(layout) & 0xFFFF)) == LANG_CHINESE;
}

} // namespace ws::win
