#include "WindowEffects.h"

#include <QWindow>

#include <dwmapi.h>
#include <windows.h>

namespace qf::win {

namespace {

// Values from the Windows 11 SDK; spelled out so older SDKs still build.
constexpr DWORD kUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kBorderColor = 34; // DWMWA_BORDER_COLOR
constexpr DWORD kCaptionColor = 35; // DWMWA_CAPTION_COLOR
constexpr DWORD kTextColor = 36; // DWMWA_TEXT_COLOR
constexpr COLORREF kDefaultColor = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
constexpr int kCornerRound = 2; // DWMWCP_ROUND

HWND handleOf(QWindow* window)
{
    return window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
}

} // namespace

void styleFramelessWindow(QWindow* window, bool dark, QColor border)
{
    const HWND hwnd = handleOf(window);
    if (!hwnd)
        return;
    const BOOL darkMode = dark;
    ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &darkMode, sizeof darkMode);
    ::DwmSetWindowAttribute(hwnd, kCornerPreference, &kCornerRound, sizeof kCornerRound);
    const COLORREF color = RGB(border.red(), border.green(), border.blue());
    ::DwmSetWindowAttribute(hwnd, kBorderColor, &color, sizeof color);
    // A one-pixel frame extension makes DWM draw its shadow around a
    // borderless window (and keeps working on Windows 10).
    const MARGINS margins {0, 0, 1, 0};
    ::DwmExtendFrameIntoClientArea(hwnd, &margins);
}

void setDarkTitleBar(QWindow* window, bool dark)
{
    if (const HWND hwnd = handleOf(window)) {
        const BOOL darkMode = dark;
        ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &darkMode, sizeof darkMode);
    }
}

void setTitleBarColors(QWindow* window, QColor caption, QColor text)
{
    const HWND hwnd = handleOf(window);
    if (!hwnd)
        return;
    const auto colorRef = [](QColor c) { return c.isValid() ? RGB(c.red(), c.green(), c.blue()) : kDefaultColor; };
    const COLORREF captionColor = colorRef(caption);
    const COLORREF textColor = colorRef(text);
    ::DwmSetWindowAttribute(hwnd, kCaptionColor, &captionColor, sizeof captionColor);
    ::DwmSetWindowAttribute(hwnd, kTextColor, &textColor, sizeof textColor);
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
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == hwnd)
        return;
    const DWORD self = ::GetCurrentThreadId();
    const DWORD other = foreground ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;
    // Sharing the input state with the foreground thread lifts the
    // focus-stealing restriction for this one call.
    const bool attached = other != 0 && other != self && ::AttachThreadInput(other, self, TRUE);
    ::SetForegroundWindow(hwnd);
    ::BringWindowToTop(hwnd);
    ::SetFocus(hwnd);
    if (attached)
        ::AttachThreadInput(other, self, FALSE);
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

} // namespace qf::win
