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
constexpr int kSnapDistance = 12; // logical pixels
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
// work area of the monitor under the pointer with `room` below its top, and
// stuck to the home spot's centre line and height when near them.
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
    const int snap = qRound(kSnapDistance * scale);

    const LONG homeX = area.left + qRound((area.right - area.left) * kHome.x() - width / 2.0);
    const LONG homeY = area.top + qRound((area.bottom - area.top) * kHome.y());
    LONG x = std::abs(rect.left - homeX) <= snap ? homeX : rect.left;
    LONG y = std::abs(rect.top - homeY) <= snap ? homeY : rect.top;
    x = std::clamp(x, area.left, std::max(area.left, area.right - width));
    y = std::clamp(y, area.top, std::max(area.top, area.bottom - room));
    ::OffsetRect(&rect, x - rect.left, y - rect.top);
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
            // so a window held by a magnet would stay held. It goes where the
            // pointer puts it instead, at the offset of the first proposal.
            RECT& rect = *reinterpret_cast<RECT*>(lParam);
            POINT cursor {};
            ::GetCursorPos(&cursor);
            if (!self->m_grabbed) {
                self->m_grab = QPoint(int(cursor.x - rect.left), int(cursor.y - rect.top));
                self->m_grabbed = true;
            }
            ::OffsetRect(&rect, cursor.x - self->m_grab.x() - rect.left, cursor.y - self->m_grab.y() - rect.top);
            shapeMove(rect, self->m_window->width(), std::max(self->m_fullHeight, self->m_window->height()),
                self->m_window->devicePixelRatio());
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
        m_window->setPosition(positionIn(screen->availableGeometry()));
}

void Placement::setFullHeight(int height)
{
    if (m_fullHeight == height)
        return;
    m_fullHeight = height;
    emit fullHeightChanged();
}

// The anchor's spot on `area`, moved as far as needed to keep the whole
// window on it with room below for every row.
QPoint Placement::positionIn(const QRect& area) const
{
    const int width = m_window->width();
    const int room = std::max(m_fullHeight, m_window->height());
    const int x = qRound(area.x() + m_anchor.x() * area.width() - width / 2.0);
    const int y = qRound(area.y() + m_anchor.y() * area.height());
    return {std::clamp(x, area.left(), std::max(area.left(), area.x() + area.width() - width)),
        std::clamp(y, area.top(), std::max(area.top(), area.y() + area.height() - room))};
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
    const QPoint target = positionIn(screen->availableGeometry());
    m_glide.stop();
    if (target == m_window->position())
        return;
    m_glide.setStartValue(m_window->position());
    m_glide.setEndValue(target);
    m_glide.start();
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
    // Stuck to home (within rounding): exactly home, the same on every monitor.
    if (std::abs(anchor.x() - kHome.x()) * area.width() <= 1.5)
        anchor.rx() = kHome.x();
    if (std::abs(anchor.y() - kHome.y()) * area.height() <= 1.5)
        anchor.ry() = kHome.y();
    m_anchor = {std::clamp(anchor.x(), 0.0, 1.0), std::clamp(anchor.y(), 0.0, 1.0)};
    save();
}

void Placement::save() const
{
    QSettings state(m_stateFile, QSettings::IniFormat);
    state.setValue(u"Launcher/CenterX"_s, m_anchor.x());
    state.setValue(u"Launcher/Top"_s, m_anchor.y());
}

} // namespace ws
