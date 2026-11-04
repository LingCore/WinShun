#pragma once

#include "SystemTheme.h"

#include <QList>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QVariant>
#include <QtGui/qwindowdefs.h>
#include <QtQml/qqmlregistration.h>

#include <atomic>

class QWindow;

namespace ws {

// Before the window is made (create(), show(), winId()): an alpha channel
// for good, for a 界面风格 that rounds the window itself (OwnFrame, while
// SystemTheme::ownFrameAvailable()) and, with `backdrop`, for the Mica behind
// it (SystemTheme::backdropAvailable()). Kept through colour changes: an
// opaque QQuickWindow::color takes the alpha out of the requested format,
// though the swap chain made with it keeps its alpha (Qt's D3D11 backend then
// logged a warning on every resize). Nothing once the window is made. Inline:
// the menus' hosts are built into tests without this file's .cpp.
inline void prepareWindowAlpha(QWindow* window, bool backdrop = false)
{
    if (!window || window->handle())
        return;
    // Available, not on: the 透明效果 setting and the style can change later.
    if (!SystemTheme::ownFrameAvailable() && !(backdrop && SystemTheme::backdropAvailable()))
        return;
    const auto keepAlpha = [window] {
        QSurfaceFormat format = window->requestedFormat();
        if (format.alphaBufferSize() < 8) {
            format.setAlphaBufferSize(8);
            window->setFormat(format);
        }
    };
    keepAlpha();
    auto* quick = qobject_cast<QQuickWindow*>(window);
    if (quick && !quick->property("_ws_alphaKept").toBool()) { // menus are prepared before every show
        quick->setProperty("_ws_alphaKept", true);
        QObject::connect(quick, &QQuickWindow::colorChanged, quick, keepAlpha);
    }
}

// The frame of a 界面风格 that rounds the windows itself (Theme.ownFrame, see
// win::setOwnFrame), set from inside the window's scene (WindowEdge.qml):
// while `active` with a `radius`, the window has it from when it is made,
// with that radius and `shadow` (the layers, see win::setOwnFrame); inactive,
// gone or moved to another window, the window has DWM's frame again.
// `applied`: the window has it (an alpha channel from its creation, see
// prepareWindowAlpha; never with the software renderer). Draws nothing.
class OwnFrame : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged FINAL)
    Q_PROPERTY(QVariant shadow READ shadow WRITE setShadow NOTIFY shadowChanged FINAL)
    Q_PROPERTY(bool applied READ applied NOTIFY appliedChanged FINAL)

public:
    explicit OwnFrame(QQuickItem* parent = nullptr);
    ~OwnFrame() override;

    bool active() const { return m_active; }
    void setActive(bool on);
    qreal radius() const { return m_radius; }
    void setRadius(qreal radius);
    QVariant shadow() const { return m_shadow; }
    void setShadow(const QVariant& shadow);
    bool applied() const { return m_applied; }

signals:
    void activeChanged();
    void radiusChanged();
    void shadowChanged();
    void appliedChanged();

protected:
    void componentComplete() override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void follow(QQuickWindow* window);
    void apply();
    void setApplied(bool on);

    bool m_active = false;
    qreal m_radius = 0;
    QVariantList m_shadow; // plain lists and maps, whatever QML gave
    QPointer<QQuickWindow> m_window;
    bool m_applied = false;
};

// Lets Windows treat parts of a frameless window as its title bar, the way
// Windows' own custom title bars work (WinUI's caption regions, Electron's
// drag regions): QML names the items that drag the window and the controls
// inside them; hit testing (WM_NCHITTEST) answers HTCAPTION over the former.
// Dragging, snapping, double-click and the window menu are then the system's,
// and the window's own mouse handling never sees those presses.
//
// With `ownFrame` the window also gets back a system frame whose whole area is
// client area (WM_NCCALCSIZE) and draws its title bar itself (TitleBar.qml):
// shadow and rounded corners, resizing at the edges, minimise and maximise
// animations, and Snap Layouts over the maximise button.
//
// The messages are handled in a subclass of the window procedure, ahead of
// Qt: Qt passes mouse messages to native event filters only before they are
// dispatched, without a way to answer them, and the move loop's not at all
// (QTBUG-67095).
class WindowFrame : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    // Where the pointer is over a drag area (window coordinates), or (-1, -1):
    // QML gets no hover events there.
    Q_PROPERTY(QPointF pointer READ pointer NOTIFY pointerChanged FINAL)
    // The maximise button belongs to Windows (Snap Layouts): QML learns
    // from here how to draw it.
    Q_PROPERTY(bool maximizeHovered READ maximizeHovered NOTIFY maximizeButtonChanged FINAL)
    Q_PROPERTY(bool maximizePressed READ maximizePressed NOTIFY maximizeButtonChanged FINAL)
    Q_PROPERTY(QString icon READ icon CONSTANT FINAL) // the program's, for a title bar

