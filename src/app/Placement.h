#pragma once

#include "platform/TaskbarSearch.h"

#include <QEasingCurve>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QVariantAnimation>
#include <QtGui/qwindowdefs.h>
#include <QtQml/qqmlregistration.h>

#include <functional>
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
//
// Or by the taskbar (attachTaskbar), as Windows' own search opens there: the
// launcher from its button on the taskbar or from Win+S. Off the taskbar by a
// gap, centred on the button along it; its edge by the taskbar stays put
// when it gets taller (atTaskbar: it does not, the list keeps its height).
//
// Opening and closing are animated (slideIn, slideOut) unless the system's
// animation effects are off. By the taskbar the window slides out from
// behind it and back, as Windows' own flyouts do: it is put just under the
// taskbar in the z-order for that, so the taskbar hides what has not come
// out yet, and kept there until it stops (activated meanwhile, as by another
// press on the box, it would come in front of the taskbar). Elsewhere it fades in rising a little, and fades out. The window
// moves with SetWindowPos, in physical pixels off where it rests (Qt keeps
// that place: moved meanwhile, it slides on to the new one); the content
// follows a little behind (contentShift, for QML).
class Placement : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged FINAL)
    // Logical pixels the content lags behind the window as it slides in.
    Q_PROPERTY(QPointF contentShift READ contentShift NOTIFY contentShiftChanged FINAL)
    Q_PROPERTY(bool atTaskbar READ atTaskbar NOTIFY atTaskbarChanged FINAL)
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
    void attachTaskbar(const taskbar::Spot& spot); // before showing it, over the taskbar

    bool moving() const { return m_moving; }
    bool atTaskbar() const { return m_taskbar.has_value(); }
    int roomNeeded() const { return m_roomNeeded; }
    void setRoomNeeded(int height);
    int room() const { return m_room; }
    int anchorWidth() const { return m_anchorWidth; }
    void setAnchorWidth(int width);

    // Shown at its place (and about to be uncloaked): in from off it; from
    // where it is, if it was on its way out. `done` once it is there.
    void slideIn(std::function<void()> done = {});
    // Out, then `done` (which hides it); back at its place after that.
    void slideOut(std::function<void()> done);
    bool sliding() const { return m_slide.state() == QAbstractAnimation::Running; }
    QPointF contentShift() const { return m_contentShift; }
    static bool animationsOn(); // the system's "animation effects"

    // Glides back to the home spot and stays there from now on.
    Q_INVOKABLE void moveHome();
    // From the right edge of the part the spot is for (anchorWidth) to that
    // of the work area, logical pixels: how much wider the window can get
    // without moving.
    Q_INVOKABLE int roomRight() const;

signals:
    void movingChanged();
    void atTaskbarChanged();
    void roomNeededChanged();
    void roomChanged();
    void anchorWidthChanged();
    void contentShiftChanged();

private:
    struct Hook; // the window procedure subclass
    friend struct Hook;

    // The taskbar it opens over, in logical pixels on its screen.
    struct TaskbarBox {
        QRect bar;
        taskbar::Edge edge = taskbar::Edge::Bottom;
        QPoint anchor;
        RECT native {}; // the taskbar, physical pixels
    };
    // An opening or a closing: from where to where, over how long.
    struct Slide {
        QPoint from, to; // the window, physical pixels off its place
        qreal opacityFrom = 1, opacityTo = 1;
        QPointF shiftFrom, shiftTo; // the content
        int moveMs = 0, fadeMs = 0, shiftMs = 0;
        QEasingCurve moveCurve, shiftCurve;
    };

    QPoint settleOn(QScreen* screen); // where the window goes there; sets room
    QPoint settleBy(QScreen* screen); // ... by the box; sets room, and with it the height (QML)
    QPoint settleByTaskbar(QScreen* screen); // ... over the taskbar; likewise
    void refit(); // its size changed: where it goes now
    void setTaskbar(std::optional<TaskbarBox> box);
    void setRoom(int room);
    void setMoving(bool moving);
    void rememberSpot(); // after a move
    void glideTo(const QPoint& target);
    void save() const;
    void moveWindow(const QPoint& position); // to its place (logical pixels), keeping a slide's offset
    void beginSlide(); // where it rests now, unless it is on its way already
    void startSlide(Slide slide, std::function<void()> done);
    void settleSlide(); // at its end, at once
    void goBehindTaskbar(); // just under it in the z-order, kept there until the slide stops
    void stepSlide(int elapsed);
    void applyOffset(); // the window where its place and the offset put it
    QPoint hiddenOffset(bool* cut) const; // behind the taskbar (or not all the way: cut)
    bool layered() const; // the window can fade (it has an alpha channel)

    QString m_stateFile;
    QString m_group;
    QPointer<QWindow> m_window;
    WId m_hwnd = 0;
    QPointF m_anchor; // centre across, top down; fractions of the work area
    int m_anchorWidth = 0;
    std::optional<QRect> m_box; // attached to it
    std::optional<TaskbarBox> m_taskbar; // over it
    QPointer<QScreen> m_taskbarScreen; // the taskbar's
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
    QVariantAnimation m_slide; // milliseconds into it
    WId m_under = 0; // the taskbar it slides behind, while it does
    Slide m_slideSpec;
    std::function<void()> m_slideDone;
    QPoint m_place; // its place, logical pixels
    QPoint m_rest; // ... physical pixels
    QPoint m_offset; // off it, physical pixels
    qreal m_opacity = 1;
    QPointF m_contentShift;
};

} // namespace ws
