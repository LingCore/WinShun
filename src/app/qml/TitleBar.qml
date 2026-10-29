pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A Windows 11 title bar as tall as the system's own apps have (48 px): icon,
// title and the caption buttons. Its background is the window's. Windows
// treats the rest of it as the caption (see WindowFrame), so dragging,
// snapping, double-click and the window menu are the system's own.
Item {
    id: bar

    required property WindowFrame frame
    property string title
    property real iconX: 16
    property color foreground: Theme.text
    // The close button asks (closeRequested) instead of closing the window, for a window that
    // empties itself first (EditWindow). Not by refusing the window's closing: that also stops
    // the application from quitting (Qt asks every window to close first).
    property bool closeIntercepted: false

    signal closeRequested()

    readonly property bool maximized: Window.visibility === Window.Maximized
    // Dimmed while the window is inactive, as Windows does.
    readonly property color ink: Window.active ? foreground : Qt.rgba(foreground.r, foreground.g, foreground.b, 0.4)

    height: 48

    Component.onCompleted: {
        frame.addDragArea(bar)
        frame.addControl(buttons)
        frame.setMaximizeButton(maximize)
    }

    Image {
        id: icon

        // On whole device pixels and sampled unsmoothed, so it stays sharp
        // (see ResultRow).
        readonly property real dpr: Screen.devicePixelRatio
        readonly property int pixels: Math.round(16 * dpr)

        x: bar.iconX
        y: Math.round((bar.height - 16) / 2)
        width: pixels / dpr
        height: pixels / dpr
        source: bar.frame.icon
        sourceSize.width: pixels / dpr
        sourceSize.height: pixels / dpr
        smooth: false
        asynchronous: true
    }

    Text {
        anchors.left: icon.right
        anchors.leftMargin: 16
        anchors.right: buttons.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: bar.title
        color: bar.ink
        font.pixelSize: Theme.fontCaption
        elide: Text.ElideRight
    }

    component CaptionButton: Rectangle {
        id: button

        property string glyph
        property bool closes: false
        property bool hovered: area.containsMouse
        property bool pressed: area.pressed
        readonly property bool red: closes && (hovered || pressed)

        signal clicked()

        width: 46
        height: bar.height
        color: red ? (pressed ? Theme.closePressed : Theme.closeHover)
             : pressed ? Qt.rgba(bar.foreground.r, bar.foreground.g, bar.foreground.b, 0.04)
             : hovered ? Qt.rgba(bar.foreground.r, bar.foreground.g, bar.foreground.b, 0.06)
             : "transparent"

        Glyph {
            anchors.centerIn: parent
            glyph: button.glyph
            caption: true
            size: 10
            color: button.red ? (button.pressed ? Qt.rgba(1, 1, 1, 0.7) : "white")
                 : button.pressed ? Qt.rgba(bar.ink.r, bar.ink.g, bar.ink.b, bar.ink.a * 0.7)
                 : bar.ink
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }

    Row {
        id: buttons
        anchors.right: parent.right

        CaptionButton {
            glyph: "" // ChromeMinimize
            onClicked: bar.Window.window.showMinimized()
        }
        CaptionButton {
            id: maximize
            // Its mouse goes to Windows (Snap Layouts): the frame says how to draw it.
            glyph: bar.maximized ? "" : "" // ChromeRestore, ChromeMaximize
            hovered: bar.frame.maximizeHovered
            pressed: bar.frame.maximizePressed
            onClicked: bar.maximized ? bar.Window.window.showNormal() : bar.Window.window.showMaximized()
        }
        CaptionButton {
            glyph: "" // ChromeClose
            closes: true
            onClicked: bar.closeIntercepted ? bar.closeRequested() : bar.Window.window.close()
        }
    }
}
