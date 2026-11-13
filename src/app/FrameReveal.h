#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <functional>

class QQuickWindow;

namespace ws {

// Wait for a frame belonging to this opening. The launcher arms it once
// its results are ready; the clipboard arms it with the opening itself.
class FrameReveal : public QObject {
public:
    void setWindow(QQuickWindow* window, std::chrono::milliseconds timeout, std::function<void()> reveal);
    // `timeout`: revealed after it all the same, should no frame come; the
    // one given to setWindow unless said here.
    void begin(std::chrono::milliseconds timeout = {});
    void arm();
    void cancel();
    bool pending() const { return m_pending != 0; }

private:
    void finish(quint64 generation);

    QPointer<QQuickWindow> m_window;
    std::function<void()> m_reveal;
    QTimer m_timeout;
    std::chrono::milliseconds m_defaultTimeout {};
    quint64 m_generation = 0;
    quint64 m_pending = 0; // GUI thread: stays pending until it actually reveals
    std::atomic<quint64> m_armed {0};
    std::atomic<quint64> m_synchronized {0}; // render thread: the opening this frame belongs to
};

} // namespace ws
