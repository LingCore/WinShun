#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QVariantAnimation>
#include <QtGui/qwindowdefs.h>
#include <QtQml/qqmlregistration.h>

#include <optional>

class QScreen;
class QWindow;

namespace ws {

// Where a window of ours opens (the launcher, the clipboard), and moving it
// with the mouse.
//
// Its spot is kept relative to a monitor's work area (where its centre is
// across, where its top is down), so it carries over to whichever monitor the
// window opens on and survives resolution and scaling changes. Home is
// centred, a fifth of the way down.
//
// The window is dragged by its header and footer (WindowFrame), in the
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
//
// Instead of at its spot, a window can go by a box (attach): the clipboard
// drops down under the search box of ours or the text caret it pastes at,
// level with its left edge, or goes over it where more rows fit there. Moved
// by the user then, it stays where it is let go, and its spot stays as it was.
class Placement : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged FINAL)
    // Height of the window with the fewest rows it keeps room for (logical
    // pixels): it settles no lower than that fits.
    Q_PROPERTY(int roomNeeded READ roomNeeded WRITE setRoomNeeded NOTIFY roomNeededChanged FINAL)
    // From the top of the window, where it is put or settles, to the bottom
    // of that work area (logical pixels); over a box, from the box up to the
    // top of it. Kept while it is being moved.
    Q_PROPERTY(int room READ room NOTIFY roomChanged FINAL)
    // How much of the window, from its left, its spot is the centre of (the
    // clipboard's list: its preview makes it wider to the right); 0: all.
    Q_PROPERTY(int anchorWidth READ anchorWidth WRITE setAnchorWidth NOTIFY anchorWidthChanged FINAL)

public:
    // `group`: the state file's section with its spot ("Launcher"). A window
    // without a spot of its own yet opens where the launcher does.
    Placement(QString stateFile, QString group, QObject* parent = nullptr);
    ~Placement() override;

    void setWindow(QWindow* window); // native handle created
    void placeOn(QScreen* screen); // before showing the window there, at its spot
    void attach(const QRect& box); // before showing it, by `box` (logical pixels, on the screen)
    void attachNative(const QRect& box); // ... physical pixels (another program's text caret)

    bool moving() const { return m_moving; }
    int roomNeeded() const { return m_roomNeeded; }
    void setRoomNeeded(int height);
    int room() const { return m_room; }
    int anchorWidth() const { return m_anchorWidth; }
    void setAnchorWidth(int width);

    // Glides back to the home spot and stays there from now on.
    Q_INVOKABLE void moveHome();
    // From the right edge of the part the spot is for (anchorWidth) to that
    // of the work area, logical pixels: how much wider the window can get
    // without moving.
    Q_INVOKABLE int roomRight() const;

signals:
    void movingChanged();
    void roomNeededChanged();
    void roomChanged();
    void anchorWidthChanged();

private:
    struct Hook; // the window procedure subclass
    friend struct Hook;

    QPoint settleOn(QScreen* screen); // where the window goes there; sets room
    QPoint settleBy(QScreen* screen); // ... by the box; sets room, and with it the height (QML)
    void refit(); // its size changed: where it goes now
    void setRoom(int room);
    void setMoving(bool moving);
    void rememberSpot(); // after a move
    void glideTo(const QPoint& target);
    void save() const;

    QString m_stateFile;
    QString m_group;
    QPointer<QWindow> m_window;
    WId m_hwnd = 0;
    QPointF m_anchor; // centre across, top down; fractions of the work area
    int m_anchorWidth = 0;
    std::optional<QRect> m_box; // attached to it
    bool m_dropped = false; // was attached, then moved by the user: stays there
    int m_left = 0; // attached or dropped: where its left edge goes
    bool m_above = false; // attached over the box: its bottom stays at the box
    int m_roomNeeded = 0;
    int m_room = 0;
    bool m_moving = false; // inside the move loop
    QPoint m_moveStart;
    QPoint m_grab; // the pointer's offset in the window being moved (physical pixels)
    bool m_grabbed = false; // m_grab is set for this move
    QVariantAnimation m_glide; // moveHome()
};

} // namespace ws
