#pragma once

#include "FrameReveal.h"

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRect>
#include <QSize>
#include <QtQml/qqmlregistration.h>

namespace ws {

// Moves and sizes a window in one go (not by x, y, width and height one
// after another). One on show that changes its size is off the screen
// (cloaked) until a frame of the new size is drawn: in between Windows
// would show the frame before, cut off, or black where the window grew
// (ClipPictureViewer going from one picture to the next, GitHub #414).
// Only moved, or hidden, it is simply put there.
class WindowPlacer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow* window READ window WRITE setWindow NOTIFY windowChanged FINAL)

public:
    explicit WindowPlacer(QObject* parent = nullptr);
    ~WindowPlacer() override;

    QQuickWindow* window() const { return m_window; }
    void setWindow(QQuickWindow* window);

    // In the window's own (device-independent) pixels, as QWindow::geometry.
    Q_INVOKABLE void place(const QRect& geometry);

signals:
    void windowChanged();

private:
    void uncloak();

    QPointer<QQuickWindow> m_window;
    FrameReveal m_reveal;
    QSize m_waitedSize; // the size the frame to show it again is to have
    bool m_cloaked = false;
};

} // namespace ws
