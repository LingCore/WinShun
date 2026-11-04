import QtQuick

// separator: a 1 px neutral line (whole device pixels), centred in its host
// box: across it when horizontal, down it when vertical.
Item {
    id: sep

    property QtObject look
    property int orientation: Qt.Horizontal
    property real dpr: 1

    readonly property real thickness: look.edge(dpr)
    readonly property bool vertical: orientation === Qt.Vertical

    Rectangle {
        x: sep.vertical ? sep.look.snap((sep.width - width) / 2, sep.dpr) : 0
        y: sep.vertical ? 0 : sep.look.snap((sep.height - height) / 2, sep.dpr)
        width: sep.vertical ? sep.thickness : sep.width
        height: sep.vertical ? sep.height : sep.thickness
        color: sep.look.line
    }
}
