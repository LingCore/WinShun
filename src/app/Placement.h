#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QVariantAnimation>
#include <QtGui/qwindowdefs.h>
#include <QtQml/qqmlregistration.h>

class QScreen;
class QWindow;

namespace ws {

// Where the launcher opens, and moving it with the mouse.
//
// Its spot is kept relative to a monitor's work area (where its centre is
// across, where its top is down), so it carries over to whichever monitor the
// launcher opens on and survives resolution and scaling changes. Home is
// centred, a fifth of the way down.
//
// The launcher is dragged by its header and footer (WindowFrame), in the
// system's own move loop: the window follows the pointer exactly, crosses
// monitors, and Esc puts it back. The loop is shaped on the way (WM_MOVING):
// the window stays on the work area of the monitor under the pointer with
// room below for its tallest layout, so a full list never runs off the
// screen, and it sticks to the centre line and to the home height. The loop's
// messages reach us through a subclass of the window procedure: Qt passes
// them to no native event filter (QTBUG-67095).
class Placement : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged FINAL)
    // Height of the launcher with every row shown (logical pixels).
    Q_PROPERTY(int fullHeight READ fullHeight WRITE setFullHeight NOTIFY fullHeightChanged FINAL)

public:
    explicit Placement(QString stateFile, QObject* parent = nullptr);
    ~Placement() override;

    void setWindow(QWindow* window); // native handle created
    void placeOn(QScreen* screen); // before showing the window there

    bool moving() const { return m_moving; }
    int fullHeight() const { return m_fullHeight; }
    void setFullHeight(int height);

    // Glides back to the home spot and stays there from now on.
    Q_INVOKABLE void moveHome();

signals:
    void movingChanged();
    void fullHeightChanged();

private:
    struct Hook; // the window procedure subclass
    friend struct Hook;

    QPoint positionIn(const QRect& area) const;
    void setMoving(bool moving);
    void rememberSpot(); // after a move
    void save() const;

    QString m_stateFile;
    QPointer<QWindow> m_window;
    WId m_hwnd = 0;
    QPointF m_anchor; // centre across, top down; fractions of the work area
    int m_fullHeight = 0;
    bool m_moving = false; // inside the move loop
    QPoint m_moveStart;
    QPoint m_grab; // the pointer's offset in the window being moved (physical pixels)
    bool m_grabbed = false; // m_grab is set for this move
    QVariantAnimation m_glide; // moveHome()
};

} // namespace ws
