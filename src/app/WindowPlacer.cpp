#include "WindowPlacer.h"

#include "platform/WindowEffects.h"

#include <chrono>

namespace ws {

using namespace std::chrono_literals;

WindowPlacer::WindowPlacer(QObject* parent)
    : QObject(parent)
{
}

WindowPlacer::~WindowPlacer()
{
    uncloak();
}

void WindowPlacer::setWindow(QQuickWindow* window)
{
    if (window == m_window)
        return;
    uncloak();
    m_window = window;
    if (window) {
        // Shown again at the latest after a few frames' time, should none come.
        m_reveal.setWindow(window, 150ms, [this] { uncloak(); });
        // The frame waited for is one drawn once the window has its new size:
        // armed when that size is in (at once, or with the resize event).
        const auto sized = [this] {
            if (m_cloaked && m_window && m_window->size() == m_waitedSize)
                m_reveal.arm();
        };
        connect(window, &QWindow::widthChanged, this, sized);
        connect(window, &QWindow::heightChanged, this, sized);
        // Hidden meanwhile: never shown again cloaked.
        connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
            if (!visible)
                uncloak();
        });
    }
    emit windowChanged();
}

void WindowPlacer::place(const QRect& geometry)
{
    if (!m_window || !geometry.isValid() || geometry == m_window->geometry())
        return;
    if (!m_window->isVisible() || geometry.size() == m_window->size()) {
        m_window->setGeometry(geometry);
        return;
    }
    m_waitedSize = geometry.size();
    m_reveal.begin();
    if (!m_cloaked) {
        win::setCloaked(m_window, true);
        m_cloaked = true;
    }
    m_window->setGeometry(geometry);
    if (m_window->size() == m_waitedSize)
        m_reveal.arm();
}

void WindowPlacer::uncloak()
{
    m_reveal.cancel();
    if (!m_cloaked)
        return;
    m_cloaked = false;
    if (m_window)
        win::setCloaked(m_window, false);
}

} // namespace ws
