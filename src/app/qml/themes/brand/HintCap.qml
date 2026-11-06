import QtQuick

// keyHint: a key that is only a hint (KeyCap, KeyHint), not a button: a fine
// 1 px neutral outline, radius 5, no fill, no depth. Words in the second grey.
Item {
    id: cap

    property QtObject look
    property real dpr: 1

    readonly property real radius: look ? look.keyHintRadius : 0
    readonly property color ink: look ? look.ink2 : "transparent"
    readonly property real horizontalPadding: 6
    // Hook.out names.
    readonly property color foreground: ink
    readonly property real leadingInset: horizontalPadding
    readonly property real contentOffsetY: 0

    Rectangle {
        anchors.fill: parent
        radius: cap.radius
        color: "transparent"
        border.width: cap.look ? cap.look.edge(cap.dpr) : 0
        border.color: cap.look ? cap.look.capEdge : "transparent"
    }
}
