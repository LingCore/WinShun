pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Segmented control: 全部 / 文件 / 内容. Tab and Shift+Tab switch too.
// Also used in the settings window with other labels.
//
// A tab can hold a smaller one, shown in it while either is current: 文件
// opens to 文件 › 文件夹. `subOf` is the tab's index, `subValue` what
// activated() gives for the smaller one (Launcher.Folders); clicking it
// again goes back to its tab.
Rectangle {
    id: tabs

    property int current: 0
    property var labels: [qsTr("全部"), qsTr("文件"), qsTr("内容")]
    property int subOf: -1
    property int subValue: -1
    property string subLabel

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
                readonly property bool hasSub: index === tabs.subOf && tabs.subLabel.length > 0
                readonly property bool subCurrent: hasSub && tabs.current === tabs.subValue
                readonly property bool active: index === tabs.current || subCurrent
                readonly property bool open: hasSub && active
                // Whole pixels: a half one would leave the labels to its right blurred.
                readonly property int labelWidth: Math.ceil(label.implicitWidth) + 24

                width: labelWidth + (open ? sub.fullWidth : 0)
                height: 32
                radius: 6
                clip: true
                color: active ? Theme.chipActive : area.containsMouse ? Theme.chipHover : "transparent"
                border.width: active ? 1 : 0
                border.color: Theme.chipBorder
                Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

                Text {
                    id: label
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: tab.modelData
                    // The smaller tab current: this one is the way back to the whole.
                    color: tab.active && !tab.subCurrent ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontBody
                    font.weight: tab.active ? Font.DemiBold : Font.Normal
                }

                MouseArea {
                    id: area
                    width: tab.labelWidth
                    height: parent.height
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tabs.activated(tab.index)
                }

                // 文件 › 文件夹: a chevron, then the smaller tab.
                Item {
                    id: sub

                    readonly property int fullWidth: chevron.width + subChip.width + 6

                    visible: tab.hasSub
                    x: tab.labelWidth - 8
                    width: fullWidth
                    height: parent.height
                    opacity: tab.open ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 150 } }

                    Glyph {
                        id: chevron
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        glyph: "\uE76C" // ChevronRight
                        size: 10
                        color: Theme.faint
                    }

                    Rectangle {
                        id: subChip
                        anchors.left: chevron.right
                        anchors.leftMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.ceil(subText.implicitWidth) + 16
                        height: 24
                        radius: 4
                        color: tab.subCurrent ? Theme.accent : subArea.containsMouse ? Theme.chipHover : "transparent"

                        Text {
                            id: subText
                            anchors.centerIn: parent
                            text: tabs.subLabel
                            color: tab.subCurrent ? Theme.accentText : subArea.containsMouse ? Theme.text : Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            font.weight: tab.subCurrent ? Font.DemiBold : Font.Normal
                        }

                        MouseArea {
                            id: subArea
                            anchors.fill: parent
                            anchors.margins: -4 // easier to hit than the chip itself
                            enabled: tab.open
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: tabs.activated(tab.subCurrent ? tab.index : tabs.subValue)
                        }
                    }
                }
            }
        }
    }
}
