import QtQuick

// A machined seam: one device pixel of dark line, then light below (or to
// the right). Give it seamThickness (2) across; it only divides areas.
Rectangle {
    id: seam

    property int orientation: Qt.Horizontal
    property real dpr: 1
    property QtObject look // injected by Look.qml
    readonly property real thickness: look ? look.seamThickness : 2
    readonly property real hair: look ? look.hair(dpr) : 1

    color: look ? look.seamLight : "transparent"

    Rectangle {
        width: seam.orientation === Qt.Horizontal ? seam.width : seam.hair
        height: seam.orientation === Qt.Horizontal ? seam.hair : seam.height
        color: seam.look ? (seam.look?.seamDark ?? Qt.rgba(0, 0, 0, 0)) : "transparent"
    }
}
