#include "Placement.h"

#include <QEasingCurve>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QWindow>

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <vector>

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
            moveWindow(value.toPoint());
    });

    connect(&m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { stepSlide(value.toInt()); });
    connect(&m_slide, &QVariantAnimation::finished, this, [this] {
        m_under = 0;
        if (const std::function<void()> done = std::exchange(m_slideDone, {}))
            done();
    });
}

struct Placement::Hook {
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<Placement*>(data);
        switch (message) {
        case WM_ENTERSIZEMOVE:
            self->m_glide.stop();
            self->settleSlide(); // grabbed on its way in: where it was going
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
        case WM_WINDOWPOSCHANGING: {
            // Sliding behind the taskbar: whatever would put it in front
            // (activating it does), just under the taskbar instead.
            auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
            const HWND under = reinterpret_cast<HWND>(self->m_under);
            if (under && !(pos->flags & SWP_NOZORDER) && pos->hwndInsertAfter != under && ::IsWindow(under))
                pos->hwndInsertAfter = under;
            break;
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
        moveWindow(settleOn(screen));
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
        moveWindow(settleBy(screen));
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
        {down(spot.anchor.x, origin.x()), down(spot.anchor.y, origin.y())}, bar});
    moveWindow(settleByTaskbar(screen));
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
        moveWindow(settleByTaskbar(m_taskbarScreen));
    } else if (m_box) {
        moveWindow(settleBy(screen));
    } else if (m_dropped) {
        const QRect area = screen->availableGeometry();
        const int width = m_window->width();
        const NativeArea native = nativeArea(screen);
        int x = std::clamp(m_left, area.left(), std::max(area.left(), area.x() + area.width() - width));
        x = native.keepInside(x, width, native.work.left(), native.work.x() + native.work.width(), native.origin.x());
        moveWindow({x, sliding() ? m_place.y() : m_window->y()});
    } else {
        moveWindow(settleOn(screen));
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
    {
        const QSignalBlocker quiet(m_glide); // as a slide's (see startSlide)
        m_glide.setStartValue(m_window->position());
        m_glide.setEndValue(target);
    }
    m_glide.start();
}

bool Placement::animationsOn()
{
    BOOL on = TRUE;
    return !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
}

bool Placement::layered() const
{
    return m_hwnd && (::GetWindowLongW(reinterpret_cast<HWND>(m_hwnd), GWL_EXSTYLE) & WS_EX_LAYERED);
}

void Placement::moveWindow(const QPoint& position)
{
    if (!m_window)
        return;
    m_place = position;
    m_window->setPosition(position);
    if (!sliding())
        return;
    RECT r {};
    ::GetWindowRect(reinterpret_cast<HWND>(m_hwnd), &r);
    m_rest = {r.left, r.top};
    applyOffset();
}