public:
    explicit WindowFrame(bool ownFrame, QObject* parent = nullptr);
    ~WindowFrame() override;

    void setWindow(QWindow* window); // created, not shown yet

    // Pressing here drags the window; a double click emits doubleClicked().
    Q_INVOKABLE void addDragArea(QQuickItem* item);
    // Inside a drag area, still the window's own (buttons, text fields).
    Q_INVOKABLE void addControl(QQuickItem* item);
    // The native sizing border in logical pixels; controls beside it keep
    // their hit areas inside this inset, including at fractional scale.
    Q_INVOKABLE qreal resizeMargin(qreal devicePixelRatio) const;
    // Laid over the window while it is shown (an open menu: DropMenu.qml, the
    // editor's key list): the window's own, ahead of everything here (drag
    // areas, the maximise button, resizing edges), so a press on it is its
    // own, not a drag of the window. Kept once, however often it is added;
    // hidden, it counts for nothing.
    Q_INVOKABLE void addPopup(QQuickItem* item);
    // Inside a drag area, dragged it moves the window as the rest does;
    // clicked (let go before the system's drag distance) it emits
    // handleClicked(button) (the launcher's logo). Not a
    // control: QML hears of the pointer over it from `pointer` alone.
    Q_INVOKABLE void addHandle(QQuickItem* item);
    // With ownFrame: hit-tested as the maximise button (Snap Layouts).
    Q_INVOKABLE void setMaximizeButton(QQuickItem* item) { m_maximizeButton = item; }
    // frameShown() once the scene as it is now is on the screen: the first
    // frame synchronised after this call has been swapped (a QQuickWindow's
    // only). Before a native resize that would otherwise show the last frame
    // cut (Main.qml, the strip closing).
    Q_INVOKABLE void afterNextFrame();

    // Shown over another program that keeps the focus (the clipboard, see
    // KeyRouter): a click does not activate the window either, and its frame
    // counts as active all the same, for the Mica backdrop.
    void setNoActivate(bool on);
    // A frameless window that can be resized at its edges meanwhile (the
    // launcher as the search panel): the system's sizing frame, all of it
    // client area as with ownFrame, so nothing looks different.
    void setResizable(bool on);
    bool resizable() const { return m_resizable; }
    // Its left and right edges widen it (the launcher): there, but over a
    // control, hit testing answers HTLEFT / HTRIGHT, so the pointer is the
    // system's sizing arrow; a press emits sideGrabbed (the caller follows
    // the mouse from there, Placement::beginSizing), a double click
    // sideDoubleClicked. No sizing frame (WS_THICKFRAME): with one, Windows
    // would snap the window when it is dragged by its header to a screen's
    // edge. Not while it is resizable (the panel's edges are the system's).
    void setSideSizing(bool on) { m_sideSizing = on; }

    QPointF pointer() const { return m_pointer; }
    bool maximizeHovered() const { return m_maximizeHovered; }
    bool maximizePressed() const { return m_maximizePressed; }
    QString icon() const { return s_icon; }
    // The program's icon as an image URL, set once before any title bar is
    // made: WinShun's from its file icon provider, the screen capture plugin's
    // from its own (no file icons there).
    static void setIcon(const QString& url) { s_icon = url; }

signals:
    void pointerChanged();
    void maximizeButtonChanged();
    void doubleClicked(); // on a drag area
    void handleClicked(Qt::MouseButton button); // see addHandle
    void frameShown(); // see afterNextFrame
    void sideGrabbed(bool left); // see setSideSizing
    void sideDoubleClicked();

private:
    static inline QString s_icon;
    struct Hook; // the window procedure subclass
    friend struct Hook;

    int hitTest(int screenX, int screenY) const; // HT* code
    bool sidesOnly() const { return m_sideSizing && !m_resizable && !m_ownFrame; } // see setSideSizing
    bool onHandle(int screenX, int screenY) const; // a handle under that point (see addHandle)
    QPointF toWindow(int screenX, int screenY) const; // logical pixels
    void setPointer(QPointF pointer);
    void setMaximizeState(bool hovered, bool pressed);
    void trackLeave();

    const bool m_ownFrame;
    QPointer<QWindow> m_window;
    WId m_hwnd = 0;
    QList<QPointer<QQuickItem>> m_dragAreas;
    QList<QPointer<QQuickItem>> m_controls;
    QList<QPointer<QQuickItem>> m_popups;
    QList<QPointer<QQuickItem>> m_handles;
    QPointer<QQuickItem> m_maximizeButton;
    QPointF m_pointer {-1, -1};
    bool m_trackingLeave = false;
    bool m_maximizeHovered = false;
    bool m_maximizePressed = false;
    bool m_noActivate = false;
    bool m_resizable = false;
    bool m_sideSizing = false;
    Qt::MouseButton m_handleButton = Qt::NoButton;
    QPoint m_handlePress;
    // afterNextFrame, set on the render thread: armed, then synchronised.
    std::atomic<bool> m_frameArmed = false;
    std::atomic<bool> m_frameSynced = false;
};

} // namespace ws
