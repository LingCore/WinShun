#pragma once

#include "platform/TaskbarSearch.h"

#include <QEasingCurve>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QTimer>
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
// the window, as tall as it is now, stays within the outer desktop bounds
// and may span monitors. Let go near the centre line or the home height, it
// glides onto them; let go so low that a few rows would not fit below, it
// glides up until they do. While moving it follows the pointer and nothing
// else. The list shows as many rows as fit below where the window settles
// (room), so it never runs off the screen and the window can sit low. The
// loop's messages reach us through a subclass of the window procedure: Qt
// passes them to no native event filter (QTBUG-67095).
//
// Instead of at its spot, a window can go by a box (attach): the clipboard
// drops down under the search box of ours or the text caret it pastes at,
// level with its left edge, or goes over it where more rows fit there
// (boxside::above). Moved by the user then, it stays where it is let go, and
// its spot stays as it was.
//
// Or by the taskbar (attachTaskbar), as Windows' own search opens there: the
// launcher from its button on the taskbar or from Win+S. Off the taskbar by a
// gap, centred on the button along it; its edge by the taskbar stays put
// when it gets taller (atTaskbar: it does not, the list keeps its height).
//
// Opening and closing are animated (slideIn, slideOut) unless the system's
// animation effects are off: it fades in coming a little from the taskbar's
// side (elsewhere: rising), and fades out where it is. By the taskbar it
// stays over it all the while, moving no further than the gap between them:
// a see-through taskbar (TranslucentTB) showed a window that slid behind it,
// and kept the last of it in its blur once the window was hidden. The window
// moves with SetWindowPos, in physical pixels off where it rests (Qt keeps
// that place: moved meanwhile, it slides on to the new one); the content
// follows a little behind (contentShift, for QML).
//
// The launcher's width is its user's too (rememberWidth): dragged at its left
// or right edge (WindowFrame::setSideSizing, beginSizing) it grows or shrinks
// on both sides at once, its centre staying where it is (its spot is that
// centre; by the taskbar, the button), as long as the work area allows. The
// width is remembered (state.ini [<group>] Width, logical pixels) and the same
// on every monitor, but never wider than one's work area; a double click on
// the edge (resetWidth) puts it back.
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
    // ... with the most rows it shows, and what a row adds to it (logical
    // pixels; 0: not said). By a box, it opens on the side where more of
    // those rows fit (boxside::above).
    Q_PROPERTY(int roomWanted READ roomWanted WRITE setRoomWanted NOTIFY roomWantedChanged FINAL)
    Q_PROPERTY(int roomStep READ roomStep WRITE setRoomStep NOTIFY roomStepChanged FINAL)
    // From the top of the window, where it is put or settles, to the bottom
    // of that work area (logical pixels); over a box, from the box up to the
    // top of it. Kept while it is being moved.
    Q_PROPERTY(int room READ room NOTIFY roomChanged FINAL)
    // By a box, it is over it (its bottom at the box) rather than under it:
    // set as it is placed there, before it shows; kept while it is moved by
    // the user and until it is placed again. False when not by a box.
    Q_PROPERTY(bool above READ above NOTIFY aboveChanged FINAL)
    // How much of the window, from its left, its spot is the centre of (the
    // clipboard's list: its preview makes it wider to the right); 0: all.
    Q_PROPERTY(int anchorWidth READ anchorWidth WRITE setAnchorWidth NOTIFY anchorWidthChanged FINAL)
    // The launcher's width (logical pixels) where it is placed now: its
    // user's (see above) within that work area; 0 when not managed here.
    Q_PROPERTY(int boxWidth READ boxWidth NOTIFY boxWidthChanged FINAL)
    // Its edge held, widening it (beginSizing).
    Q_PROPERTY(bool sizing READ sizing NOTIFY sizingChanged FINAL)
    Q_PROPERTY(bool windowSizing READ windowSizing NOTIFY windowSizingChanged FINAL)
    // The content's logical size, independent of unsolicited native resizes.
    Q_PROPERTY(QSize contentSize READ contentSize WRITE setContentSize NOTIFY contentSizeChanged FINAL)

