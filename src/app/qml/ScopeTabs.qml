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
//
// The first tab (全部) opens the same way to a plugin's command while its
// keyword has the list: 全部 › [icon] 结束进程, with `pluginLabel` and
// `pluginIcon` (an image URL); the chip is current as long as it shows,
// and clicking it is pluginClicked (the launcher leaves the keyword).
Rectangle {
    id: tabs

    property int current: 0
    property bool contentTab: true // the launcher's: 内容 is there (Launcher.contentEnabled)
    property var labels: contentTab ? [qsTr("全部"), qsTr("文件"), qsTr("内容")] : [qsTr("全部"), qsTr("文件")]
    property int subOf: -1
    property int subValue: -1
    property string subLabel
    property string pluginLabel
    property string pluginIcon
    // Tabs shown but not to be chosen now (their indices), greyed: the
    // settings' choices that mean nothing at the moment.
    property var disabled: []
    property bool multiSelect: false
    property var checkedIndices: []

    signal activated(int scope)
    signal toggled(int index, bool checked)
    signal pluginClicked()

    // Read by all the tabs: one Screen attached object, not one per item
    // (GitHub #111; docs/pitfalls.md); a host that has the window's passes it.
    property real dpr: Screen.devicePixelRatio
    // Where it stands, for the style's segmented part (Hook.role): "scope" in
    // the launcher's search field; "" elsewhere (the settings' choices).
    property string role
    // One choice: the style's segmented part draws all but the words (the
    // checkboxes of multiSelect keep their own chips).
    readonly property bool segmentedPart: !multiSelect
    property int hoveredIndex: -1
    property int pressedIndex: -1
    property var segments: [] // [{x, width}] of each tab in the part, laid out here
    function placeSegments() {
        const list = []
        for (let i = 0; i < tabRepeater.count; ++i) {
            const tab = tabRepeater.itemAt(i)
            if (tab)
                list.push({ x: row.x + tab.x, width: tab.width })
        }
        segments = list
    }

    // Labels on whole device pixels (see SearchBar.onPixel): centred, at 150 %
    // they sat on half ones, and came out a pixel taller on some frames while
    // the launcher slid in.
    function onPixel(v) { return Math.round(v * tabs.dpr) / tabs.dpr }


    // In the launcher: as large as 界面大小 says (Theme.tabHeight); in the
    // settings window, as the rest there.
    property bool scaled: false
    readonly property int tabHeight: scaled ? Theme.tabHeight : 32
    // The tabs' own height: lower when a host holds it lower than it would be
    // (the keys in a style's inset search field).
    readonly property real shownTabHeight: Math.min(tabHeight, height - 6)
    readonly property int bodySize: scaled ? Theme.uiBody : Theme.fontBody
    readonly property int captionSize: scaled ? Theme.uiCaption : Theme.fontCaption

    implicitWidth: row.implicitWidth + 6
    implicitHeight: tabHeight + 6
    radius: Theme.largeCardRadius
    color: segmentedPart ? "transparent" : Theme.track

    Hook { // over the tabs' row, not the margin around it: a hover fill as tall as a tab
        id: track
        y: row.y
        width: parent.width
        height: tabs.shownTabHeight
        visible: tabs.segmentedPart
        part: tabs.segmentedPart ? Theme.segmented : null
        count: tabs.labels.length
        currentIndex: {
            for (let i = 0; i < tabs.labels.length; ++i) {
                if (i === tabs.current || (i === tabs.subOf && tabs.subLabel.length > 0 && tabs.current === tabs.subValue))
                    return i
            }
            return -1
        }
        hoveredIndex: tabs.hoveredIndex
        pressedIndex: tabs.pressedIndex
        segments: tabs.segments
        role: tabs.role
        dpr: tabs.dpr
    }

    Row {
        id: row
        x: tabs.onPixel((tabs.width - width) / 2)
        y: tabs.onPixel((tabs.height - height) / 2)
        spacing: 2
        onPositioningComplete: tabs.placeSegments()
        onXChanged: tabs.placeSegments()

        Repeater {
            id: tabRepeater
            model: tabs.labels

            delegate: Rectangle {
                id: tab

                required property int index
                required property string modelData
                readonly property bool isChecked: tabs.multiSelect && tabs.checkedIndices.indexOf(index) >= 0
                readonly property bool hasSub: index === tabs.subOf && tabs.subLabel.length > 0
                readonly property bool subCurrent: hasSub && tabs.current === tabs.subValue
                readonly property bool active: tabs.multiSelect ? isChecked : (index === tabs.current || subCurrent)
                readonly property bool open: hasSub && active
                // The plugin's chip: in the first tab, while it is current.
                readonly property bool hasPlugin: index === 0 && tabs.pluginLabel.length > 0
                readonly property bool pluginOpen: hasPlugin && tabs.current === 0
                readonly property bool off: tabs.disabled.length > 0 && tabs.disabled.indexOf(index) >= 0
                // Whole device pixels (at 150 % a whole logical one can still be half): a half one would leave the labels to its right blurred.
                readonly property real labelWidth: Theme.snapUp(label.implicitWidth + (tabs.multiSelect ? 38 : 24), tabs.dpr)

                width: labelWidth + (open ? sub.fullWidth : 0) + (pluginOpen ? pluginSub.fullWidth : 0)
                height: tabs.shownTabHeight
                radius: Theme.largeControlRadius
                clip: true
                // One choice: the segmented part draws the chip under the words.
                color: tabs.segmentedPart ? "transparent" : active ? Theme.chipActive : area.containsMouse ? Theme.chipHover : "transparent"
                border.width: active && !tabs.segmentedPart ? Theme.edge(tabs.dpr) : 0
                border.color: Theme.chipBorder
                Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                onWidthChanged: tabs.placeSegments()
                onXChanged: tabs.placeSegments()

                Glyph {
                    visible: tabs.multiSelect
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: tab.isChecked ? "\uE73E" : "\uE739" // CheckMark / Checkbox
                    size: 12
                    color: tab.off ? Theme.faint : tab.isChecked ? Theme.accent : Theme.subtext
                }

                Text {
                    id: label
                    // The smaller tab (or the plugin's chip) current: this one is the way back to the whole.
                    readonly property color ownColor: tab.off ? Theme.faint : tab.active && !tab.subCurrent && !tab.pluginOpen ? Theme.text : Theme.subtext
                    readonly property int ownWeight: tab.active ? Theme.fontStrong : Font.Normal
                    x: tabs.multiSelect ? 26 : 12
                    y: tabs.onPixel((tab.height - height) / 2 + (tabs.segmentedPart ? track.call("labelOffsetY", [tab.index], 0) : 0))
                    text: tab.modelData
                    color: tab.off || !tabs.segmentedPart || tab.subCurrent || tab.pluginOpen ? ownColor : track.call("labelColor", [tab.index], ownColor)
                    font.pixelSize: tabs.bodySize
                    font.weight: tabs.segmentedPart ? track.call("labelWeight", [tab.index], ownWeight) : ownWeight
                }

                MouseArea {
                    id: area
                    width: tab.labelWidth
                    height: parent.height
                    enabled: !tab.off
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tabs.multiSelect ? tabs.toggled(tab.index, !tab.isChecked) : tabs.activated(tab.index)
                    onContainsMouseChanged: {
                        if (containsMouse)
                            tabs.hoveredIndex = tab.index
                        else if (tabs.hoveredIndex === tab.index)
                            tabs.hoveredIndex = -1
                    }
                    onPressedChanged: {
                        if (pressed)
                            tabs.pressedIndex = tab.index
                        else if (tabs.pressedIndex === tab.index)
                            tabs.pressedIndex = -1
                    }
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
                        height: tabs.tabHeight - 8
                        radius: Theme.controlRadius
                        color: tab.subCurrent ? Theme.accentFill : subArea.containsMouse ? Theme.chipHover : "transparent"

                        Text {
                            id: subText
                            x: tabs.onPixel((subChip.width - width) / 2)
                            y: tabs.onPixel((subChip.height - height) / 2)
                            text: tabs.subLabel
                            color: tab.subCurrent ? Theme.accentText : subArea.containsMouse ? Theme.text : Theme.subtext
                            font.pixelSize: tabs.captionSize
                            font.weight: tab.subCurrent ? Theme.fontStrong : Font.Normal
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

                // 全部 › [icon] 结束进程: the plugin command whose keyword has the list.
                Item {
                    id: pluginSub

                    readonly property int fullWidth: pluginChevron.width + pluginChip.width + 6

                    visible: tab.hasPlugin
                    x: tab.labelWidth - 8
                    width: fullWidth
                    height: parent.height
                    opacity: tab.pluginOpen ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 150 } }

                    Glyph {
                        id: pluginChevron
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        glyph: "" // ChevronRight
                        size: 10
                        color: Theme.faint
                    }

                    Rectangle { // current as long as it shows: the accent, as the smaller tab when current
                        id: pluginChip
                        anchors.left: pluginChevron.right
                        anchors.leftMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8 + pluginIconSlot.width + 6 + Math.ceil(pluginText.implicitWidth) + 8
                        height: tabs.tabHeight - 8
                        radius: Theme.controlRadius
                        color: Theme.accentFill

                        Item {
                            id: pluginIconSlot
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16

                            Image { // as a row's icon (ResultRow.qml): its own device pixels, unsmoothed
                                readonly property real dpr: tabs.dpr
                                readonly property int pixels: Math.round(pluginIconSlot.width * dpr)

                                x: 1 / 64
                                y: 1 / 64
                                width: pixels / dpr
                                height: pixels / dpr
                                source: tabs.pluginIcon
                                sourceSize.width: pixels / dpr
                                sourceSize.height: pixels / dpr
                                smooth: false
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                            }
                        }

                        Text {
                            id: pluginText
                            x: 8 + pluginIconSlot.width + 6
                            y: tabs.onPixel((pluginChip.height - height) / 2)
                            text: tabs.pluginLabel
                            color: Theme.accentText
                            font.pixelSize: tabs.captionSize
                            font.weight: Theme.fontStrong
                        }

                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -4
                            enabled: tab.pluginOpen
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: tabs.pluginClicked()
                        }
                    }
                }
            }
        }
    }
}
