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
constexpr int kBoxGap = 4; // logical pixels, between a box and the window attached to it
// Logical pixels between the taskbar and a window over it, and that window
// and the work area's edges: Windows 11's Start menu and search keep 12.
constexpr int kTaskbarGap = 12;
// The launcher's search box (SearchBar.qml): by a taskbar on the left or
// right, it is level with the button.
constexpr int kHeaderHeight = 64;

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

Placement::Placement(QString stateFile, QString group, QObject* parent)
    : QObject(parent)
    , m_stateFile(std::move(stateFile))
    , m_group(std::move(group))
    , m_anchor(kHome)
{
    const QSettings state(m_stateFile, QSettings::IniFormat);
    for (const QString& section : {m_group, u"Launcher"_s}) {
        bool okX = false;
        bool okY = false;
        const double x = state.value(section + u"/CenterX"_s).toDouble(&okX);
        const double y = state.value(section + u"/Top"_s).toDouble(&okY);
        if (okX && okY && x >= 0 && x <= 1 && y >= 0 && y <= 1) {
            m_anchor = {x, y};
            break;
        }
    }

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
    // Wider (the clipboard's preview), or taller over a box: kept on the
    // screen, and over a box its bottom stays there.
    connect(window, &QWindow::widthChanged, this, &Placement::refit);
    connect(window, &QWindow::heightChanged, this, [this] {
        if ((m_box && m_above) || m_taskbar)
            refit();
    });
}

void Placement::placeOn(QScreen* screen)
{
    m_glide.stop();
    m_box.reset();
    setTaskbar({});
    m_dropped = false;
    if (m_window && screen)
        m_window->setPosition(settleOn(screen));
}

void Placement::attach(const QRect& box)
{
    m_glide.stop();
    m_box = box;
    setTaskbar({});
    m_dropped = false;
    m_left = box.x();
    QScreen* screen = QGuiApplication::screenAt(box.center());
    if (!screen && m_window)
        screen = m_window->screen();
    if (m_window && screen)
        m_window->setPosition(settleBy(screen));
}

void Placement::attachNative(const QRect& box)
{
    // In Qt's pixels on the screen it is on: from that screen's origin (the
    // same in both) scaled, outwards, so the box covers all of it.
    const HMONITOR monitor = ::MonitorFromPoint({box.x(), box.y() + box.height() / 2}, MONITOR_DEFAULTTONEAREST);
    QScreen* screen = QGuiApplication::primaryScreen();
    for (QScreen* candidate : QGuiApplication::screens()) {
        const auto* native = candidate->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            screen = candidate;
    }
    if (!screen)
        return;
    const QPoint origin = screen->geometry().topLeft();
    const qreal scale = screen->devicePixelRatio();
    const auto down = [&](int pos, int from) { return from + int(std::floor((pos - from) / scale)); };
    const auto up = [&](int pos, int from) { return from + int(std::ceil((pos - from) / scale)); };
    const int left = down(box.x(), origin.x());
    const int top = down(box.y(), origin.y());
    attach(QRect(QPoint(left, top), QPoint(std::max(left, up(box.x() + box.width(), origin.x()) - 1),
        std::max(top, up(box.y() + box.height(), origin.y()) - 1))));
}

void Placement::attachTaskbar(const taskbar::Spot& spot)
{
    m_glide.stop();
    const HMONITOR monitor = ::MonitorFromPoint(spot.anchor, MONITOR_DEFAULTTONEAREST);
    QScreen* screen = nullptr;
    for (QScreen* candidate : QGuiApplication::screens()) {
        const auto* native = candidate->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            screen = candidate;
    }
    if (!screen || !m_window)
        return;
    // In Qt's pixels on that screen: from its origin (the same in both)
    // scaled; the taskbar outwards, so the window keeps clear of all of it.
    const QPoint origin = screen->geometry().topLeft();
    const qreal scale = screen->devicePixelRatio();
    const auto down = [&](LONG pos, int from) { return from + int(std::floor((pos - from) / scale)); };
    const auto up = [&](LONG pos, int from) { return from + int(std::ceil((pos - from) / scale)); };
    const RECT& bar = spot.taskbar;
    const QRect logicalBar(QPoint(down(bar.left, origin.x()), down(bar.top, origin.y())),
        QPoint(up(bar.right, origin.x()) - 1, up(bar.bottom, origin.y()) - 1));
    m_box.reset();
    m_dropped = false;
    m_taskbarScreen = screen;
    setTaskbar(TaskbarBox {logicalBar, spot.edge,
        {down(spot.anchor.x, origin.x()), down(spot.anchor.y, origin.y())}});
    m_window->setPosition(settleByTaskbar(screen));
}

