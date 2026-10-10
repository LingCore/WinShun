pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Tabs under a settings page's title, splitting a long page into shorter
// ones (SettingsSection.tab), as in Windows 11's SelectorBar: the labels in
// a row, a short accent bar under the current one. ← and → move between
// them while the bar has the keyboard.
Item {
    id: bar

    property var labels: []
    property int current: 0

    signal activated(int index)

    implicitWidth: row.width + row.x
    implicitHeight: 40
    activeFocusOnTab: true

    Keys.onLeftPressed: if (current > 0) activated(current - 1)
    Keys.onRightPressed: if (current < labels.length - 1) activated(current + 1)

    Row {
        id: row
        x: -12 // the first label under the page's title
        height: parent.height

        Repeater {
            id: items
            model: bar.labels

            delegate: Item {
                id: item

                required property int index
                required property string modelData
                readonly property bool selected: index === bar.current

                // Room for the label in bold, so that it keeps its place
                // when it becomes the current one. Whole pixels: a half one
                // would leave the labels to its right blurred.
                width: Math.ceil(bold.width) + 24
                height: row.height

                TextMetrics {
                    id: bold
                    text: item.modelData
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -2 // clear of the bar under it
                    text: item.modelData
                    color: item.selected || area.containsMouse && !area.pressed ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontBody
                    font.weight: item.selected ? Font.DemiBold : Font.Normal
                }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bar.activated(item.index)
                }
            }
        }
    }

    Rectangle { // under the current label, sliding between them as the categories' mark does
        readonly property Item target: items.count > bar.current ? items.itemAt(bar.current) : null

        visible: target !== null
        x: target ? row.x + target.x + Math.round((target.width - width) / 2) : 0
        y: parent.height - height - 2
        width: 16
        height: 3
        radius: 1.5
        color: Theme.accent

        Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    }

    Rectangle { // keyboard focus, around the current label
        readonly property Item target: items.count > bar.current ? items.itemAt(bar.current) : null

        visible: bar.activeFocus && target !== null
        x: target ? row.x + target.x : 0
        y: 2
        width: target ? target.width : 0
        height: parent.height - 4
        radius: 4
        color: "transparent"
        border.width: 2
        border.color: Theme.text
    }
}
