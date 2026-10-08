#include "Placement.h"

#include <QEasingCurve>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QWindow>

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

const QPointF kHome {0.5, 0.2}; // centred, a fifth of the way down
constexpr int kSnapDistance = 12; // logical pixels, where a dropped window settles home
constexpr int kGlideMs = 220;

// The scale Qt draws at on a monitor.
qreal scaleOf(HMONITOR monitor, qreal fallback)
{
    for (QScreen* screen : QGuiApplication::screens()) {
        const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            return screen->devicePixelRatio();
    }
    return fallback;
}

// Where a window being moved goes instead of `rect` (physical pixels): on the
// work area of the monitor under the pointer with `room` (logical pixels)
// below its top.
void shapeMove(RECT& rect, int width, int room, qreal fallbackScale)
{
    POINT cursor {};
    ::GetCursorPos(&cursor);
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(monitor, &info))
        return;
    const RECT& area = info.rcWork;
    // Sizes as they will be on that monitor once the window is over there.
    const qreal scale = scaleOf(monitor, fallbackScale);
    width = qRound(width * scale);
    room = qRound(room * scale);
    const LONG x = std::clamp(rect.left, area.left, std::max(area.left, area.right - width));
    const LONG y = std::clamp(rect.top, area.top, std::max(area.top, area.bottom - room));
    ::OffsetRect(&rect, x - rect.left, y - rect.top);
}

// A screen's work area in physical pixels, and where Qt puts a window's
// logical geometry on it: from the screen's origin (the same in both), each
// coordinate and length scaled and rounded (QHighDpi::toNativeWindowGeometry).
struct NativeArea {
    QRect work;
    QPoint origin;
    qreal scale = 1;

    int toNative(int pos, int from) const { return from + qRound((pos - from) * scale); }

    // `pos` (logical), moved by whole logical pixels until the span from it,
    // `length` long, lies within [lo, hi) physically. The start wins when
    // both cannot.
    int keepInside(int pos, int length, int lo, int hi, int from) const
    {
        const int extent = qRound(length * scale);
        for (int i = 0; i < 4 && toNative(pos, from) + extent > hi; ++i)
            --pos;
        for (int i = 0; i < 4 && toNative(pos, from) < lo; ++i)
            ++pos;
        return pos;
    }
};

NativeArea nativeArea(QScreen* screen)
{
    NativeArea native {screen->availableGeometry(), screen->geometry().topLeft()};
    const auto* windows = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    MONITORINFO info {sizeof info};
    if (windows && ::GetMonitorInfoW(windows->handle(), &info)) {
        const RECT& work = info.rcWork;
        native.work = QRect(work.left, work.top, work.right - work.left, work.bottom - work.top);
        native.scale = screen->devicePixelRatio();
    }
    return native;
}

} // namespace

Placement::Placement(QString stateFile, QObject* parent)
    : QObject(parent)
    , m_stateFile(std::move(stateFile))
    , m_anchor(kHome)
{
    const QSettings state(m_stateFile, QSettings::IniFormat);
    bool okX = false;
    bool okY = false;
    const double x = state.value(u"Launcher/CenterX"_s).toDouble(&okX);
    const double y = state.value(u"Launcher/Top"_s).toDouble(&okY);
    if (okX && okY && x >= 0 && x <= 1 && y >= 0 && y <= 1)
        m_anchor = {x, y};

    m_glide.setDuration(kGlideMs);
    m_glide.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_glide, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        if (m_window)
            m_window->setPosition(value.toPoint());
    });
}

