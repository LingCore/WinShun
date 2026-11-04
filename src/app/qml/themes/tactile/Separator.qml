import QtQuick

// A machined seam: one device pixel of dark line, then light below (or to
// the right). Give it seamThickness (2) across; it only divides areas.
Rectangle {
    id: seam

    property int orientation: Qt.Horizontal
    property real dpr: 1
    property QtObject look // injected by Look.qml
    readonly property real thickness: look.seamThickness

    color: look.seamLight

    Rectangle {
        width: seam.orientation === Qt.Horizontal ? seam.width : seam.look.hair(seam.dpr)
        height: seam.orientation === Qt.Horizontal ? seam.look.hair(seam.dpr) : seam.height
        color: seam.look.seamDark
    }
}
