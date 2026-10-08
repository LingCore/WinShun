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
// the window, as tall as it is now, stays on the work area of the monitor
// under the pointer. Let go near the centre line or the home height, it
// glides onto them; let go so low that a few rows would not fit below, it
// glides up until they do. While moving it follows the pointer and nothing
// else. The list shows as many rows as fit below where the window settles
// (room), so it never runs off the screen and the window can sit low. The
// loop's messages reach us through a subclass of the window procedure: Qt
// passes them to no native event filter (QTBUG-67095).
class Placement : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged FINAL)
    // Height of the launcher with the fewest rows it keeps room for (logical
    // pixels): it settles no lower than that fits.
    Q_PROPERTY(int roomNeeded READ roomNeeded WRITE setRoomNeeded NOTIFY roomNeededChanged FINAL)
    // From the top of the launcher, where it is put or settles, to the bottom
    // of that work area (logical pixels). Kept while it is being moved.
    Q_PROPERTY(int room READ room NOTIFY roomChanged FINAL)

public:
    explicit Placement(QString stateFile, QObject* parent = nullptr);
    ~Placement() override;

    void setWindow(QWindow* window); // native handle created
    void placeOn(QScreen* screen); // before showing the window there

    bool moving() const { return m_moving; }
    int roomNeeded() const { return m_roomNeeded; }
    void setRoomNeeded(int height);
    int room() const { return m_room; }

    // Glides back to the home spot and stays there from now on.
    Q_INVOKABLE void moveHome();

signals:
    void movingChanged();
    void roomNeededChanged();
    void roomChanged();

private:
    struct Hook; // the window procedure subclass
    friend struct Hook;

    QPoint settleOn(QScreen* screen); // where the window goes there; sets room
    void setMoving(bool moving);
    void rememberSpot(); // after a move
    void glideTo(const QPoint& target);
    void save() const;

    QString m_stateFile;
    QPointer<QWindow> m_window;
    WId m_hwnd = 0;
    QPointF m_anchor; // centre across, top down; fractions of the work area
    int m_roomNeeded = 0;
    int m_room = 0;
    bool m_moving = false; // inside the move loop
    QPoint m_moveStart;
    QPoint m_grab; // the pointer's offset in the window being moved (physical pixels)
    bool m_grabbed = false; // m_grab is set for this move
    QVariantAnimation m_glide; // moveHome()
};

} // namespace ws