struct Placement::Hook {
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<Placement*>(data);
        switch (message) {
        case WM_ENTERSIZEMOVE:
            self->m_glide.stop();
            self->m_moveStart = self->m_window->position();
            self->m_grabbed = false;
            self->setMoving(true);
            break;
        case WM_MOVING: {
            // Windows proposes where the window was plus the pointer's step,
            // so a window held at the work area's edge would lag the pointer
            // when coming back. It goes where the pointer puts it instead, at
            // the offset of the first proposal.
            RECT& rect = *reinterpret_cast<RECT*>(lParam);
            POINT cursor {};
            ::GetCursorPos(&cursor);
            if (!self->m_grabbed) {
                self->m_grab = QPoint(int(cursor.x - rect.left), int(cursor.y - rect.top));
                self->m_grabbed = true;
            }
            ::OffsetRect(&rect, cursor.x - self->m_grab.x() - rect.left, cursor.y - self->m_grab.y() - rect.top);
            // Only what is there: room for rows not shown is made on letting go.
            shapeMove(rect, self->m_window->width(), self->m_window->height(), self->m_window->devicePixelRatio());
            return TRUE;
        }
        case WM_EXITSIZEMOVE:
            self->setMoving(false);
            // Qt updates the window's screen on this message too, after us.
            QMetaObject::invokeMethod(self, &Placement::rememberSpot, Qt::QueuedConnection);
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

Placement::~Placement()
{
    if (m_hwnd)
        ::RemoveWindowSubclass(reinterpret_cast<HWND>(m_hwnd), &Hook::proc, 0);
}

void Placement::setWindow(QWindow* window)
{
    m_window = window;
    m_hwnd = window->winId();
    ::SetWindowSubclass(reinterpret_cast<HWND>(m_hwnd), &Hook::proc, 0, reinterpret_cast<DWORD_PTR>(this));
}

void Placement::placeOn(QScreen* screen)
{
    m_glide.stop();
    if (m_window && screen)
        m_window->setPosition(settleOn(screen));
}

void Placement::setRoomNeeded(int height)
{
    if (m_roomNeeded == height)
        return;
    m_roomNeeded = height;
    emit roomNeededChanged();
}

// The anchor's spot on the screen's work area, moved as far as needed to keep
// the window on it with room below for the fewest rows. More rows than fit
// there are not shown (room), so a long list does not push the window up.
QPoint Placement::settleOn(QScreen* screen)
{
    const QRect area = screen->availableGeometry();
    const int width = m_window->width();
    int x = qRound(area.x() + m_anchor.x() * area.width() - width / 2.0);
    int y = qRound(area.y() + m_anchor.y() * area.height());
    x = std::clamp(x, area.left(), std::max(area.left(), area.x() + area.width() - width));
    y = std::clamp(y, area.top(), std::max(area.top(), area.y() + area.height() - m_roomNeeded));

    // The same in physical pixels. Qt rounds a window's position and its size
    // each on its own (and the logical work area too), so an edge clamped in
    // logical pixels can end up a pixel past the work area, over the taskbar.
    const NativeArea native = nativeArea(screen);
    x = native.keepInside(x, width, native.work.left(), native.work.x() + native.work.width(), native.origin.x());
    y = native.keepInside(y, m_roomNeeded, native.work.top(), native.work.y() + native.work.height(), native.origin.y());
    const int bottom = native.work.y() + native.work.height();
    const int room = int(std::floor((bottom - native.toNative(y, native.origin.y())) / native.scale));
    if (m_room != room) {
        m_room = room;
        emit roomChanged();
    }
    return {x, y};
}

void Placement::moveHome()
{
    if (!m_window || m_moving)
        return;
    m_anchor = kHome;
    save();
    QScreen* screen = m_window->screen();
    if (!screen)
        return;
    glideTo(settleOn(screen));
}

void Placement::setMoving(bool moving)
{
    if (m_moving == moving)
        return;
    m_moving = moving;
    emit movingChanged();
}

void Placement::rememberSpot()
{
    if (!m_window || m_window->position() == m_moveStart) // Esc, or not moved at all
        return;
    const QRect frame = m_window->geometry();
    QScreen* screen = QGuiApplication::screenAt(frame.center());
    if (!screen)
        screen = m_window->screen();
    const QRect area = screen ? screen->availableGeometry() : QRect();
    if (area.isEmpty())
        return;
    QPointF anchor((frame.x() + frame.width() / 2.0 - area.x()) / area.width(),
        qreal(frame.y() - area.y()) / area.height());
    // Let go near the centre line or the home height: settles onto it, the
    // same on every monitor. Only now, so the drag itself never stalls.
    if (std::abs(anchor.x() - kHome.x()) * area.width() <= kSnapDistance)
        anchor.rx() = kHome.x();
    if (std::abs(anchor.y() - kHome.y()) * area.height() <= kSnapDistance)
        anchor.ry() = kHome.y();
    m_anchor = {std::clamp(anchor.x(), 0.0, 1.0), std::clamp(anchor.y(), 0.0, 1.0)};
    save();
    glideTo(settleOn(screen));
}

void Placement::glideTo(const QPoint& target)
{
    m_glide.stop();
    if (!m_window || target == m_window->position())
        return;
    m_glide.setStartValue(m_window->position());
    m_glide.setEndValue(target);
    m_glide.start();
}

void Placement::save() const
{
    QSettings state(m_stateFile, QSettings::IniFormat);
    state.setValue(u"Launcher/CenterX"_s, m_anchor.x());
    state.setValue(u"Launcher/Top"_s, m_anchor.y());
}

} // namespace ws
