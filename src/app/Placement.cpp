#include "Placement.h"

#include "BoxSide.h"
#include "DragBounds.h"
#include "SystemTheme.h"
#include "WindowAvoidance.h"

#include <QEasingCurve>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QWindow>

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

const QPointF kHome {0.5, 0.2}; // centred, a fifth of the way down
constexpr int kSnapDistance = 12; // logical pixels, where a dropped window settles home
constexpr int kGlideMs = 220;
constexpr int kBoxGap = 4; // logical pixels, between a box and the window attached to it
constexpr int kCaretGap = 12; // ... and another program's text caret: the line typed in stays clear
// Logical pixels between the taskbar and a window over it, and that window
// and the work area's edges: Windows 11's Start menu and search keep 12.
constexpr int kTaskbarGap = 12;
// The launcher's search box (SearchBar.qml): by a taskbar on the left or
// right, it is level with the button.

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

// A held window can span monitors. Restrict only the outer desktop bounds;
// fitting it to the pointer's monitor here would make it jump at the seam.
void shapeMove(RECT& rect, int width, int room, qreal fallbackScale)
{
    POINT cursor {};
    ::GetCursorPos(&cursor);
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(monitor, &info))
        return;
    const auto workArea = [](const RECT& area) {
        return QRect(area.left, area.top, area.right - area.left, area.bottom - area.top);
    };
    QList<QRect> areas {workArea(info.rcWork)};
    for (QScreen* screen : QGuiApplication::screens()) {
        const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        MONITORINFO other {sizeof other};
        if (native && native->handle() != monitor && ::GetMonitorInfoW(native->handle(), &other))
            areas.append(workArea(other.rcWork));
    }
    // Sizes as they will be on that monitor once the window is over there.
    const qreal scale = scaleOf(monitor, fallbackScale);
    width = qRound(width * scale);
    room = qRound(room * scale);
    const QPoint at = dragbounds::constrain(QPoint(int(rect.left), int(rect.top)), {width, room}, areas);
    ::OffsetRect(&rect, at.x() - rect.left, at.y() - rect.top);
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

// Native bounds in Qt's pixels on that screen, outwards; empty: empty.
QRect logicalOf(const NativeArea& native, const QRect& r)
{
    if (r.isEmpty())
        return {};
    const auto down = [&](int value, int origin) { return origin + int(std::floor((value - origin) / native.scale)); };
    const auto up = [&](int value, int origin) { return origin + int(std::ceil((value - origin) / native.scale)); };
    return QRect(QPoint(down(r.x(), native.origin.x()), down(r.y(), native.origin.y())),
        QPoint(up(r.x() + r.width(), native.origin.x()) - 1, up(r.y() + r.height(), native.origin.y()) - 1));
}

} // namespace

