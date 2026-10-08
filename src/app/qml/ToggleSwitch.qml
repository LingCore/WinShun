import QtQuick
import WinShun

// Windows 11 style on/off switch with an "开 / 关" label. It never changes
// `checked` itself: bind it and handle toggled().
Item {
    id: toggle

    property bool checked
    signal toggled(bool on)

    implicitWidth: row.implicitWidth
    implicitHeight: 32
    activeFocusOnTab: true

    Keys.onSpacePressed: toggle.toggled(!toggle.checked)

    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: 14
            text: toggle.checked ? qsTr("开") : qsTr("关")
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }

        Rectangle {
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            height: 20
            radius: 10
            color: toggle.checked ? Theme.accent : area.containsMouse ? Theme.controlHover : "transparent"
            border.width: toggle.checked ? 0 : 1
            border.color: Theme.subtext

            Rectangle {
                readonly property real size: area.pressed ? 14 : area.containsMouse ? 14 : 12
                anchors.verticalCenter: parent.verticalCenter
                x: toggle.checked ? parent.width - width - 4 : 4
                width: area.pressed ? 17 : size
                height: size
                radius: size / 2
                color: toggle.checked ? Theme.onAccent : Theme.subtext

                Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
            }

            Rectangle { // keyboard focus
                visible: toggle.activeFocus
                anchors.fill: parent
                anchors.margins: -3
                radius: 13
                color: "transparent"
                border.width: 2
                border.color: Theme.text
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: toggle.toggled(!toggle.checked)
    }
}
