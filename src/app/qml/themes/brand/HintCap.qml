import QtQuick

// keyHint: a key that is only a hint (KeyCap, KeyHint), not a button: a fine
// 1 px neutral outline, radius 5, no fill, no depth. Words in the second grey.
Item {
    id: cap

    property QtObject look
    property real dpr: 1

    readonly property real radius: look.keyHintRadius
    readonly property color ink: look.ink2
    readonly property real horizontalPadding: 6
    // Hook.out names.
    readonly property color foreground: ink
    readonly property real leadingInset: horizontalPadding
    readonly property real contentOffsetY: 0

    Rectangle {
        anchors.fill: parent
        radius: cap.radius
        color: "transparent"
        border.width: cap.look.edge(cap.dpr)
        border.color: cap.look.capEdge
    }
}
