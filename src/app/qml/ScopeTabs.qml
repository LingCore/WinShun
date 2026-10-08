pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Segmented control: 全部 / 文件 / 内容. Tab and Shift+Tab switch too.
// Also used in the settings window with other labels.
Rectangle {
    id: tabs

    property int current: 0
    property var labels: [qsTr("全部"), qsTr("文件"), qsTr("内容")]

    signal activated(int scope)

    implicitWidth: row.implicitWidth + 6
    implicitHeight: 38
    radius: 8
    color: Theme.track

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2

        Repeater {
            model: tabs.labels

            delegate: Rectangle {
                id: tab

                required property int index
                required property string modelData
                readonly property bool active: index === tabs.current

                width: label.implicitWidth + 24
                height: 32
                radius: 6
                color: active ? Theme.chipActive : area.containsMouse ? Theme.chipHover : "transparent"
                border.width: active ? 1 : 0
                border.color: Theme.chipBorder

                Text {
                    id: label
                    anchors.centerIn: parent
                    text: tab.modelData
                    color: tab.active ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontBody
                    font.weight: tab.active ? Font.DemiBold : Font.Normal
                }

                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tabs.activated(tab.index)
                }
            }
        }
    }
}