Placement::Placement(QString stateFile, QString group, QObject* parent)
    : QObject(parent)
    , m_stateFile(std::move(stateFile))
    , m_group(std::move(group))
    , m_anchor(kHome)
{
    m_displaySettle.setSingleShot(true);
    m_displaySettle.setInterval(0);
    connect(&m_displaySettle, &QTimer::timeout, this, &Placement::restoreDisplayGeometry);
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

    {
        bool okX = false;
        bool okY = false;
        const double x = state.value(u"Panel/CenterX"_s).toDouble(&okX);
        const double y = state.value(u"Panel/Top"_s).toDouble(&okY);
        if (okX && okY && x >= 0 && x <= 1 && y >= 0 && y <= 1)
            m_panelAnchor = {x, y};
        const int w = state.value(u"Panel/Width"_s).toInt();
        const int h = state.value(u"Panel/Height"_s).toInt();
        if (w >= 400 && h >= 300 && w <= 20000 && h <= 20000)
            m_panelSize = {w, h};
    }

    m_glide.setDuration(kGlideMs);
    m_glide.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_glide, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        if (m_window)
            moveWindow(value.toPoint());
    });

    connect(&m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { stepSlide(value.toInt()); });
    connect(&m_slide, &QVariantAnimation::finished, this, [this] {
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
            self->m_resizeStart = self->m_window->size();
            self->m_grabbed = false;
            self->setMoving(true);
            break;
        case WM_SIZING:
            if (self->m_windowSizingEnabled) {
                if (!self->m_windowSizing) {
                    self->m_windowSizing = true;
                    emit self->windowSizingChanged();
                }
                // Constrain the proposed native rectangle, before Windows applies it.
                // Qt's minimum/maximum size handles the other sizing limits.
                RECT& rect = *reinterpret_cast<RECT*>(lParam);
                MONITORINFO info {sizeof info};
                if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
                    rect.left = std::max(rect.left, info.rcWork.left);
                    rect.top = std::max(rect.top, info.rcWork.top);
                    rect.right = std::min(rect.right, info.rcWork.right);
                    rect.bottom = std::min(rect.bottom, info.rcWork.bottom);
                }
                return TRUE;
            }
            break;
        case WM_NCLBUTTONDBLCLK:
            if (self->m_windowSizingEnabled && wParam >= HTLEFT && wParam <= HTBOTTOMRIGHT) {
                emit self->sizeResetRequested();
                return 0;
            }
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
        // Its edge held (beginSizing): the pointer and the button are ours
        // until it is let go; Qt hears of none of it.
        case WM_MOUSEMOVE:
            if (self->m_sizing) {
                self->stepSizing();
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (self->m_sizing) {
                self->endSizing(false);
                return 0;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (self->m_sizing) {
                if (wParam == VK_ESCAPE)
                    self->endSizing(true);
                return 0;
            }
            break;
        case WM_CANCELMODE:
            if (self->m_sizing)
                self->endSizing(true);
            break;
        case WM_CAPTURECHANGED: // taken by someone else (endSizing gives it up after clearing m_sizing)
            if (self->m_sizing)
                self->endSizing(false);
            break;
        case WM_EXITSIZEMOVE:
            if (self->m_windowSizing) {
                const QSize size = self->m_window->size();
                if (size != self->m_resizeStart) {
                    // A deliberate resize keeps its opposite edge fixed, including
                    // windows attached to a caret. Automatic fitting is never saved.
                    self->m_box.reset();
                    self->m_avoid.reset();
                    self->setTaskbar({});
                    self->m_dropped = true;
                    self->m_left = self->m_window->x();
                    if (auto* screen = self->m_window->screen())
                        self->setRoom(screen->availableGeometry().bottom() + 1 - self->m_window->y());
                    emit self->windowResized(self->m_resizeStart, size);
                }
                self->m_windowSizing = false;
                emit self->windowSizingChanged();
                self->setMoving(false);
                self->m_displaySettle.stop();
                self->m_displayRestorePending = false;
                break;
            }
            self->setMoving(false);
            // A completed user move/resize owns its final geometry, including
            // a drag across DPI boundaries. Do not snap it back to the old
            // anchor through a display change deferred while it was held.
            self->m_displaySettle.stop();
            self->m_displayRestorePending = false;
            // Qt updates the window's screen on this message too, after us.
            QMetaObject::invokeMethod(self, self->m_panel ? &Placement::savePanel : &Placement::rememberSpot,
                Qt::QueuedConnection);
            break;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            self->scheduleDisplayRestore(); // after Qt has processed the native change
            break;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA)
                self->scheduleDisplayRestore();
            break;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
                self->scheduleDisplayRestore();
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
    window->installEventFilter(this);
    connect(window, &QWindow::screenChanged, this, &Placement::watchScreen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { scheduleDisplayRestore(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { scheduleDisplayRestore(); });
    watchScreen(window->screen());
    ::SetWindowSubclass(reinterpret_cast<HWND>(m_hwnd), &Hook::proc, 0, reinterpret_cast<DWORD_PTR>(this));
    // Wider (the clipboard's preview), or taller over a box: kept on the
    // screen, and over a box its bottom stays there.
    connect(window, &QWindow::widthChanged, this, &Placement::refit);
    connect(window, &QWindow::heightChanged, this, [this] {
        if ((m_box && m_above) || m_taskbar || m_avoid)
            refit();
    });
}

void Placement::setContentSize(QSize size)
{
    if (size == m_contentSize)
        return;
    m_contentSize = size;
    emit contentSizeChanged();
}

bool Placement::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::Show))
        scheduleDisplayRestore();
    return QObject::eventFilter(watched, event);
}

