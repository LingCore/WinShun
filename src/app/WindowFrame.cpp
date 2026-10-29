#include "WindowFrame.h"

#include "FileIconProvider.h"
#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QStyleHints>
#include <QWindow>

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace ws {

namespace {

// Logical pixels along the left and right edges that widen a window with
// side sizing: the inset of the launcher's rows, whose highlight starts
// there, so a click on a row never sizes the window instead.
constexpr qreal kSideEdge = 6;

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
            if (!self->m_ownFrame && !self->m_resizable)
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
        case WM_NCACTIVATE:
            // DWM learns from the default handling whether the frame is
            // active, which the Mica backdrop depends on; Qt keeps the
            // message from it for a frameless window. -1: nothing to repaint.
            // The frame's dark mode too, in case Qt has set it back (see
            // win::setDarkFrame). Not activated, it looks active all the same.
            ::DefSubclassProc(hwnd, message, wParam, lParam);
            win::setDarkFrame(self->m_window, QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
            return ::DefWindowProcW(hwnd, message, self->m_noActivate ? TRUE : wParam, -1);
        case WM_MOUSEACTIVATE:
        case WM_POINTERACTIVATE: // touch and pen
            if (self->m_noActivate)
                return MA_NOACTIVATE; // PA_NOACTIVATE is the same
            break;
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
            if ((wParam == HTLEFT || wParam == HTRIGHT) && self->sidesOnly()) {
                emit self->sideGrabbed(wParam == HTLEFT); // followed from here (setSideSizing)
                return 0;
            }
            // The system's way there puts the window in front first (measured:
            // Windows reported it in front during the drag); straight to
            // moving it, the program behind keeps the front.
            if (wParam == HTCAPTION && self->m_noActivate)
                return ::DefWindowProcW(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, lParam);
            // A handle: dragged, it moves the window as the rest does; let go
            // before the pointer has gone the system's drag distance, a click.
            // Told apart here, before the move loop, which takes the release
            // and leaves no trace of a click (DragDetect follows the mouse
            // until one or the other, the button's release included).
            if (wParam == HTCAPTION && self->onHandle(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                const POINT at {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (::DragDetect(hwnd, at)) {
                    // The move loop starts from where the pointer is now, past
                    // the drag distance: the window catches up with it first,
                    // or it would trail the pointer by that much all the way
                    // (measured: 288 of 300 px).
                    POINT now {};
                    RECT rect {};
                    if (::GetCursorPos(&now) && ::GetWindowRect(hwnd, &rect))
                        ::SetWindowPos(hwnd, nullptr, rect.left + now.x - at.x, rect.top + now.y - at.y, 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
                    break; // the move loop, as for any press on the drag area
                }
                if (!(::GetAsyncKeyState(VK_ESCAPE) & 0x8000)) // Esc ends it too: no click then
                    QMetaObject::invokeMethod(self, &WindowFrame::handleClicked, Qt::QueuedConnection);
                return 0;
            }
            break;
        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTMAXBUTTON) {
                self->setMaximizeState(true, true);
                return 0;
            }
            if ((wParam == HTLEFT || wParam == HTRIGHT) && self->sidesOnly()) {
                emit self->sideDoubleClicked();
                return 0;
            }
            // Its first click did what a click there does; the second is not
            // a double click on the title bar (the launcher's goes home).
            if (wParam == HTCAPTION && self->onHandle(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
                return 0;
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
    // The frame's dark mode as it is shown too, not only as it activates
    // (WM_NCACTIVATE above): the clipboard and the preview open without the focus.
    win::keepDarkFrameOnShow(window);
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible) { // no leave message comes then
            m_trackingLeave = false;
            setPointer({-1, -1});
            setMaximizeState(false, false);
        }
    });
    if (auto* quick = qobject_cast<QQuickWindow*>(window)) { // afterNextFrame; both on the render thread
        connect(quick, &QQuickWindow::afterSynchronizing, this, [this] {
            if (m_frameArmed.exchange(false))
                m_frameSynced.store(true);
        }, Qt::DirectConnection);
        connect(quick, &QQuickWindow::frameSwapped, this, [this] {
            if (!m_frameSynced.exchange(false))
                return;
            // Handed over, not yet shown: a resize now would drop it, and the
            // screen keep the frame before (measured, 240 Hz). Shown once DWM
            // has composed again.
            ::DwmFlush();
            QMetaObject::invokeMethod(this, &WindowFrame::frameShown, Qt::QueuedConnection);
        }, Qt::DirectConnection);
    }
    if (!m_ownFrame)
        return;
    // For Windows a normal window (snapping, the animations, the window menu);
    // the frame itself is client area from now on (WM_NCCALCSIZE).
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

void WindowFrame::afterNextFrame()
{
    // This thread waits while the scene is synchronised, so the first
    // synchronisation after this takes the scene as it is now.
    m_frameArmed.store(true);
    if (auto* quick = qobject_cast<QQuickWindow*>(m_window.data()))
        quick->update();
}

void WindowFrame::setNoActivate(bool on)
{
    if (on == m_noActivate || !m_hwnd)
        return;
    m_noActivate = on;
    const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, on ? exStyle | WS_EX_NOACTIVATE : exStyle & ~WS_EX_NOACTIVATE);
    // The frame drawn as active (see WM_NCACTIVATE above), or as it is again.
    ::DefWindowProcW(hwnd, WM_NCACTIVATE, on || ::GetForegroundWindow() == hwnd, -1);
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

void WindowFrame::addPopup(QQuickItem* item)
{
    // Added each time it opens, and a plugin card's menu goes with its card:
    // the ones gone leave the list here.
    m_popups.removeIf([](const QPointer<QQuickItem>& popup) { return popup.isNull(); });
    if (item && !m_popups.contains(item))
        m_popups.append(item);
}

void WindowFrame::addHandle(QQuickItem* item)
{
    if (item)
        m_handles.append(item);
}

bool WindowFrame::onHandle(int screenX, int screenY) const
{
    return anyCovers(m_handles, toWindow(screenX, screenY));
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
    if (anyCovers(m_popups, pos))
        return HTCLIENT; // an open menu over a drag area or the title bar: the press is the menu's
    const bool onMaximize = m_ownFrame && covers(m_maximizeButton, pos);
    const bool onControl = onMaximize || anyCovers(m_controls, pos);
    if ((m_ownFrame || m_resizable) && !::IsZoomed(hwnd)) {
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
    if (sidesOnly() && !onControl && !::IsZoomed(hwnd)) {
        POINT point {screenX, screenY};
        ::ScreenToClient(hwnd, &point);
        RECT client {};
        ::GetClientRect(hwnd, &client);
        const qreal dpr = m_window ? m_window->devicePixelRatio() : 1.0;
        const LONG edge = std::max(2L, std::lround(kSideEdge * dpr));
        if (point.x < edge)
            return HTLEFT;
        if (point.x >= client.right - edge)
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

void WindowFrame::setResizable(bool on)
{
    if (on == m_resizable || !m_hwnd || m_ownFrame)
        return;
    m_resizable = on;
    // The sizing frame (what lets the system size it at the edges): with
    // WM_NCCALCSIZE answered above, none of it shows. Qt keeps the window
    // frameless in its own terms, so its geometry stays the whole window.
    const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, on ? style | WS_THICKFRAME : style & ~WS_THICKFRAME);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
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