void Placement::setRoomNeeded(int height)
{
    if (m_roomNeeded == height)
        return;
    m_roomNeeded = height;
    emit roomNeededChanged();
}

void Placement::setAnchorWidth(int width)
{
    if (m_anchorWidth == width)
        return;
    m_anchorWidth = width;
    emit anchorWidthChanged();
}

void Placement::setTaskbar(std::optional<TaskbarBox> box)
{
    const bool was = m_taskbar.has_value();
    m_taskbar = std::move(box);
    if (was != m_taskbar.has_value())
        emit atTaskbarChanged();
}

void Placement::setRoom(int room)
{
    if (m_room == room)
        return;
    m_room = room;
    emit roomChanged();
}

// The anchor's spot on the screen's work area, moved as far as needed to keep
// the window on it with room below for the fewest rows. More rows than fit
// there are not shown (room), so a long list does not push the window up.
QPoint Placement::settleOn(QScreen* screen)
{
    const QRect area = screen->availableGeometry();
    const int width = m_window->width();
    const int anchorWidth = m_anchorWidth > 0 ? std::min(m_anchorWidth, width) : width;
    int x = qRound(area.x() + m_anchor.x() * area.width() - anchorWidth / 2.0);
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
    setRoom(int(std::floor((bottom - native.toNative(y, native.origin.y())) / native.scale)));
    return {x, y};
}

// Under the box, level with its left edge (moved in to stay on the work
// area); over it where the fewest rows would not fit under it and more of
// them fit over it.
QPoint Placement::settleBy(QScreen* screen)
{
    const QRect area = screen->availableGeometry();
    const NativeArea native = nativeArea(screen);
    const int right = native.work.x() + native.work.width();
    const int bottom = native.work.y() + native.work.height();
    const int width = m_window->width();
    int x = std::clamp(m_left, area.left(), std::max(area.left(), area.x() + area.width() - width));
    x = native.keepInside(x, width, native.work.left(), right, native.origin.x());

    const int under = m_box->y() + m_box->height() + kBoxGap;
    const int over = m_box->y() - kBoxGap;
    const int roomUnder = int(std::floor((bottom - native.toNative(under, native.origin.y())) / native.scale));
    const int roomOver = int(std::floor((native.toNative(over, native.origin.y()) - native.work.y()) / native.scale));
    m_above = roomUnder < m_roomNeeded && roomOver > roomUnder;
    setRoom(std::max(0, m_above ? roomOver : roomUnder)); // the window takes its height from it
    const int height = m_window->height();
    int y = m_above ? over - height : under;
    y = std::clamp(y, area.top(), std::max(area.top(), area.y() + area.height() - height));
    y = native.keepInside(y, height, native.work.top(), bottom, native.origin.y());
    return {x, y};
}