public:
    // `group`: the state file's section with its spot ("Launcher"). A window
    // without a spot of its own yet opens where the launcher does.
    Placement(QString stateFile, QString group, QObject* parent = nullptr);
    ~Placement() override;

    void setWindow(QWindow* window); // native handle created
    QSize contentSize() const { return m_contentSize; }
    void setContentSize(QSize size);
    void placeOn(QScreen* screen); // before showing the window there, at its spot
    // A temporary spot beside the file manager typed in; never saved as home.
    // `inside`: in its bottom right corner rather than beside it, as wide as it
    // leaves (not below the least width; not remembered); larger than it, over
    // `lists` (the lister's file lists together) before the rest of `frame`.
    // `orInto` (File Explorer's file list, GitHub #203): beside `window` only
    // where all of the box keeps clear of it; else inside, in that list's
    // corner within `frame`. Decided here, once: its rows coming and going
    // later do not move it between the two.
    void avoidNative(const QRect& window, QScreen* screen, bool inside = false, const QRect& lists = {},
        const QRect& frame = {}, const QRect& orInto = {});
    // Where avoidNative put it, for the diagnostics journal (GitHub #203):
    // by the file manager typed in at all; in its file list's corner, else
    // beside it.
    bool avoiding() const { return m_avoid.has_value(); }
    bool avoidingInside() const { return m_avoid.has_value() && m_avoidInside; }
    // Geometry supplied by the ordinary-process peer to its UIAccess display.
    void placeDisplay(const QRect& geometry, int room, bool above);
    // A user move in the display peer; retain the usual temporary/home rules.
    void adoptDisplayGeometry(const QRect& geometry);
    // Before showing it, by `box` (logical pixels, on `screen`; none: the
    // screen at the box's centre).
    void attach(const QRect& box, QScreen* screen = nullptr);
    void attachNative(const QRect& box); // ... physical pixels (another program's text caret)
    void attachTaskbar(const taskbar::Spot& spot); // before showing it, over the taskbar

    bool moving() const { return m_moving; }
    bool atTaskbar() const { return m_taskbar.has_value(); }
    int roomNeeded() const { return m_roomNeeded; }
    void setRoomNeeded(int height);
    int roomWanted() const { return m_roomWanted; }
    void setRoomWanted(int height);
    int roomStep() const { return m_roomStep; }
    void setRoomStep(int height);
    int room() const { return m_room; }
    bool above() const { return m_above; }
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

    // The search panel (Launcher::panel): the window as large as its user
    // left it (state.ini [Panel]: its size, and its spot as fractions of the
    // work area, as the launcher's), within the work area of `screen`; about
    // 1100 by 700 logical pixels at first, centred, a tenth of the way down.
    // Meanwhile it is neither refitted nor glided; moved or sized by its
    // user, that is what is remembered. leavePanel() remembers and goes back
    // to being placed as before (the caller places it again). The taskbar
    // spot it was opened over, if any, is kept for that (taskbarSpot).
    bool panel() const { return m_panel; }
    QRect panelRect(QScreen* screen) const;
    void enterPanel(QScreen* screen);
    void leavePanel();
    std::optional<taskbar::Spot> taskbarSpot() const { return m_taskbar ? m_taskbarSpot : std::nullopt; }

    // Glides back to the home spot and stays there from now on.
    Q_INVOKABLE void moveHome();
    // Centre on the current monitor's work area, preserving size and content.
    // Also detaches temporary taskbar/file-manager placement; saved per mode.
    Q_INVOKABLE void centerOnScreen();
    // Its rows and type changed size (界面大小): a window up now (the
    // clipboard, pinned) is fitted on its screen again.
    void resized();
    // From the right edge of the part the spot is for (anchorWidth) to that
    // of the work area, logical pixels: how much wider the window can get
    // without moving.
    Q_INVOKABLE int roomRight() const;

    // Its width is managed here (the launcher's): `defaultWidth` until its
    // user changes it, never under `minimumWidth` (logical pixels).
    void rememberWidth(int defaultWidth, int minimumWidth);
    int boxWidth() const { return m_boxWidth; }
    bool sizing() const { return m_sizing; }
    // Native resizing on all edges, separate from the launcher's centred width grip.
    void enableWindowSizing() { m_windowSizingEnabled = true; }
    bool windowSizing() const { return m_windowSizing; }
    bool customWidth() const { return m_defaultWidth > 0 && m_wantedWidth != m_defaultWidth; }
    // A press on its left or right edge (WindowFrame::sideGrabbed): the
    // window follows the pointer until the button is let go; Esc puts it back.
    void beginSizing(bool left);
    // Back to the default width, centred where it is (a double click on the edge, the settings).
    Q_INVOKABLE void resetWidth();

