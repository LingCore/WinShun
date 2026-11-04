import QtQuick

// A key hint (Ctrl, Enter, Tab): not a button, so only the outline of a
// key in one device pixel; no face, side or shadow. Faint ink.
Rectangle {
    id: hint

    property real dpr: 1
    property QtObject look // injected by Look.qml
    readonly property color foreground: look.ink3
    // label insets inside the outline (CSS padding 0 4 1 3; text 12 px)
    readonly property real leftPadding: 3
    readonly property real rightPadding: 4
    readonly property real contentOffsetY: -0.5

    radius: look.capRadius
    color: "transparent"
    border.width: look.hair(dpr)
    border.color: look.capLine
}
