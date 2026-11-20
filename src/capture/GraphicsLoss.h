#pragma once

#include <QQuickWindow>

#include <atomic>

namespace ws::capture {

// Graphics thrown away under windows in use: the graphics device lost
// (Windows resetting a card that stopped answering, TDR, or a driver update)
// and Qt Quick starting over. QSGThreadedRenderLoop::handleDeviceLoss tears
// the scene graph down, which is sceneGraphInvalidated, and the next frame
// makes it anew. Also counted: a window destroyed (the overlays made anew
// for a changed monitor). Counted on the drawing thread as it happens, read
// on any (the capture's journal, docs/pitfalls.md "截图：遮罩一出来固定卡约 2 秒").
class GraphicsLoss {
public:
    // From now on. The window must not outlive this.
    void watch(QQuickWindow* window)
    {
        QObject::connect(
            window, &QQuickWindow::sceneGraphInvalidated, window,
            [this] { m_lost.fetch_add(1, std::memory_order_relaxed); }, Qt::DirectConnection);
    }
    void begin() { m_before = m_lost.load(std::memory_order_relaxed); } // counted from here
    int since() const { return m_lost.load(std::memory_order_relaxed) - m_before; }

private:
    std::atomic<int> m_lost {0};
    int m_before = 0;
};

} // namespace ws::capture
