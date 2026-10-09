#include "WindowLogo.h"

#include <commctrl.h>
#include <dwmapi.h>

namespace ws {

struct WindowLogo::Hook {
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<WindowLogo*>(data);
        if (message == WM_WINDOWPOSCHANGED) {
            // Moved (dragged, gliding) or resized: the logo too, now.
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (self->m_shown && (!(pos->flags & SWP_NOMOVE) || !(pos->flags & SWP_NOSIZE)))
                self->place();
        } else if (message == WM_NCDESTROY) {
            ::RemoveWindowSubclass(hwnd, &Hook::proc, 0);
            self->m_hwnd = nullptr;
        }
        return ::DefSubclassProc(hwnd, message, wParam, lParam);
    }
};

WindowLogo::WindowLogo(QWindow* window, int rowHeight)
    : QObject(window)
    , m_window(window)
    , m_hwnd(reinterpret_cast<HWND>(window->winId()))
    , m_rowHeight(rowHeight)
{
    ::SetWindowSubclass(m_hwnd, &Hook::proc, 0, reinterpret_cast<DWORD_PTR>(this));
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible) {
            m_shown = false;
            m_logo.hide();
        }
    });
}

WindowLogo::~WindowLogo()
{
    if (m_hwnd)
        ::RemoveWindowSubclass(m_hwnd, &Hook::proc, 0);
}

void WindowLogo::reveal()
{
    if (!m_window || !m_window->isVisible())
        return;
    m_shown = true;
    place();
}

void WindowLogo::place()
{
    if (!m_hwnd)
        return;
    RECT host {};
    if (FAILED(::DwmGetWindowAttribute(m_hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &host, sizeof host)))
        ::GetWindowRect(m_hwnd, &host);
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &info))
        return;
    // The window's own DPI: right in the middle of moving to another screen,
    // where Qt's devicePixelRatio is not yet.
    const qreal scale = ::GetDpiForWindow(m_hwnd) / 96.0;
    const RECT row {host.left, host.top, host.right, host.top + qRound(m_rowHeight * scale)};
    m_logo.show(host, row, info.rcWork, m_avoid ? &*m_avoid : nullptr, scale);
}

} // namespace ws