void Placement::applyOffset()
{
    if (m_hwnd)
        ::SetWindowPos(reinterpret_cast<HWND>(m_hwnd), nullptr, m_rest.x() + m_offset.x(), m_rest.y() + m_offset.y(), 0,
            0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Off its place until it is behind the taskbar it opens over, all of it, as
// far as that goes without reaching onto another monitor (cut: not all the way).
QPoint Placement::hiddenOffset(bool* cut) const
{
    *cut = false;
    if (!m_taskbar || !m_hwnd)
        return {};
    RECT window {};
    ::GetWindowRect(reinterpret_cast<HWND>(m_hwnd), &window);
    ::OffsetRect(&window, m_rest.x() - window.left, m_rest.y() - window.top);
    const RECT& bar = m_taskbar->native;
    QPoint full;
    switch (m_taskbar->edge) {
    case taskbar::Edge::Bottom:
        full = {0, int(bar.top - window.top)};
        break;
    case taskbar::Edge::Top:
        full = {0, int(bar.bottom - window.bottom)};
        break;
    case taskbar::Edge::Left:
        full = {int(bar.right - window.right), 0};
        break;
    case taskbar::Edge::Right:
        full = {int(bar.left - window.left), 0};
        break;
    }
    std::vector<RECT> others;
    const HMONITOR own = ::MonitorFromRect(&bar, MONITOR_DEFAULTTONEAREST);
    for (QScreen* screen : QGuiApplication::screens()) {
        const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        MONITORINFO info {sizeof info};
        if (native && native->handle() != own && ::GetMonitorInfoW(native->handle(), &info))
            others.push_back(info.rcMonitor);
    }
    for (int step = 20; step > 0; --step) {
        const QPoint offset = full * step / 20;
        RECT moved = window;
        ::OffsetRect(&moved, offset.x(), offset.y());
        RECT overlap {};
        if (std::none_of(others.begin(), others.end(), [&](const RECT& m) { return ::IntersectRect(&overlap, &moved, &m); })) {
            *cut = step < 20;
            return offset;
        }
    }
    *cut = true;
    return {};
}

namespace {

// The taskbar window on the monitor of `bar`.
HWND taskbarWindow(const RECT& bar)
{
    const HMONITOR monitor = ::MonitorFromRect(&bar, MONITOR_DEFAULTTONEAREST);
    for (const wchar_t* name : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        for (HWND window = ::FindWindowExW(nullptr, nullptr, name, nullptr); window;
             window = ::FindWindowExW(nullptr, window, name, nullptr)) {
            if (::MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == monitor)
                return window;
        }
    }
    return nullptr;
}

// As CSS's cubic-bezier().
QEasingCurve bezier(qreal x1, qreal y1, qreal x2, qreal y2)
{
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(QPointF(x1, y1), QPointF(x2, y2), QPointF(1, 1));
    return curve;
}

// Entrances: Windows 11's own
// (learn.microsoft.com/windows/apps/design/signature-experiences/motion).
const QEasingCurve& fastIn()
{
    static const QEasingCurve curve = bezier(0, 0, 0, 1);
    return curve;
}

// Exits: under way at once, then faster and faster. Not Windows' gentle exit
// (1, 0, 1, 1), which moves a mere 4 % in its first 70 ms: the launcher
// seemed to close late (most of all when closed again as it opened).
const QEasingCurve& accelerate()
{
    static const QEasingCurve curve = bezier(0.3, 0, 1, 1);
    return curve;
}

} // namespace

void Placement::beginSlide()
{
    if (sliding())
        return; // on from where it is
    RECT r {};
    ::GetWindowRect(reinterpret_cast<HWND>(m_hwnd), &r);
    m_rest = {r.left, r.top};
    m_place = m_window->position();
    m_offset = {};
    m_opacity = 1;
    m_contentShift = {};
}

void Placement::slideIn(std::function<void()> done)
{
    if (!m_window || !m_hwnd)
        return;
    const bool wasSliding = sliding();
    beginSlide();
    m_slide.stop(); // a closing undone: its end never comes
    m_slideDone = {};
    m_under = 0;
    const bool fades = layered();
    bool cut = false;
    const QPoint hidden = hiddenOffset(&cut);
    Slide slide;
    slide.from = m_offset;
    slide.opacityFrom = m_opacity;
    slide.shiftFrom = m_contentShift;
    if (!animationsOn() || (m_taskbar ? hidden.isNull() && !fades : !fades)) { // straight there
        m_slideSpec = slide;
        stepSlide(0);
        if (done)
            done();
        return;
    }
    if (m_taskbar && !hidden.isNull()) {
        // Out from behind the taskbar, the content a little behind it.
        const QPointF away = QPointF(hidden) / std::max(1.0, std::hypot(qreal(hidden.x()), qreal(hidden.y())));
        if (!wasSliding) {
            slide.from = hidden;
            slide.opacityFrom = cut && fades ? 0 : 1;
            slide.shiftFrom = away * 24;
        }
        // As Windows' own Start menu (measured: the same curve over some 200 ms).
        slide.moveMs = 200;
        slide.fadeMs = 120;
        slide.shiftMs = 260;
        goBehindTaskbar();
    } else {
        // Faded in, rising a little.
        if (!wasSliding) {
            slide.from = QPoint(0, qRound(16 * m_window->devicePixelRatio()));
            slide.opacityFrom = 0;
            slide.shiftFrom = QPointF(0, 8);
        }
        slide.moveMs = 200;
        slide.fadeMs = 120;
        slide.shiftMs = 240;
    }
    slide.moveCurve = fastIn();
    slide.shiftCurve = fastIn();
    startSlide(std::move(slide), std::move(done));
}

void Placement::slideOut(std::function<void()> done)
{
    // Hidden by then: back to its place, as it was.
    auto finish = [this, done = std::move(done)] {
        if (done)
            done();
        m_slideSpec = Slide {};
        stepSlide(0);
    };
    if (!m_window || !m_hwnd || !m_window->isVisible()) {
        finish();
        return;
    }
    beginSlide();
    m_slide.stop();
    m_slideDone = {};
    m_under = 0;
    const bool fades = layered();
    bool cut = false;
    const QPoint hidden = hiddenOffset(&cut);
    if (!animationsOn() || (m_taskbar ? hidden.isNull() && !fades : !fades)) {
        finish();
        return;
    }
    Slide slide;
    slide.from = m_offset;
    slide.opacityFrom = m_opacity;
    slide.shiftFrom = m_contentShift;
    if (m_taskbar && !hidden.isNull()) { // down behind the taskbar
        slide.to = hidden;
        slide.opacityTo = cut && fades ? 0 : 1;
        slide.moveMs = 200;
        slide.fadeMs = 200;
        goBehindTaskbar();
    } else { // faded out, sinking a little
        slide.to = QPoint(0, qRound(8 * m_window->devicePixelRatio()));
        slide.opacityTo = 0;
        slide.moveMs = 167;
        slide.fadeMs = 150;
    }
    slide.moveCurve = accelerate();
    slide.shiftCurve = accelerate();
    slide.shiftMs = slide.moveMs;
    startSlide(std::move(slide), std::move(finish));
}

void Placement::settleSlide()
{
    if (!sliding())
        return;
    m_slide.stop();
    m_under = 0;
    stepSlide(std::max({m_slideSpec.moveMs, m_slideSpec.fadeMs, m_slideSpec.shiftMs}));
    if (const std::function<void()> done = std::exchange(m_slideDone, {}))
        done();
}

void Placement::goBehindTaskbar()
{
    const HWND bar = taskbarWindow(m_taskbar->native);
    m_under = reinterpret_cast<WId>(bar);
    if (bar)
        ::SetWindowPos(reinterpret_cast<HWND>(m_hwnd), bar, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Placement::startSlide(Slide slide, std::function<void()> done)
{
    m_slideSpec = std::move(slide);
    m_slideDone = std::move(done);
    const int total = std::max({m_slideSpec.moveMs, m_slideSpec.fadeMs, m_slideSpec.shiftMs, 1});
    {
        // Stopped where the last one got to (its end, mostly): set up, it
        // reports the value there, and the window would be put that far into
        // this one for a moment (behind the taskbar as it starts closing).
        const QSignalBlocker quiet(m_slide);
        m_slide.setStartValue(0);
        m_slide.setEndValue(total);
        m_slide.setDuration(total);
    }
    stepSlide(0);
    m_slide.start();
}

void Placement::stepSlide(int elapsed)
{
    const Slide& s = m_slideSpec;
    const auto progress = [elapsed](int ms, const QEasingCurve& curve) {
        return ms <= 0 ? 1.0 : curve.valueForProgress(std::min(1.0, qreal(elapsed) / ms));
    };
    const qreal move = progress(s.moveMs, s.moveCurve);
    const QPoint offset(qRound(s.from.x() + (s.to.x() - s.from.x()) * move),
        qRound(s.from.y() + (s.to.y() - s.from.y()) * move));
    if (offset != m_offset) {
        m_offset = offset;
        applyOffset();
    }
    const qreal fade = s.fadeMs <= 0 ? 1.0 : std::min(1.0, qreal(elapsed) / s.fadeMs);
    const qreal opacity = s.opacityFrom + (s.opacityTo - s.opacityFrom) * fade;
    if (opacity != m_opacity) {
        m_opacity = opacity;
        if (m_window && layered())
            m_window->setOpacity(opacity);
    }
    const QPointF shift = s.shiftFrom + (s.shiftTo - s.shiftFrom) * progress(s.shiftMs, s.shiftCurve);
    if (shift != m_contentShift) {
        m_contentShift = shift;
        emit contentShiftChanged();
    }
}

void Placement::save() const
{
    QSettings state(m_stateFile, QSettings::IniFormat);
    state.setValue(m_group + u"/CenterX"_s, m_anchor.x());
    state.setValue(m_group + u"/Top"_s, m_anchor.y());
}

} // namespace ws
