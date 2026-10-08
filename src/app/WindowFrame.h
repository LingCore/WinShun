#pragma once

#include <QList>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QtGui/qwindowdefs.h>
#include <QtQml/qqmlregistration.h>

class QWindow;

namespace ws {

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
    // With ownFrame: hit-tested as the maximise button (Snap Layouts).
    Q_INVOKABLE void setMaximizeButton(QQuickItem* item) { m_maximizeButton = item; }

    QPointF pointer() const { return m_pointer; }
    bool maximizeHovered() const { return m_maximizeHovered; }
    bool maximizePressed() const { return m_maximizePressed; }
    QString icon() const;

signals:
    void pointerChanged();
    void maximizeButtonChanged();
    void doubleClicked(); // on a drag area

private:
    struct Hook; // the window procedure subclass
    friend struct Hook;

    int hitTest(int screenX, int screenY) const; // HT* code
    QPointF toWindow(int screenX, int screenY) const; // logical pixels
    void setPointer(QPointF pointer);
    void setMaximizeState(bool hovered, bool pressed);
    void trackLeave();

    const bool m_ownFrame;
    QPointer<QWindow> m_window;
    WId m_hwnd = 0;
    QList<QPointer<QQuickItem>> m_dragAreas;
    QList<QPointer<QQuickItem>> m_controls;
    QPointer<QQuickItem> m_maximizeButton;
    QPointF m_pointer {-1, -1};
    bool m_trackingLeave = false;
    bool m_maximizeHovered = false;
    bool m_maximizePressed = false;
};

} // namespace ws