// Off the taskbar by a gap, on the work area's side of it, and as far from
// the work area's edges; along the taskbar centred on the button (by a
// taskbar on the left or right: the search box level with it). The rows get
// the room from there to the far side of the work area.
QPoint Placement::settleByTaskbar(QScreen* screen)
{
    const QRect area = screen->availableGeometry();
    const NativeArea native = nativeArea(screen);
    const int right = native.work.x() + native.work.width();
    const int bottom = native.work.y() + native.work.height();
    const int width = m_window->width();
    const int height = m_window->height();
    const auto alongX = [&](int x) {
        x = std::clamp(x, area.left() + kTaskbarGap, std::max(area.left() + kTaskbarGap, area.right() + 1 - kTaskbarGap - width));
        return native.keepInside(x, width, native.work.left(), right, native.origin.x());
    };
    const auto alongY = [&](int y) {
        y = std::clamp(y, area.top() + kTaskbarGap, std::max(area.top() + kTaskbarGap, area.bottom() + 1 - kTaskbarGap - height));
        return native.keepInside(y, height, native.work.top(), bottom, native.origin.y());
    };
    // Logical pixels from a physical span.
    const auto logical = [&](int span) { return int(std::floor(span / native.scale)); };
    const QRect& bar = m_taskbar->bar;
    const QPoint& at = m_taskbar->anchor;
    switch (m_taskbar->edge) {
    case taskbar::Edge::Bottom: {
        const int edge = bar.top() - kTaskbarGap; // the window's bottom
        setRoom(logical(native.toNative(edge, native.origin.y()) - native.work.y()) - kTaskbarGap);
        return {alongX(at.x() - width / 2), alongY(edge - height)};
    }
    case taskbar::Edge::Top: {
        const int edge = bar.bottom() + 1 + kTaskbarGap; // the window's top
        setRoom(logical(bottom - native.toNative(edge, native.origin.y())) - kTaskbarGap);
        return {alongX(at.x() - width / 2), alongY(edge)};
    }
    case taskbar::Edge::Left:
    case taskbar::Edge::Right:
        setRoom(area.height() - 2 * kTaskbarGap);
        return {alongX(m_taskbar->edge == taskbar::Edge::Left ? bar.right() + 1 + kTaskbarGap : bar.left() - kTaskbarGap - width),
            alongY(at.y() - kHeaderHeight / 2)};
    }
    return m_window->position();
}

int Placement::roomRight() const
{
    QScreen* screen = m_window ? m_window->screen() : nullptr;
    if (!screen)
        return 0;
    const NativeArea native = nativeArea(screen);
    const int width = m_anchorWidth > 0 ? std::min(m_anchorWidth, m_window->width()) : m_window->width();
    const int right = native.toNative(m_window->x() + width, native.origin.x());
    return int(std::floor((native.work.x() + native.work.width() - right) / native.scale));
}

void Placement::refit()
{
    if (!m_window || m_moving || m_glide.state() == QAbstractAnimation::Running)
        return;
    QScreen* screen = m_window->screen();
    if (!screen)
        return;
    if (m_taskbar && m_taskbarScreen) {
        m_window->setPosition(settleByTaskbar(m_taskbarScreen));
    } else if (m_box) {
        m_window->setPosition(settleBy(screen));
    } else if (m_dropped) {
        const QRect area = screen->availableGeometry();
        const int width = m_window->width();
        const NativeArea native = nativeArea(screen);
        int x = std::clamp(m_left, area.left(), std::max(area.left(), area.x() + area.width() - width));
        x = native.keepInside(x, width, native.work.left(), native.work.x() + native.work.width(), native.origin.x());
        m_window->setX(x);
    } else {
        m_window->setPosition(settleOn(screen));
    }
}

void Placement::moveHome()
{
    if (!m_window || m_moving || m_box || m_taskbar || m_dropped) // by a box: no spot to go back to
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
    if (m_box || m_taskbar || m_dropped) { // by a box: stays where it was let go, its spot as it was
        m_box.reset();
        setTaskbar({});
        m_dropped = true;
        m_left = m_window->x();
        return;
    }
    const QRect frame = m_window->geometry();
    QScreen* screen = QGuiApplication::screenAt(frame.center());
    if (!screen)
        screen = m_window->screen();
    const QRect area = screen ? screen->availableGeometry() : QRect();
    if (area.isEmpty())
        return;
    const int anchorWidth = m_anchorWidth > 0 ? std::min(m_anchorWidth, frame.width()) : frame.width();
    QPointF anchor((frame.x() + anchorWidth / 2.0 - area.x()) / area.width(),
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
    state.setValue(m_group + u"/CenterX"_s, m_anchor.x());
    state.setValue(m_group + u"/Top"_s, m_anchor.y());
}

} // namespace ws
