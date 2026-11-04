import QtQuick

// windowFrame: our own rounded window (radius 18; DWM rounding is off for
// this style, the shadow comes from C++ by Look.windowShadow). paint "back":
// the window's opaque body (the host's `fill`, else Look.background) under
// the content; "edge": the 1 px neutral line over the content, as DWM draws
// its edge; "all": both. Maximized: square, no edge.
Item {
    id: frame

    property QtObject look
    property string paint: "all"
    property color fill: "transparent"
    property bool active: true
    property bool maximized: false
    property real dpr: 1

    readonly property real radius: look ? (maximized ? 0 : look.windowRadius) : 0

    Rectangle {
        visible: frame.paint !== "edge"
        anchors.fill: parent
        radius: frame.radius
        color: frame.look ? (frame.fill.a > 0 ? frame.fill : frame.look.background) : "transparent"
    }
    Rectangle {
        visible: frame.paint !== "back" && !frame.maximized
        anchors.fill: parent
        radius: frame.radius
        color: "transparent"
        border.width: frame.look ? frame.look.edge(frame.dpr) : 0
        border.color: frame.look ? frame.look.windowEdge : "transparent"
    }
}
