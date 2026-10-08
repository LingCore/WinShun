#include "WindowFrame.h"

#include "FileIconProvider.h"

#include <QCoreApplication>
#include <QDir>
#include <QWindow>

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

namespace ws {

namespace {

bool covers(const QPointer<QQuickItem>& item, QPointF pos)
{
    return item && item->isVisible()
        && item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).contains(pos);
}

bool anyCovers(const QList<QPointer<QQuickItem>>& items, QPointF pos)
{
    return std::any_of(items.begin(), items.end(), [pos](const QPointer<QQuickItem>& item) { return covers(item, pos); });
}

// The window menu, as a right click on the system title bar opens it.
void showWindowMenu(HWND hwnd, POINT pos)
{
    const HMENU menu = ::GetSystemMenu(hwnd, FALSE);
    if (!menu)
        return;
    const bool maximized = ::IsZoomed(hwnd);
    const auto enable = [menu](UINT command, bool on) {
        ::EnableMenuItem(menu, command, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
    };
    enable(SC_RESTORE, maximized);
    enable(SC_MOVE, !maximized);
    enable(SC_SIZE, !maximized);
    enable(SC_MINIMIZE, true);
    enable(SC_MAXIMIZE, !maximized);
    const UINT align = ::GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const BOOL command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | align, pos.x, pos.y, 0, hwnd, nullptr);
    if (command)
        ::PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(command), 0);
}

} // namespace

struct WindowFrame::Hook {
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<WindowFrame*>(data);
        switch (message) {
        case WM_NCCALCSIZE:
            if (!self->m_ownFrame)
                break;
            // All of it is client area. Maximised, a window overhangs the
            // monitor by its frame: then its client area is the work area, and
            // Qt counts the overhang as the frame (QTBUG-113736).
            if (wParam && ::IsZoomed(hwnd)) {
                RECT& rect = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0];
                MONITORINFO info {sizeof info};
                const RECT window = rect;
                if (::GetMonitorInfoW(::MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST), &info))
                    ::IntersectRect(&rect, &window, &info.rcWork);
            }
            return 0;
        case WM_NCHITTEST:
            return self->hitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        // Over drag areas and the maximise button the mouse messages are
        // non-client ones: QML hears of them from here.
        case WM_NCMOUSEMOVE:
            if (wParam == HTCAPTION || wParam == HTMAXBUTTON)
                self->trackLeave();
            self->setPointer(wParam == HTCAPTION ? self->toWindow(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)) : QPointF(-1, -1));
            self->setMaximizeState(wParam == HTMAXBUTTON, wParam == HTMAXBUTTON && self->m_maximizePressed);
            break;
        case WM_NCMOUSELEAVE:
            self->m_trackingLeave = false;
            self->setPointer({-1, -1});
            self->setMaximizeState(false, false);
            break;
        case WM_MOUSEMOVE: // into the client area
            self->setPointer({-1, -1});
            self->setMaximizeState(false, false);
            break;
        case WM_NCLBUTTONDOWN:
            if (wParam == HTMAXBUTTON) {
                self->setMaximizeState(true, true);
                return 0; // not to the system, which would track a button of its own
            }
            break;
        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTMAXBUTTON) {
                self->setMaximizeState(true, true);
                return 0;
            }
            if (wParam == HTCAPTION) {
                emit self->doubleClicked();
                if (!self->m_ownFrame)
                    return 0; // nothing to maximise
            }
            break;
        case WM_NCLBUTTONUP:
            if (wParam == HTMAXBUTTON && self->m_maximizePressed) {
                self->setMaximizeState(false, false); // the window changes under the pointer
                ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                return 0;
            }
            self->setMaximizeState(wParam == HTMAXBUTTON, false);
            break;
        case WM_NCRBUTTONUP:
            // The window menu with our own frame, nothing otherwise. Not to Qt:
            // it would take the context menu message that follows for one inside
            // the window, which all of it is.
            if (wParam == HTCAPTION) {
                if (self->m_ownFrame)
                    showWindowMenu(hwnd, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                return 0;
            }
            break;
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, &Hook::proc, 0);
            self->m_hwnd = 0;
            break;
        default:
            break;
        }
        return ::DefSubclassProc(hwnd, message, wParam, lParam);
    }
};