void Placement::watchScreen(QScreen* screen)
{
    for (const auto& connection : std::as_const(m_screenConnections))
        disconnect(connection);
    m_screenConnections.clear();
    if (screen) {
        m_screenConnections = {
            connect(screen, &QScreen::geometryChanged, this, &Placement::scheduleDisplayRestore),
            connect(screen, &QScreen::availableGeometryChanged, this, &Placement::scheduleDisplayRestore),
            connect(screen, &QScreen::logicalDotsPerInchChanged, this, &Placement::scheduleDisplayRestore)};
    }
    scheduleDisplayRestore();
}

void Placement::scheduleDisplayRestore()
{
    m_displayRestorePending = true;
    m_displaySettle.start();
}

void Placement::restoreDisplayGeometry()
{
    if (!m_window || m_moving || m_sizing)
        return;
    QScreen* screen = m_window->screen();
    if (!screen)
        return;
    m_displayRestorePending = false;
    m_glide.stop();
    settleSlide();
    if (m_panel) {
        // Only user sizing writes m_panelSize. A smaller temporary work area
        // during reconnect must never become the next opening's preference.
        m_window->setGeometry(panelRect(screen));
        return;
    }
    // Over an Opus file list (it comes here as it is shown too): narrowed to
    // the list again, not back to the user's width (GitHub #82).
    if (m_avoid && m_avoidInside)
        settleAvoiding(screen, true);
    else
        fitWidth(screen);
    if (!m_contentSize.isEmpty())
        m_window->resize(m_contentSize);
    refit();
}

void Placement::placeOn(QScreen* screen)
{
    avoidNative({}, screen);
}

void Placement::avoidNative(const QRect& window, QScreen* screen, bool inside, const QRect& lists, const QRect& frame,
    const QRect& orInto)
{
    m_glide.stop();
    m_avoid = window.isEmpty() ? std::nullopt : std::optional {window};
    m_avoidInside = inside;
    m_avoidLists = lists;
    m_avoidFrame = frame;
    m_box.reset();
    setAbove(false);
    setTaskbar({});
    m_dropped = false;
    if (m_window && screen) {
        fitWidth(screen);
        if (m_avoid && !inside && !orInto.isEmpty()) {
            // As tall as settleAvoiding has it, from home as there.
            const NativeArea native = nativeArea(screen);
            const int height = std::max(m_roomNeeded, m_roomWanted > 0 ? m_roomWanted : m_window->height());
            if (!windowavoidance::clearOf(screen->availableGeometry(), logicalOf(native, window), {widthNow(), height},
                    settleOn(screen, false))) {
                m_avoid = orInto;
                m_avoidInside = true;
                m_avoidLists = {};
            }
        }
        moveWindow(m_avoid ? settleAvoiding(screen, true) : settleOn(screen));
    }
}

void Placement::attach(const QRect& box, QScreen* screen)
{
    attachBy(box, screen, kBoxGap);
}

void Placement::placeDisplay(const QRect& geometry, int room, bool above)
{
    if (!m_window || geometry.isEmpty()) return;
    m_glide.stop();
    m_box.reset();
    m_avoid.reset();
    setTaskbar({});
    m_dropped = true;
    m_left = geometry.x();
    setAbove(above);
    setRoom(room);
    m_place = geometry.topLeft();
    m_window->setGeometry(geometry);
}

void Placement::adoptDisplayGeometry(const QRect& geometry)
{
    if (!m_window || geometry.isEmpty()) return;
    m_moveStart = m_window->position();
    m_window->setGeometry(geometry);
    m_place = geometry.topLeft();
    rememberSpot();
}

void Placement::attachBy(const QRect& box, QScreen* screen, int gap)
{
    m_glide.stop();
    m_avoid.reset();
    m_box = box;
    m_boxGap = gap;
    setTaskbar({});
    m_dropped = false;
    m_left = box.x();
    if (!screen)
        screen = QGuiApplication::screenAt(box.center());
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
    const QRect logical(QPoint(left, top), QPoint(std::max(left, up(box.x() + box.width(), origin.x()) - 1),
        std::max(top, up(box.y() + box.height(), origin.y()) - 1)));
    // On that screen, as converted: in a gap between monitors' logical
    // areas (mixed scales) screenAt would find none, or another one.
    attachBy(logical, screen, kCaretGap); // further off it than off a box of ours
}

