import QtQuick

// windowFrame: Windows 10's window edge (paint "edge"), which DWM draws on
// Windows 11. The window's body is its colour: fluent draws none (paint
// "back" is nothing). A style may draw a body (tactile's panel), never the
// corners: those are DWM's (WindowEdge).
Item {
    id: part

    property QtObject look: null
    property string paint: "all" // back | edge | all
    property color fill: "transparent"
    property bool active: true
    property bool maximized: false
    property real dpr: 1
    // DWM draws this window's edge: the Look's, or the window's own (Hook
    // extra; WindowEdge.nativeEdge, the search box without its shadow).
    property bool nativeEdge: look !== null && look.dwmEdge

    Rectangle {
        anchors.fill: parent
        visible: part.paint !== "back" && !part.maximized && part.look !== null && !part.nativeEdge
        // Square: drawn only where DWM rounds nothing (Windows 10, a window without its shadow).
        radius: 0
        color: "transparent"
        border.width: Math.max(1, Math.round(part.dpr)) / part.dpr
        border.color: part.look ? part.look.windowEdge : "transparent"
    }
}