WindowFrame::WindowFrame(bool ownFrame, QObject* parent)
    : QObject(parent)
    , m_ownFrame(ownFrame)
{
}

WindowFrame::~WindowFrame()
{
    if (m_hwnd)
        ::RemoveWindowSubclass(reinterpret_cast<HWND>(m_hwnd), &Hook::proc, 0);
}

void WindowFrame::setWindow(QWindow* window)
{
    m_window = window;
    m_hwnd = window->winId();
    const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    ::SetWindowSubclass(hwnd, &Hook::proc, 0, reinterpret_cast<DWORD_PTR>(this));
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible) { // no leave message comes then
            m_trackingLeave = false;
            setPointer({-1, -1});
            setMaximizeState(false, false);
        }
    });
    if (!m_ownFrame)
        return;
    // For Windows a normal window (snapping, the animations, the window menu);
    // the frame itself is client area from now on (WM_NCCALCSIZE).
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

void WindowFrame::addDragArea(QQuickItem* item)
{
    if (item)
        m_dragAreas.append(item);
}

void WindowFrame::addControl(QQuickItem* item)
{
    if (item)
        m_controls.append(item);
}

QString WindowFrame::icon() const
{
    // Native separators: the shell does not parse "F:/..." and the generic
    // program icon would come back.
    return FileIconProvider::iconUrl(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), false);
}

QPointF WindowFrame::toWindow(int screenX, int screenY) const
{
    POINT point {screenX, screenY};
    ::ScreenToClient(reinterpret_cast<HWND>(m_hwnd), &point);
    const qreal dpr = m_window ? m_window->devicePixelRatio() : 1.0;
    return {point.x / dpr, point.y / dpr};
}

int WindowFrame::hitTest(int screenX, int screenY) const
{
    const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    const QPointF pos = toWindow(screenX, screenY);
    const bool onMaximize = m_ownFrame && covers(m_maximizeButton, pos);
    const bool onControl = onMaximize || anyCovers(m_controls, pos);
    if (m_ownFrame && !::IsZoomed(hwnd)) {
        // Resize edges as thick as the system frame, inside the window.
        POINT point {screenX, screenY};
        ::ScreenToClient(hwnd, &point);
        RECT client {};
        ::GetClientRect(hwnd, &client);
        const UINT dpi = ::GetDpiForWindow(hwnd);
        const int edge = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const bool top = point.y < edge;
        // Caption buttons reach the window's edge, as on the system's title
        // bar: beside them only the corner above resizes.
        const bool sides = top || !onControl;
        const bool left = sides && point.x < edge;
        const bool right = sides && point.x >= client.right - edge;
        if (top)
            return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (point.y >= client.bottom - edge)
            return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left)
            return HTLEFT;
        if (right)
            return HTRIGHT;
    }
    if (onMaximize)
        return HTMAXBUTTON;
    if (onControl)
        return HTCLIENT;
    if (anyCovers(m_dragAreas, pos))
        return HTCAPTION;
    return HTCLIENT;
}

void WindowFrame::setPointer(QPointF pointer)
{
    if (m_pointer == pointer)
        return;
    m_pointer = pointer;
    emit pointerChanged();
}

void WindowFrame::setMaximizeState(bool hovered, bool pressed)
{
    if (hovered == m_maximizeHovered && pressed == m_maximizePressed)
        return;
    m_maximizeHovered = hovered;
    m_maximizePressed = pressed;
    emit maximizeButtonChanged();
}

// Asks for WM_NCMOUSELEAVE once the pointer is over the non-client parts.
void WindowFrame::trackLeave()
{
    if (m_trackingLeave)
        return;
    TRACKMOUSEEVENT track {sizeof track, TME_LEAVE | TME_NONCLIENT, reinterpret_cast<HWND>(m_hwnd), 0};
    m_trackingLeave = ::TrackMouseEvent(&track) != FALSE;
}

} // namespace ws
