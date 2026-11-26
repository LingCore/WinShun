#include "SavedWindowSize.h"

#include "platform/WindowEffects.h"

#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QList>
#include <QPlatformSurfaceEvent>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QWindow>

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// Logical pixels: a saved size outside these was never a window's.
constexpr int kLeast = 100;
constexpr int kMost = 20000;

constexpr Qt::WindowStates kNotNormal = Qt::WindowMinimized | Qt::WindowMaximized | Qt::WindowFullScreen;

// 外观 › 记住窗口大小, and every SavedWindowSize there is (GUI thread only).
bool g_remembering = true;
QList<SavedWindowSize*> g_all;

// The window's size as Windows has it now, in Qt's logical pixels (rounded as
// Qt rounds it). Qt hears of each size in the sizing loop a moment later, so
// QWindow::size may still be the one before as the loop ends. Its client area:
// what Qt counts as the window (all of ours, framed by WindowFrame).
QSize logicalSize(HWND hwnd, const QWindow& window)
{
    RECT client {};
    ::GetClientRect(hwnd, &client);
    return (QSizeF(client.right, client.bottom) / window.devicePixelRatio()).toSize();
}

} // namespace

struct SavedWindowSize::Hook {
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<SavedWindowSize*>(data);
        switch (message) {
        case WM_ENTERSIZEMOVE:
            self->m_sizing = false;
            self->m_loopStart = self->m_window ? logicalSize(hwnd, *self->m_window) : QSize();
            break;
        case WM_SIZING: // an edge dragged; a move sends none, nor the snap at its end
            self->m_sizing = true;
            break;
        case WM_EXITSIZEMOVE:
            if (std::exchange(self->m_sizing, false) && self->m_window && !::IsZoomed(hwnd) && !::IsIconic(hwnd))
                self->sized(self->m_loopStart, logicalSize(hwnd, *self->m_window));
            break;
        case WM_NCLBUTTONDBLCLK:
            // Not to the system, which stretches a window from the top of the
            // work area to the bottom at its top or bottom edge.
            if (wParam >= HTLEFT && wParam <= HTBOTTOMRIGHT && !::IsZoomed(hwnd)) {
                self->reset();
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

SavedWindowSize::SavedWindowSize(QString file, QString group, QObject* parent)
    : QObject(parent)
    , m_file(std::move(file))
    , m_group(std::move(group))
{
    g_all.append(this);
    if (!g_remembering) {
        forget(); // left from when it was on
        return;
    }
    const QSettings state(m_file, QSettings::IniFormat);
    bool wide = false;
    bool tall = false;
    const int width = state.value(m_group + u"/Width"_s).toInt(&wide);
    const int height = state.value(m_group + u"/Height"_s).toInt(&tall);
    if (wide && tall && std::clamp(width, kLeast, kMost) == width && std::clamp(height, kLeast, kMost) == height)
        m_saved = {width, height};
    m_maximized = state.value(m_group + u"/Maximized"_s).toBool();
}

SavedWindowSize::~SavedWindowSize()
{
    g_all.removeOne(this);
    if (m_hwnd)
        ::RemoveWindowSubclass(reinterpret_cast<HWND>(m_hwnd), &Hook::proc, 0);
}

void SavedWindowSize::setRemembering(bool on)
{
    if (std::exchange(g_remembering, on) == on)
        return;
    for (SavedWindowSize* each : std::as_const(g_all)) {
        if (on)
            each->remember();
        else
            each->forget();
    }
}

bool SavedWindowSize::remembering()
{
    return g_remembering;
}

void SavedWindowSize::forget()
{
    {
        QSettings state(m_file, QSettings::IniFormat);
        for (const QString& key : {m_group + u"/Width"_s, m_group + u"/Height"_s, m_group + u"/Maximized"_s}) {
            if (state.contains(key))
                state.remove(key); // the file written only where there was something
        }
    }
    m_saved = {};
    if (m_shown)
        return; // what it has now lasts while the program runs
    if (!m_default.isEmpty())
        m_wanted = m_default;
    m_maximized = false;
}

void SavedWindowSize::remember()
{
    if (!m_window)
        return;
    QSettings state(m_file, QSettings::IniFormat);
    if (m_wanted != m_default) {
        state.setValue(m_group + u"/Width"_s, m_wanted.width());
        state.setValue(m_group + u"/Height"_s, m_wanted.height());
    }
    if (m_maximized)
        state.setValue(m_group + u"/Maximized"_s, true);
}

void SavedWindowSize::setWindow(QWindow* window)
{
    m_window = window;
    m_default = window->size();
    m_wanted = m_saved.isEmpty() ? m_default : m_saved;
    hook(); // first: a window not made yet gets its first handle here, no new one to hook again
    window->installEventFilter(this);
    connect(window, &QWindow::windowStateChanged, this, &SavedWindowSize::stateChanged);
}

void SavedWindowSize::hook()
{
    m_hwnd = m_window->winId();
    ::SetWindowSubclass(reinterpret_cast<HWND>(m_hwnd), &Hook::proc, 0, reinterpret_cast<DWORD_PTR>(this));
}

bool SavedWindowSize::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_window)
        return QObject::eventFilter(watched, event);
    // Qt made the window a new native one: the old handle took the subclass with it.
    if (event->type() == QEvent::PlatformSurface
        && static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated
        && m_window->winId() != m_hwnd) {
        m_sizing = false;
        hook();
    }
    // Maximized as Windows shows it (SW_SHOWMAXIMIZED), told just before:
    // told while it was hidden, Qt took any resize of the hidden window (a
    // frame restyled) for one restored, and the window opened at its size.
    if (event->type() == QEvent::Show && std::exchange(m_maximizeOnShow, false)
        && !(m_window->windowStates() & kNotNormal))
        m_window->setWindowStates(Qt::WindowMaximized);
    return QObject::eventFilter(watched, event);
}

QSize SavedWindowSize::fit(QSize wanted, QSize minimum, QSize area)
{
    const QSize room = (area - QSize(kMargin, kMargin)).expandedTo({0, 0});
    return wanted.boundedTo(room).expandedTo(minimum.boundedTo(area));
}

void SavedWindowSize::placeOn(QScreen* screen)
{
    if (!m_window)
        return;
    if (!screen)
        screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const bool first = !std::exchange(m_shown, true);
    if (m_window->windowStates() & Qt::WindowMinimized)
        m_window->setWindowStates(m_window->windowStates() & ~Qt::WindowMinimized);
    if (m_window->windowStates() & (Qt::WindowMaximized | Qt::WindowFullScreen))
        return;
    // Onto that screen first, as a window dragged there arrives: Windows tells
    // Qt of the other scale and Qt sizes the window for it. Sent there by its
    // position alone, Qt takes the size for one it scaled itself: made at 200%
    // and opened next at 150%, the window came out too wide for good, its
    // contents laid out for another width (docs/pitfalls.md).
    win::moveToMonitorOf(m_window, screen->geometry().center());
    const QRect area = screen->availableGeometry();
    const QSize size = fit(m_wanted, m_window->minimumSize(), area.size());
    m_window->setGeometry(QRect(area.center() - QPoint(size.width() / 2, size.height() / 2), size));
    // Maximized over that as it is shown (eventFilter): un-maximized, it comes
    // back to the size above.
    m_maximizeOnShow = first && m_maximized;
}

void SavedWindowSize::sized(QSize from, QSize size)
{
    // Only what its user changed: a height the work area cut short as it
    // opened, left as it was, stays the wish.
    const QSize wanted(size.width() != from.width() ? size.width() : m_wanted.width(),
        size.height() != from.height() ? size.height() : m_wanted.height());
    if (wanted == m_wanted)
        return;
    m_wanted = wanted;
    if (!g_remembering)
        return; // for this run only
    QSettings state(m_file, QSettings::IniFormat);
    state.setValue(m_group + u"/Width"_s, wanted.width());
    state.setValue(m_group + u"/Height"_s, wanted.height());
}

void SavedWindowSize::reset()
{
    m_wanted = m_default;
    {
        QSettings state(m_file, QSettings::IniFormat);
        state.remove(m_group + u"/Width"_s);
        state.remove(m_group + u"/Height"_s);
    }
    QScreen* screen = m_window ? m_window->screen() : nullptr;
    if (!screen || (m_window->windowStates() & kNotNormal))
        return;
    // About its centre, on its work area.
    const QRect area = screen->availableGeometry();
    QRect rect({}, fit(m_default, m_window->minimumSize(), area.size()));
    rect.moveCenter(m_window->geometry().center());
    rect.moveLeft(std::clamp(rect.left(), area.left(), std::max(area.left(), area.right() + 1 - rect.width())));
    rect.moveTop(std::clamp(rect.top(), area.top(), std::max(area.top(), area.bottom() + 1 - rect.height())));
    m_window->setGeometry(rect);
}

void SavedWindowSize::stateChanged(Qt::WindowState state)
{
    // Only what its user does on screen; minimized, maximized or not it still is when it comes back.
    if (state == Qt::WindowMinimized || !m_window || !m_window->isVisible())
        return;
    const bool maximized = state == Qt::WindowMaximized;
    if (maximized == m_maximized)
        return;
    m_maximized = maximized;
    if (!g_remembering)
        return;
    QSettings file(m_file, QSettings::IniFormat);
    if (maximized)
        file.setValue(m_group + u"/Maximized"_s, true);
    else
        file.remove(m_group + u"/Maximized"_s);
}

} // namespace ws
