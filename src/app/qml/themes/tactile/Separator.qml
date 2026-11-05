import QtQuick

// A machined seam: one device pixel of dark line, then light below (or to
// the right). Horizontal: it fills its host across (the host gives it
// seamThickness high). Vertical: seamThickness wide, centred in the host and
// as tall as it, however wide the host is.
Item {
    id: seam

    property int orientation: Qt.Horizontal
    property real dpr: 1
    property QtObject look // injected by Look.qml
    readonly property real thickness: look ? look.seamThickness : 2
    readonly property real hair: look ? look.hair(dpr) : 1
    readonly property bool vertical: orientation === Qt.Vertical

    Rectangle {
        id: light
        x: seam.vertical ? Math.round((seam.width - seam.thickness) / 2 * seam.dpr) / seam.dpr : 0
        width: seam.vertical ? seam.thickness : seam.width
        height: seam.height
        color: seam.look ? seam.look.seamLight : "transparent"

        Rectangle {
            width: seam.vertical ? seam.hair : parent.width
            height: seam.vertical ? parent.height : seam.hair
            color: seam.look ? (seam.look?.seamDark ?? Qt.rgba(0, 0, 0, 0)) : "transparent"
        }
    }
}
