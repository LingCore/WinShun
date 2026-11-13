#include "FrameReveal.h"

#include <QQuickWindow>

#include <utility>

namespace ws {

void FrameReveal::setWindow(QQuickWindow* window, std::chrono::milliseconds timeout, std::function<void()> reveal)
{
    m_window = window;
    m_reveal = std::move(reveal);
    m_timeout.setSingleShot(true);
    m_defaultTimeout = timeout;
    connect(&m_timeout, &QTimer::timeout, this, [this] { finish(m_pending); });
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible)
            cancel();
    });
    connect(window, &QQuickWindow::afterSynchronizing, this, [this] {
        m_synchronized = m_armed.load();
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        if (const quint64 generation = m_synchronized.exchange(0))
            QMetaObject::invokeMethod(this, [this, generation] { finish(generation); }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
}

void FrameReveal::begin(std::chrono::milliseconds timeout)
{
    m_armed = 0;
    m_pending = ++m_generation;
    m_timeout.start(timeout.count() > 0 ? timeout : m_defaultTimeout);
}

void FrameReveal::arm()
{
    if (!m_pending || !m_window)
        return;
    m_armed = m_pending;
    m_window->update(); // a fresh frame even if its contents did not change
}

void FrameReveal::cancel()
{
    m_pending = 0;
    m_armed = 0;
    m_timeout.stop();
}

void FrameReveal::finish(quint64 generation)
{
    // A queued or in-flight frame from the last opening must not reveal
    // the next one or stop its fallback timer.
    if (!generation || generation != m_pending || !m_window || !m_window->isVisible())
        return;
    cancel();
    m_reveal();
}

} // namespace ws
