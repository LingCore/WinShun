import QtQuick
import WinShun

// A window's frame as the style draws it (Theme.windowFrame, themes/README.md).
// Under DWM's frame (fluent, and any style with square corners) Windows rounds,
// edges and shadows the window, and this draws only the edge Windows 10 lacks.
// An "own" frame (Theme.ownFrame): the window turns see-through, the part
// draws its body in `fill` (the window's colour) with the style's corners and
// its edge over everything, and OwnFrame puts the style's shadow under it.
//
// Every window: `color: <its colour>` stays as it is, and `WindowEdge { fill: <the same colour> }`.
Item {
    id: edge

    // The window's own colour: its body under an own frame.
    property color fill: "transparent"
    readonly property bool maximized: Window.window !== null && (Window.window.visibility === Window.Maximized
                                                                || Window.window.visibility === Window.FullScreen)
    // Corners of the window as drawn now: full-bleed surfaces by a corner
    // (a title bar, a footer, a sidebar) round theirs by this.
    readonly property real cornerRadius: Theme.ownFrame && !maximized ? Theme.frameRadius : 0
    readonly property real dpr: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1

    anchors.fill: parent
    z: 1000

    // See-through under an own frame: the body is the part's.
    Binding {
        target: edge.Window.window
        property: "color"
        value: "transparent"
        when: Theme.ownFrame && edge.Window.window !== null
    }

    Hook {
        parent: edge.parent
        anchors.fill: parent
        z: -1000
        visible: Theme.ownFrame
        part: Theme.ownFrame ? Theme.windowFrame : null
        paint: "back"
        fill: edge.fill
        maximized: edge.maximized
        active: edge.Window.active
        dpr: edge.dpr
    }

    // Under DWM's frame every style's edge is fluent's (Windows 10 only).
    Hook {
        anchors.fill: parent
        part: Theme.ownFrame ? Theme.windowFrame : Theme.base.windowFrame
        paint: "edge"
        fill: edge.fill
        maximized: edge.maximized
        active: edge.Window.active
        dpr: edge.dpr
    }

    OwnFrame { active: Theme.ownFrame; radius: Theme.frameRadius; shadow: Theme.frameShadow }
}
