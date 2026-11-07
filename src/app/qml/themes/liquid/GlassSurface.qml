import QtQuick

// One flat surface and an optional edge. Acrylic belongs to the window.
Rectangle {
    property QtObject look: null
    property real dpr: 1
    property color baseColor: "transparent"
    property color edgeColor: look ? look.controlBorder : "transparent"
    readonly property real hair: 1 / Math.max(1, dpr)

    radius: look ? look.controlRadius : 0
    antialiasing: radius > 0
    color: baseColor
    border.width: hair
    border.color: edgeColor
}
