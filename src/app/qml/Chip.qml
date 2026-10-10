import QtQuick
import WinShun

// A removable tag: "node_modules ×".
Rectangle {
    id: chip

    property string text
    property bool marked: false // has the words the settings are searched for
    signal removeClicked()

    implicitWidth: label.implicitWidth + 40
    implicitHeight: 32
    radius: 4
    color: Theme.control
    border.width: marked ? 2 : 1
    border.color: marked ? Theme.accent : Theme.controlBorder

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
        anchors.verticalCenter: parent.verticalCenter
        width: 22
        height: 22
        radius: 3
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
