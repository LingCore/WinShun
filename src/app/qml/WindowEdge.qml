import QtQuick
import WinShun

// A window's frame is always Windows' own (DWM): its corners, edge and shadow,
// in every 界面风格 (themes/README.md, docs/pitfalls.md). This draws only the
// edge Windows 10 lacks, and under everything a style's own window body
// (Theme.windowFrame, paint "back": tactile's panel), square, where DWM rounds.
//
// Every window: `color: <its colour>` stays as it is, and `WindowEdge { fill: <the same colour> }`.
Item {
    id: edge

    // The window's own colour: a style's body is laid in it.
    property color fill: "transparent"
    // false: no style's body (a menu or a tip: its colour is all it has).
    property bool body: true
    readonly property bool maximized: Window.window !== null && (Window.window.visibility === Window.Maximized
                                                                || Window.window.visibility === Window.FullScreen)
    readonly property real dpr: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1

    anchors.fill: parent
    z: 1000
    // Nothing to draw under DWM's edge on Windows 11, nor maximized (the body
    // Hook below is the window's child, not this one's).
    visible: !SystemTheme.nativeWindowBorder && !maximized

    // A style's window body (fluent's is the window's colour itself: none).
    Hook {
        parent: edge.parent
        anchors.fill: parent
        z: -1000
        visible: part !== null
        part: edge.body && Theme.windowFrame !== Theme.base.windowFrame ? Theme.windowFrame : null
        paint: "back"
        fill: edge.fill
        maximized: edge.maximized
        active: edge.Window.active
        dpr: edge.dpr
    }

    // Windows 10's edge, the same in every style.
    Hook {
        anchors.fill: parent
        part: Theme.base.windowFrame
        paint: "edge"
        fill: edge.fill
        maximized: edge.maximized
        active: edge.Window.active
        dpr: edge.dpr
    }
}
