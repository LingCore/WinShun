pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Tabs under a settings page's title, splitting a long page into shorter
// ones (SettingsSection.tab), as in Windows 11's SelectorBar: the labels in
// a row, a short accent bar under the current one. ← and → move between
// them while the bar has the keyboard. The style's segmented part
// (Theme.segmented, role "tabs") draws the rest; fluent's is that
// SelectorBar, another style's a track with a knob.
Item {
    id: bar

    property var labels: []
    property int current: 0
    // Read once for the labels and the part (GitHub #111; docs/pitfalls.md).
    readonly property real dpr: Screen.devicePixelRatio
    // Where the first label starts, from the bar's left: fluent's labels have
    // no track, so the first one stands under the page's title (-12 for its
    // padding); a style with a track starts it at the cards' edge.
    readonly property real inset: Theme.tok("pageTabsInset", -12)
    property int hoveredIndex: -1
    property int pressedIndex: -1
    property var segments: [] // [{x, width}] of each label in the part, laid out here

    function placeSegments() {
        const list = []
        for (let i = 0; i < items.count; ++i) {
            const item = items.itemAt(i)
            if (item)
                list.push({ x: row.x + item.x, width: item.width })
        }
        segments = list
    }

    signal activated(int index)

    implicitWidth: row.width + row.x
    implicitHeight: 40
    activeFocusOnTab: true

    Keys.onLeftPressed: if (current > 0) activated(current - 1)
    Keys.onRightPressed: if (current < labels.length - 1) activated(current + 1)

    Hook { // under the labels: a hover fill, the mark of the current one
        id: track
        y: 2
        width: bar.width
        height: bar.height - 3
        part: Theme.segmented
        count: bar.labels.length
        currentIndex: bar.current
        hoveredIndex: bar.hoveredIndex
        pressedIndex: bar.pressedIndex
        segments: bar.segments
        role: "tabs"
        dpr: bar.dpr
    }

    Row {
        id: row
        x: bar.inset
        height: parent.height
        onPositioningComplete: bar.placeSegments()
        onXChanged: bar.placeSegments()

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
                onWidthChanged: bar.placeSegments()
                onXChanged: bar.placeSegments()

                TextMetrics {
                    id: bold
                    text: item.modelData
                    font.pixelSize: Theme.fontBody
                    font.weight: Theme.fontStrong
                }
                Text {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -2 + track.call("labelOffsetY", [item.index], 0) // clear of the bar under it
                    text: item.modelData
                    color: track.call("labelColor", [item.index],
                                      item.selected || area.containsMouse && !area.pressed ? Theme.text : Theme.subtext)
                    font.pixelSize: Theme.fontBody
                    font.weight: track.call("labelWeight", [item.index], item.selected ? Theme.fontStrong : Font.Normal)
                }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bar.activated(item.index)
                    onContainsMouseChanged: {
                        if (containsMouse)
                            bar.hoveredIndex = item.index
                        else if (bar.hoveredIndex === item.index)
                            bar.hoveredIndex = -1
                    }
                    onPressedChanged: {
                        if (pressed)
                            bar.pressedIndex = item.index
                        else if (bar.pressedIndex === item.index)
                            bar.pressedIndex = -1
                    }
                }
            }
        }
    }

    Rectangle { // keyboard focus, around the current label
        readonly property Item target: items.count > bar.current ? items.itemAt(bar.current) : null

        visible: bar.activeFocus && target !== null
        x: target ? row.x + target.x : 0
        y: 2
        width: target ? target.width : 0
        height: parent.height - 4
        radius: Theme.controlRadius
        color: "transparent"
        border.width: 2
        border.color: Theme.text
    }
}
