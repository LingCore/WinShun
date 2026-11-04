import QtQuick

// windowFrame: a window's body (paint "back", under everything) or its edge
// (paint "edge", over everything); "all" both. Under DWM's frame Windows draws the edge and
// the window's colour is its body: only Windows 10 (no edge of DWM's) gets
// one here. An "own" frame (look.frame) draws both, rounded by windowRadius.
Item {
    id: part

    property QtObject look: null
    property string paint: "all" // back | edge | all
    property color fill: "transparent"
    property bool active: true
    property bool maximized: false
    property real dpr: 1

    readonly property bool own: look && look.frame === "own"
    readonly property real radius: maximized || !look ? 0 : look.windowRadius

    Rectangle {
        anchors.fill: parent
        visible: part.paint !== "edge" && part.own
        radius: part.radius
        antialiasing: radius > 0
        color: part.fill
    }

    Rectangle {
        anchors.fill: parent
        visible: part.paint !== "back" && !part.maximized && (part.own || (part.look && !part.look.dwmEdge))
        radius: part.radius
        antialiasing: radius > 0
        color: "transparent"
        border.width: Math.max(1, Math.round(part.dpr)) / part.dpr
        border.color: part.look ? part.look.windowEdge : "transparent"
    }
}
