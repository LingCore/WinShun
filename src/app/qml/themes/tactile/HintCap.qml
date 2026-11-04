import QtQuick

// A key hint (Ctrl, Enter, Tab): not a button, so only the outline of a
// key in one device pixel; no face, side or shadow. Faint ink.
Rectangle {
    id: hint

    property real dpr: 1
    property QtObject look // injected by Look.qml
    readonly property color foreground: look ? look.ink3 : "transparent"
    // label insets inside the outline (CSS padding 0 4 1 3; text 12 px)
    readonly property real leftPadding: 3
    readonly property real rightPadding: 4
    readonly property real contentOffsetY: -0.5

    radius: look ? look.capRadius : 0
    color: "transparent"
    border.width: look ? look.hair(dpr) : 0
    border.color: look ? look.capLine : "transparent"
}