signals:
    void movingChanged();
    void atTaskbarChanged();
    void roomNeededChanged();
    void roomWantedChanged();
    void roomStepChanged();
    void roomChanged();
    void aboveChanged();
    void anchorWidthChanged();
    void contentShiftChanged();
    void boxWidthChanged();
    void sizingChanged();
    void widthReset(); // resetWidth() changed it
    void contentSizeChanged();
    void windowSizingChanged();
    void windowResized(QSize previousSize, QSize size);
    void sizeResetRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

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

    int widthNow() const; // what it is as placed: the launcher's own width, else the window's
    int widthOn(QScreen* screen) const; // the launcher's width on that screen's work area
    void fitWidth(QScreen* screen); // before placing it there
    void stepSizing(); // the pointer moved, the edge held
    void endSizing(bool cancel);
    // Physical pixels wide, centred on `centre` (physical), within the work area.
    void applyWidth(int width, qreal centre);
    void keepWidth(); // as it is now: remembered, and m_left (dropped) where it went
    QPoint settleOn(QScreen* screen, bool updateRoom = true); // where the window goes there; sets room
    QPoint settleAvoiding(QScreen* screen, bool fitList = false); // fitList: inside, narrowed to the list
    void attachBy(const QRect& box, QScreen* screen, int gap); // attach, this far off the box
    QPoint settleBy(QScreen* screen); // ... by the box; sets room, and with it the height (QML)
    QPoint settleByTaskbar(QScreen* screen); // ... over the taskbar; likewise
    void refit(); // its size changed: where it goes now
    void watchScreen(QScreen* screen);
    void scheduleDisplayRestore();
    void restoreDisplayGeometry();
    void setTaskbar(std::optional<TaskbarBox> box);
    void setRoom(int room);
    void setAbove(bool above);
    void setMoving(bool moving);
    void rememberSpot(); // after a move
    void savePanel(); // the panel's size and spot, after a move or a resize
    void glideTo(const QPoint& target);
    void save() const;
    void moveWindow(const QPoint& position); // to its place (logical pixels), keeping a slide's offset
    void beginSlide(); // where it rests now, unless it is on its way already
    void startSlide(Slide slide, std::function<void()> done);
    void settleSlide(); // at its end, at once
    void stepSlide(int elapsed);
    void applyOffset(); // the window where its place and the offset put it
    QPoint nearOffset(int logical) const; // toward the taskbar, not onto it (physical pixels)
    QPointF towardTaskbar() const; // which way it is, one logical pixel
    bool layered() const; // the window can fade (it has an alpha channel)

    QString m_stateFile;
    QString m_group;
    QPointer<QWindow> m_window;
    QSize m_contentSize;
    QTimer m_displaySettle;
    QList<QMetaObject::Connection> m_screenConnections;
    bool m_displayRestorePending = false;
    WId m_hwnd = 0;
    QPointF m_anchor; // centre across, top down; fractions of the work area
    int m_anchorWidth = 0;
    std::optional<QRect> m_box; // attached to it
    std::optional<QRect> m_avoid; // native bounds of the file manager typed in
    bool m_avoidInside = false; // the corner of m_avoid, not beside it
    QRect m_avoidLists; // ... native: the lister's file lists together; empty: not known
    QRect m_avoidFrame; // ... native: the lister's client area; empty: not known
    int m_boxGap = 0; // ... this far off it, logical pixels
    std::optional<TaskbarBox> m_taskbar; // over it
    std::optional<taskbar::Spot> m_taskbarSpot; // ... as attachTaskbar was given it
    QPointer<QScreen> m_taskbarScreen; // the taskbar's
    bool m_panel = false; // see enterPanel
    QPointF m_panelAnchor {0.5, 0.1}; // the panel's spot: centre across, top down (fractions of the work area)
    QSize m_panelSize; // logical pixels; empty: not set yet
    bool m_dropped = false; // was attached, then moved by the user: stays there
    int m_left = 0; // attached or dropped: where its left edge goes
    bool m_above = false; // attached over the box: its bottom stays at the box
    int m_roomNeeded = 0;
    int m_roomWanted = 0;
    int m_roomStep = 0;
    int m_room = 0;
    bool m_moving = false; // inside the move loop
    QPoint m_moveStart;
    QPoint m_grab; // the pointer's offset in the window being moved (physical pixels)
    bool m_grabbed = false; // m_grab is set for this move
    QVariantAnimation m_glide; // moveHome()
    QVariantAnimation m_slide; // milliseconds into it
    Slide m_slideSpec;
    std::function<void()> m_slideDone;
    QPoint m_place; // its place, logical pixels
    QPoint m_rest; // ... physical pixels
    QPoint m_offset; // off it, physical pixels
    qreal m_opacity = 1;
    QPointF m_contentShift;
    int m_defaultWidth = 0; // see rememberWidth; 0: not managed
    int m_minWidth = 0;
    int m_wantedWidth = 0; // its user's
    int m_boxWidth = 0; // ... within the work area it is on
    bool m_sizing = false;
    bool m_windowSizingEnabled = false;
    bool m_windowSizing = false;
    QSize m_resizeStart;
    bool m_sizeLeft = false;
    RECT m_sizeStart {}; // the window as the edge was grabbed, physical pixels
    POINT m_sizeFrom {}; // ... and the pointer
};

} // namespace ws
