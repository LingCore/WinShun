#pragma once

#include <QQuickWindow>
#include <QTimer>

#include <chrono>

namespace ws::capture {

// Keep a window's scene graph warm across quick reopenings, but let Qt
// release its device, swapchain and caches after a hidden interval. The
// window and its QML/model objects survive, including unsaved annotations.
inline void releaseIdleGraphics(QQuickWindow* window, std::chrono::milliseconds delay = std::chrono::seconds(20))
{
    auto* idle = new QTimer(window);
    idle->setSingleShot(true);
    idle->setInterval(delay);
    QObject::connect(window, &QWindow::visibleChanged, idle, [window, idle](bool visible) {
        if (visible) {
            idle->stop();
            window->setPersistentGraphics(true);
            window->setPersistentSceneGraph(true);
        } else {
            idle->start();
        }
    });
    QObject::connect(idle, &QTimer::timeout, window, [window] {
        if (window->isVisible())
            return;
        window->setPersistentSceneGraph(false);
        window->setPersistentGraphics(false);
        window->releaseResources();
    });
}

} // namespace ws::capture