void Placement::attachTaskbar(const taskbar::Spot& spot)
{
    m_glide.stop();
    m_avoid.reset();
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
    setAbove(false);
    m_dropped = false;
    m_taskbarScreen = screen;
    m_taskbarSpot = spot;
    setTaskbar(TaskbarBox {logicalBar, spot.edge,
        {down(spot.anchor.x, origin.x()), down(spot.anchor.y, origin.y())}, bar});
    fitWidth(screen);
    moveWindow(settleByTaskbar(screen));
}

QRect Placement::panelRect(QScreen* screen) const
{
    const QRect area = screen->availableGeometry();
    // Its own size, else most of a small screen and a comfortable part of a large one.
    QSize size = m_panelSize.isEmpty() ? QSize(1100, 700) : m_panelSize;
    size = QSize(std::clamp(size.width(), std::min(400, area.width()), std::max(400, area.width() - 40)),
        std::clamp(size.height(), std::min(300, area.height()), std::max(300, area.height() - 40)));
    int x = qRound(area.x() + m_panelAnchor.x() * area.width() - size.width() / 2.0);
    int y = qRound(area.y() + m_panelAnchor.y() * area.height());
    x = std::clamp(x, area.left(), std::max(area.left(), area.x() + area.width() - size.width()));
    y = std::clamp(y, area.top(), std::max(area.top(), area.y() + area.height() - size.height()));
    // On whole physical pixels inside the work area, as settleOn keeps the launcher.
    const NativeArea native = nativeArea(screen);
    x = native.keepInside(x, size.width(), native.work.left(), native.work.x() + native.work.width(), native.origin.x());
    y = native.keepInside(y, size.height(), native.work.top(), native.work.y() + native.work.height(), native.origin.y());
    return {QPoint(x, y), size};
}

void Placement::enterPanel(QScreen* screen)
{
    if (!m_window || m_panel)
        return;
    m_glide.stop();
    m_avoid.reset();
    settleSlide();
    m_box.reset();
    setAbove(false);
    m_dropped = false;
    setTaskbar({}); // the spot stays in m_taskbarSpot for leavePanel's caller
    m_panel = true;
    const QRect rect = panelRect(screen ? screen : m_window->screen());
    m_place = rect.topLeft();
    m_window->setGeometry(rect);
}

void Placement::leavePanel()
{
    if (!m_panel)
        return;
    m_panel = false;
}

void Placement::savePanel()
{
    if (!m_window || !m_panel)
        return;
    const QRect frame = m_window->geometry();
    QScreen* screen = QGuiApplication::screenAt(frame.center());
    if (!screen)
        screen = m_window->screen();
    const QRect area = screen ? screen->availableGeometry() : QRect();
    if (area.isEmpty() || frame.width() < 400 || frame.height() < 300)
        return;
    m_panelSize = frame.size();
    m_panelAnchor = {std::clamp((frame.x() + frame.width() / 2.0 - area.x()) / area.width(), 0.0, 1.0),
        std::clamp(qreal(frame.y() - area.y()) / area.height(), 0.0, 1.0)};
    QSettings state(m_stateFile, QSettings::IniFormat);
    state.setValue(u"Panel/CenterX"_s, m_panelAnchor.x());
    state.setValue(u"Panel/Top"_s, m_panelAnchor.y());
    state.setValue(u"Panel/Width"_s, m_panelSize.width());
    state.setValue(u"Panel/Height"_s, m_panelSize.height());
}

void Placement::setRoomNeeded(int height)
{
    if (m_roomNeeded == height)
        return;
    m_roomNeeded = height;
    emit roomNeededChanged();
    if (m_avoid)
        refit();
}

void Placement::setRoomWanted(int height)
{
    if (m_roomWanted == height)
        return;
    m_roomWanted = height;
    emit roomWantedChanged();
    if (m_avoid)
        refit();
}

void Placement::setRoomStep(int height)
{
    if (m_roomStep == height)
        return;
    m_roomStep = height;
    emit roomStepChanged();
}

