import QtQuick
import WinShun

// A removable tag: "node_modules ×".
Rectangle {
    id: chip

    property string text
    property bool marked: false // has the words the settings are searched for
    readonly property real dpr: Screen.devicePixelRatio // one Screen attached object for all its items (GitHub #111)
    signal removeClicked()

    implicitWidth: Theme.snapUp(label.implicitWidth + 40, chip.dpr) // whole device pixels: the next chip's edge in a Flow
    implicitHeight: 32
    radius: Theme.controlRadius
    color: marked ? Theme.selection : Theme.control
    border.width: 1
    border.color: Theme.controlBorder

    Text {
        id: label
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        text: chip.text
        textFormat: Text.PlainText
        color: Theme.text
        font.pixelSize: Theme.fontBody
    }

    Rectangle {
        anchors.right: parent.right
        anchors.rightMargin: 4
        y: Theme.snap((parent.height - height) / 2, chip.dpr)
        width: 22
        height: 22
        radius: Theme.smallControlRadius
        color: area.pressed ? Theme.controlPressed : area.containsMouse ? Theme.chipHover : "transparent"

        Glyph {
            anchors.centerIn: parent
            glyph: "" // Cancel
            size: 12
            color: area.containsMouse ? Theme.text : Theme.subtext
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.removeClicked()
        }
    }
}
