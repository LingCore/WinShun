#include "DialogBarVisibility.h"

#include "platform/DialogOwner.h"
#include "platform/WindowEffects.h"

#include <QEvent>
#include <QVariant>

#include <dwmapi.h>

#include <utility>

namespace ws {

void DialogBarVisibility::setWindow(QQuickWindow* window, std::function<void(bool)> showLogo)
{
    m_window = window;
    m_showLogo = std::move(showLogo);
    window->installEventFilter(this);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(300);
    connect(&m_timeout, &QTimer::timeout, this, [this] { reveal(m_pending.load()); });
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible) {
            cancel(); // close/Alt+F4 can hide the bar without DialogBar::hideWindow
            win::setCloaked(m_window, false);
        }
    });
    connect(window, &QQuickWindow::afterSynchronizing, this, [this] {
        m_synchronized = m_pending.load();
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        // place() runs on the GUI thread. Until that thread really removes
        // the cloak, it must not see the bar as ready and show only the logo.
        if (const quint64 generation = m_synchronized.exchange(0))
            QMetaObject::invokeMethod(this, [this, generation] { reveal(generation); }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
}

bool DialogBarVisibility::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && event->type() == QEvent::Close) {
        // QWindow::close() destroys the HWND. Keep the bar's native identity:
        // DialogJump still recognises it as the file dialog's companion.
        event->ignore();
        hide();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void DialogBarVisibility::show(HWND dialog, bool elevated)
{
    if (!m_window)
        return;
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    // Right above the dialog again, in its band: also when the bar is up
    // already, as the dialog may have come up over it.
    win::followDialog(hwnd, dialog, elevated);
    if (m_pending.load() || isRevealed())
        return;
    m_showLogo(false);
    win::setCloaked(m_window, true);
    m_pending = ++m_generation;
    m_timeout.start();
    m_window->setProperty("_q_showWithoutActivating", true);
    m_window->show();
    m_window->setProperty("_q_showWithoutActivating", QVariant());
    // Qt can still consider a natively hidden window visible: show() then
    // does nothing. Recover it without taking focus from the file dialog.
    if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd))
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    // Showing may put it elsewhere in the Z-order: back over the dialog while
    // the bar is still cloaked, before the first frame reveals it.
    win::followDialog(hwnd, dialog, elevated);
    m_window->update(); // a fresh frame even if its query and size did not change
}

void DialogBarVisibility::hide()
{
    cancel(); // also when Qt already hid it: no orphan logo or pending callback
    if (m_window) {
        m_window->hide();
        win::setCloaked(m_window, false);
    }
}

void DialogBarVisibility::cancel()
{
    m_pending = 0;
    m_timeout.stop();
    if (m_showLogo)
        m_showLogo(false);
}

void DialogBarVisibility::reveal(quint64 generation)
{
    // A frame from before a drag, foreground switch, or close belongs to
    // that opening. It must not reveal the next one or stop its timeout.
    if (!generation || generation != m_pending.load() || !m_window || !m_window->isVisible())
        return;
    m_timeout.stop();
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd))
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    win::setCloaked(m_window, false);
    m_pending = 0;
    m_window->update(); // the timeout path may not have painted a frame yet
    if (isRevealed())
        m_showLogo(true);
}

bool DialogBarVisibility::isRevealed() const
{
    if (!m_window || !m_window->isVisible() || m_pending.load())
        return false;
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd))
        return false;
    DWORD cloaked = 0;
    ::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    return cloaked == 0;
}

} // namespace ws