void Placement::resized()
{
    if (m_window && m_window->isVisible())
        QTimer::singleShot(0, this, &Placement::refit); // once QML has given it its new height
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

void Placement::setAbove(bool above)
{
    if (m_above == above)
        return;
    m_above = above;
    emit aboveChanged();
}

// The anchor's spot on the screen's work area, moved as far as needed to keep
// the window on it with room below for the fewest rows. More rows than fit
// there are not shown (room), so a long list does not push the window up.
QPoint Placement::settleOn(QScreen* screen, bool updateRoom)
{
    const QRect area = screen->availableGeometry();
    const int width = widthNow();
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
    if (updateRoom)
        setRoom(int(std::floor((bottom - native.toNative(y, native.origin.y())) / native.scale)));
    return {x, y};
}

// The same monitor and the same width, without persisting an automatic move.
QPoint Placement::settleAvoiding(QScreen* screen, bool fitList)
{
    const QPoint home = settleOn(screen, false);
    const NativeArea native = nativeArea(screen);
    const auto logical = [&](const QRect& r) { return logicalOf(native, r); };
    const QRect obstacle = logical(*m_avoid);
    const QRect area = screen->availableGeometry();
    int height = std::max(m_roomNeeded, m_roomWanted > 0 ? m_roomWanted : m_window->height());
    QPoint at;
    if (m_avoidInside) {
        // Over the list (GitHub #82). As it opens, as narrow as the list is
        // (the user's width the most, the least width the launcher has the
        // least: Main.qml sizes the window by boxWidth); later, as it is then.
        const bool narrow = fitList && m_defaultWidth > 0 && !m_panel;
        const int width = narrow ? std::min(460, widthOn(screen)) : widthNow();
        const QRect spot = windowavoidance::inList(area, obstacle, logical(m_avoidLists), logical(m_avoidFrame),
            {width, height}, {narrow ? std::min(320, width) : width, m_roomNeeded});
        if (narrow && spot.width() != m_boxWidth) {
            m_boxWidth = spot.width();
            emit boxWidthChanged();
        }
        at = spot.topLeft();
        height = spot.height();
        // Keep the panel's bottom at the file list's bottom as the result
        // count changes; room still describes the full result budget.
        if (!fitList)
            at.ry() += std::max(0, spot.height() - m_window->height());
    } else {
        at = windowavoidance::place(area, obstacle, {widthNow(), height}, home);
    }
    at.setX(native.keepInside(at.x(), widthNow(), native.work.x(), native.work.x() + native.work.width(), native.origin.x()));
    at.setY(native.keepInside(at.y(), std::min(height, area.height()), native.work.y(), native.work.y() + native.work.height(),
        native.origin.y()));
    const int room = int(std::floor((native.work.y() + native.work.height() - native.toNative(at.y(), native.origin.y())) / native.scale));
    // Over a list, its rows end above the list's bottom edge, not the work area's.
    setRoom(m_avoidInside ? std::min(room, height) : room);
    return at;
}

// Under the box, or above it where more whole rows fit (GitHub #38).
QPoint Placement::settleBy(QScreen* screen)
{
    const QRect area = screen->availableGeometry();
    const NativeArea native = nativeArea(screen);
    const int right = native.work.x() + native.work.width();
    const int bottom = native.work.y() + native.work.height();
    const int width = m_window->width();
    int x = std::clamp(m_left, area.left(), std::max(area.left(), area.x() + area.width() - width));
    x = native.keepInside(x, width, native.work.left(), right, native.origin.x());

    const int under = m_box->y() + m_box->height() + m_boxGap;
    const int over = m_box->y() - m_boxGap;
    const int roomUnder = int(std::floor((bottom - native.toNative(under, native.origin.y())) / native.scale));
    const int roomOver = int(std::floor((native.toNative(over, native.origin.y()) - native.work.y()) / native.scale));
    setAbove(boxside::above(roomUnder, roomOver, m_roomNeeded, m_roomWanted, m_roomStep));
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
    const int width = widthNow();
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
            alongY(at.y() - SystemTheme::headerHeight() / 2)};
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
    if (!m_window || m_moving || m_sizing || m_panel || m_glide.state() == QAbstractAnimation::Running)
        return;
    QScreen* screen = m_window->screen();
    if (!screen)
        return;
    if (m_taskbar && m_taskbarScreen) {
        moveWindow(settleByTaskbar(m_taskbarScreen));
    } else if (m_box) {
        moveWindow(settleBy(screen));
    } else if (m_avoid) {
        moveWindow(settleAvoiding(screen));
    } else if (m_dropped) {
        const QRect area = screen->availableGeometry();
        const int width = widthNow();
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
    if (!m_window || m_moving || m_box || m_avoid || m_taskbar || m_dropped || m_panel) // temporary spot: no home to change
        return;
    m_anchor = kHome;
    save();
    QScreen* screen = m_window->screen();
    if (!screen)
        return;
    glideTo(settleOn(screen));
}

void Placement::centerOnScreen()
{
    if (!m_window || m_moving || m_sizing || m_windowSizing)
        return;
    QScreen* screen = m_window->screen();
    if (!screen)
        return;
    const QRect area = screen->availableGeometry();
    if (area.isEmpty())
        return;
    m_glide.stop();
    settleSlide();
    m_box.reset();
    m_avoid.reset();
    setTaskbar({});
    m_taskbarSpot.reset();
    m_taskbarScreen.clear();
    m_dropped = false;
    setAbove(false);
    const QSize size = m_window->size();
    QPoint target(area.x() + std::max(0, (area.width() - size.width()) / 2),
        area.y() + std::max(0, (area.height() - size.height()) / 2));
    const NativeArea native = nativeArea(screen);
    target.setX(native.keepInside(target.x(), size.width(), native.work.left(), native.work.x() + native.work.width(), native.origin.x()));
    target.setY(native.keepInside(target.y(), size.height(), native.work.top(), native.work.y() + native.work.height(), native.origin.y()));
    moveWindow(target);
    if (m_panel) {
        savePanel();
    } else {
        m_anchor = {0.5, qreal(target.y() - area.y()) / area.height()};
        save();
        setRoom(area.y() + area.height() - target.y());
    }
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
    if (m_panel) {
        savePanel();
        return;
    }
    if (!m_window || m_window->position() == m_moveStart) // Esc, or not moved at all
        return;
    if (m_box || m_avoid || m_taskbar || m_dropped) { // temporary spot: moving it does not change home
        // `above` too: the rows do not turn over in the hand that moved them.
        m_box.reset();
        m_avoid.reset();
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

// `logical` pixels off its place toward the taskbar it opens over (else down),
// never onto the taskbar: by it, no further than the gap between them. A
// see-through taskbar (TranslucentTB's blur) showed the window behind it, and
// kept the last of it in its blur once the window was hidden.
QPoint Placement::nearOffset(int logical) const
{
    const int full = qRound(logical * m_window->devicePixelRatio());
    if (!m_taskbar || !m_hwnd)
        return {0, full};
    RECT window {};
    ::GetWindowRect(reinterpret_cast<HWND>(m_hwnd), &window);
    ::OffsetRect(&window, m_rest.x() - window.left, m_rest.y() - window.top);
    const RECT& bar = m_taskbar->native;
    const auto upTo = [full](LONG gap) { return std::clamp(int(gap), 0, full); };
    switch (m_taskbar->edge) {
    case taskbar::Edge::Bottom:
        return {0, upTo(bar.top - window.bottom)};
    case taskbar::Edge::Top:
        return {0, -upTo(window.top - bar.bottom)};
    case taskbar::Edge::Left:
        return {-upTo(window.left - bar.right), 0};
    case taskbar::Edge::Right:
        return {upTo(bar.left - window.right), 0};
    }
    return {};
}

// Which way the taskbar it opens over is (else down), one logical pixel.
QPointF Placement::towardTaskbar() const
{
    if (!m_taskbar)
        return {0, 1};
    switch (m_taskbar->edge) {
    case taskbar::Edge::Bottom:
        return {0, 1};
    case taskbar::Edge::Top:
        return {0, -1};
    case taskbar::Edge::Left:
        return {-1, 0};
    case taskbar::Edge::Right:
        return {1, 0};
    }
    return {0, 1};
}

namespace {

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
    Slide slide;
    slide.from = m_offset;
    slide.opacityFrom = m_opacity;
    slide.shiftFrom = m_contentShift;
    if (!animationsOn() || !layered()) { // straight there
        m_slideSpec = slide;
        stepSlide(0);
        if (done)
            done();
        return;
    }
    // Faded in, coming a little from the taskbar's side (rising, by one at the
    // bottom), all of it over the taskbar.
    if (!wasSliding) {
        slide.from = nearOffset(16);
        slide.opacityFrom = 0;
        slide.shiftFrom = towardTaskbar() * 8;
    }
    slide.moveMs = 200;
    slide.fadeMs = 120;
    slide.shiftMs = 240;
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
    if (!animationsOn() || !layered()) {
        finish();
        return;
    }
    // Faded out where it is, briefly: closing needs no more show than that
    // (it sank toward the taskbar as well once; too much motion, the user
    // said). Caught while still coming in: from where that got to.
    Slide slide;
    slide.from = slide.to = m_offset;
    slide.opacityFrom = m_opacity;
    slide.shiftFrom = slide.shiftTo = m_contentShift;
    slide.opacityTo = 0;
    slide.fadeMs = 120;
    startSlide(std::move(slide), std::move(finish));
}

void Placement::settleSlide()
{
    if (!sliding())
        return;
    m_slide.stop();
    stepSlide(std::max({m_slideSpec.moveMs, m_slideSpec.fadeMs, m_slideSpec.shiftMs}));
    if (const std::function<void()> done = std::exchange(m_slideDone, {}))
        done();
}

void Placement::startSlide(Slide slide, std::function<void()> done)
{
    m_slideSpec = std::move(slide);
    m_slideDone = std::move(done);
    const int total = std::max({m_slideSpec.moveMs, m_slideSpec.fadeMs, m_slideSpec.shiftMs, 1});
    {
        // Stopped where the last one got to (its end, mostly): set up, it
        // reports the value there, and the window would be put that far into
        // this one for a moment (faded out as it starts closing).
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
    // On whole device pixels, as the window moves. Between them text and the
    // unsmoothed icons round to the grid each their own way: as the shift
    // died away under a pixel, the rows' icons moved up by one, row after
    // row, and the search bar at its very end, the window still by then.
    const qreal dpr = m_window ? m_window->devicePixelRatio() : 1.0;
    const QPointF exact = s.shiftFrom + (s.shiftTo - s.shiftFrom) * progress(s.shiftMs, s.shiftCurve);
    const QPointF shift(qRound(exact.x() * dpr) / dpr, qRound(exact.y() * dpr) / dpr);
    if (shift != m_contentShift) {
        m_contentShift = shift;
        emit contentShiftChanged();
    }
}

void Placement::rememberWidth(int defaultWidth, int minimumWidth)
{
    m_defaultWidth = defaultWidth;
    m_minWidth = minimumWidth;
    const QSettings state(m_stateFile, QSettings::IniFormat);
    const int saved = state.value(m_group + u"/Width"_s).toInt();
    m_wantedWidth = saved >= minimumWidth && saved <= 20000 ? saved : defaultWidth;
    m_boxWidth = widthOn(m_window ? m_window->screen() : QGuiApplication::primaryScreen());
    emit boxWidthChanged();
}

int Placement::widthNow() const
{
    return m_defaultWidth > 0 && !m_panel && m_boxWidth > 0 ? m_boxWidth : m_window->width();
}

int Placement::widthOn(QScreen* screen) const
{
    if (!screen)
        return m_wantedWidth;
    // Off the work area's edges by the gap the taskbar's windows keep.
    const int most = screen->availableGeometry().width() - 2 * kTaskbarGap;
    return std::max(std::min(m_minWidth, most), std::min(m_wantedWidth, most));
}

void Placement::fitWidth(QScreen* screen)
{
    if (m_defaultWidth <= 0 || m_panel || !screen)
        return;
    const int width = widthOn(screen);
    if (width == m_boxWidth)
        return;
    m_boxWidth = width;
    emit boxWidthChanged(); // QML sizes the window (Main.qml); placed by it from here on
}

void Placement::beginSizing(bool left)
{
    if (!m_window || !m_hwnd || m_panel || m_defaultWidth <= 0 || m_moving)
        return;
    m_glide.stop();
    settleSlide(); // grabbed on its way in: where it was going
    const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    ::GetWindowRect(hwnd, &m_sizeStart);
    ::GetCursorPos(&m_sizeFrom);
    m_sizeLeft = left;
    m_sizing = true;
    ::SetCapture(hwnd); // the move and the release, wherever the pointer goes
    emit sizingChanged();
}

void Placement::stepSizing()
{
    POINT cursor {};
    ::GetCursorPos(&cursor);
    // Both edges move: the one held with the pointer, the other as far the other way.
    const LONG moved = m_sizeLeft ? m_sizeFrom.x - cursor.x : cursor.x - m_sizeFrom.x;
    applyWidth(int(m_sizeStart.right - m_sizeStart.left + 2 * moved), (m_sizeStart.left + m_sizeStart.right) / 2.0);
}

void Placement::endSizing(bool cancel)
{
    if (!m_sizing)
        return;
    m_sizing = false; // before giving up the capture: WM_CAPTURECHANGED comes at once
    const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    if (::GetCapture() == hwnd)
        ::ReleaseCapture();
    if (cancel) {
        const RECT& r = m_sizeStart;
        ::SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
            SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    } else {
        keepWidth();
    }
    if (m_displayRestorePending)
        scheduleDisplayRestore();
    emit sizingChanged();
}

void Placement::applyWidth(int width, qreal centre)
{
    QScreen* screen = m_window ? m_window->screen() : nullptr;
    if (!screen || !m_hwnd)
        return;
    const NativeArea native = nativeArea(screen);
    const int most = native.work.width() - 2 * qRound(kTaskbarGap * native.scale);
    const int least = std::min(qRound(m_minWidth * native.scale), most);
    width = std::clamp(width, least, std::max(least, most));
    const int left = native.work.x();
    const int right = native.work.x() + native.work.width();
    const int x = std::clamp(int(std::lround(centre - width / 2.0)), left, std::max(left, right - width));
    const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    RECT now {};
    ::GetWindowRect(hwnd, &now);
    if (now.left == x && now.right - now.left == width)
        return;
    ::SetWindowPos(hwnd, nullptr, x, now.top, width, now.bottom - now.top,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

void Placement::keepWidth()
{
    QScreen* screen = m_window ? m_window->screen() : nullptr;
    if (!screen || !m_hwnd)
        return;
    RECT r {};
    ::GetWindowRect(reinterpret_cast<HWND>(m_hwnd), &r);
    const NativeArea native = nativeArea(screen);
    // In Qt's terms (from the screen's origin, scaled; see NativeArea).
    const int width = qRound((r.right - r.left) / native.scale);
    if (m_dropped)
        m_left = native.origin.x() + qRound((r.left - native.origin.x()) / native.scale);
    if (width == m_boxWidth)
        return;
    // Narrowed by the work area, it was not its user's doing: the wish stays.
    if (width != widthOn(screen) || m_wantedWidth <= width) {
        m_wantedWidth = width;
        QSettings state(m_stateFile, QSettings::IniFormat);
        if (m_wantedWidth == m_defaultWidth)
            state.remove(m_group + u"/Width"_s);
        else
            state.setValue(m_group + u"/Width"_s, m_wantedWidth);
    }
    m_boxWidth = width;
    emit boxWidthChanged();
}

void Placement::resetWidth()
{
    if (m_defaultWidth <= 0 || m_sizing)
        return;
    const bool changed = m_wantedWidth != m_defaultWidth;
    m_wantedWidth = m_defaultWidth;
    QSettings(m_stateFile, QSettings::IniFormat).remove(m_group + u"/Width"_s);
    QScreen* screen = m_window ? m_window->screen() : nullptr;
    if (m_window && m_window->isVisible() && !m_panel && screen) {
        // One move and resize, about its centre, as dragging the edge does.
        RECT r {};
        ::GetWindowRect(reinterpret_cast<HWND>(m_hwnd), &r);
        applyWidth(qRound(widthOn(screen) * nativeArea(screen).scale), (r.left + r.right) / 2.0);
        keepWidth();
    }
    fitWidth(screen);
    if (changed)
        emit widthReset();
}

void Placement::save() const
{
    QSettings state(m_stateFile, QSettings::IniFormat);
    state.setValue(m_group + u"/CenterX"_s, m_anchor.x());
    state.setValue(m_group + u"/Top"_s, m_anchor.y());
}

} // namespace ws
