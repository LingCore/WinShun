pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// One column of the folder menu (FolderMenuWindow), filling its window: the
// rows, the highlight under the one chosen or pointed at, the scroll bar.
// The menu decides what it shows and which row is highlighted; the column
// tells it what the pointer does.
Item {
    id: column

    property var entries: []
    property int highlighted: -1
    // Its highlighted row is the one the next column came from, and the keys
    // are a column further on: drawn fainter.
    property bool faint: false
    property real dpr: 1
    readonly property real padding: Theme.menuPadding
    readonly property int itemHeight: Theme.menuItemHeight
    readonly property int sectionHeight: Theme.menuCaptionSize + 12
    readonly property real listHeight: entries.reduce((height, entry) => height + rowHeight(entry), 0)

    signal pointed(int index, Item row) // the pointer came onto a row, or moved on it
    signal unpointed(int index, Item row) // the pointer went off it
    signal gone(int index, Item row) // a row scrolled away (pooled)
    signal chosen(int index) // a row clicked
    signal expanded(int index) // its arrow clicked
    signal scrolled() // the list moved: the wheel, its bar, or to show its highlighted row

    function rowHeight(entry: var): real {
        return entry.separator === true ? 9 : entry.heading === true ? sectionHeight : itemHeight
    }
    // Where row i's top is on the screen, in Qt's pixels; inside the list
    // when it is scrolled out of sight.
    function rowTop(i: int): real {
        let y = 0
        for (let k = 0; k < i && k < entries.length; ++k)
            y += rowHeight(entries[k])
        y = Math.max(0, Math.min(y - (flick.contentY - flick.originY), flick.height - itemHeight))
        return column.mapToGlobal(0, flick.y + y).y
    }
    // Whether row i is in sight: its middle inside the list.
    function rowInSight(i: int): bool {
        if (i < 0 || i >= entries.length)
            return false
        let y = 0
        for (let k = 0; k < i; ++k)
            y += rowHeight(entries[k])
        const middle = y - (flick.contentY - flick.originY) + rowHeight(entries[i]) / 2
        return middle >= 0 && middle <= flick.height
    }
    function sync() {
        flick.currentIndex = highlighted // the list's highlight follows it
        if (highlighted >= 0) {
            flick.forceLayout()
            flick.positionViewAtIndex(highlighted, ListView.Contain)
        }
    }

    onHighlightedChanged: Qt.callLater(column.sync)
    onEntriesChanged: {
        flick.currentIndex = -1
        flick.contentY = 0
        Qt.callLater(column.sync)
    }

    // The highlight under an item chosen or pointed at (the style's
    // rowHighlight, role "menu"). A part that knows the menu (declares
    // `role`, or `inMenu` as brand's does) draws the item's fill; one that
    // does not (a list's highlight) leaves it to the menu's own.
    component MenuHighlight: Hook {
        id: highlightPart
        readonly property bool inMenu: item !== null && ("role" in item || "inMenu" in item)
        part: Theme.rowHighlight
        hovered: inMenu
        role: "menu"
        dpr: column.dpr

        Rectangle {
            anchors.fill: parent
            visible: !highlightPart.inMenu
            radius: Theme.menuItemRadius
            color: Theme.menuHover
        }
    }

    ListView {
        id: flick
        objectName: "folderMenuScroll"
        x: column.padding
        y: column.padding
        width: column.width - 2 * column.padding
        height: column.height - 2 * column.padding
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        reuseItems: true
        model: column.entries
        onContentYChanged: column.scrolled()
        // One highlight for the list, under the item chosen or pointed at:
        // it jumps, or slides as the style says (Theme.rowSlide). Not
        // managed by the view's own current item: column.highlighted is.
        currentIndex: -1
        highlightMoveDuration: Theme.rowSlide
        highlightMoveVelocity: -1
        highlightResizeDuration: 0
        highlightResizeVelocity: -1
        highlight: Item {
            width: flick.width
            visible: flick.currentItem !== null && column.highlighted >= 0
            MenuHighlight {
                anchors.fill: parent
                opacity: column.faint ? 0.5 : 1
            }
        }
        delegate: Item {
            id: entry
            required property var modelData
            required property int index
            readonly property bool separator: modelData.separator === true
            readonly property bool disabled: modelData.disabled === true
            readonly property bool section: modelData.heading === true
            readonly property bool branch: modelData.children !== undefined || modelData.folder !== undefined
            readonly property string tooltip: modelData.tooltip ?? ""
            readonly property real trailingWidth: Math.min(detail.implicitWidth, 96) + (branch ? 20 : 0)
            objectName: "folderMenuEntry" + index
            width: flick.width
            height: column.rowHeight(modelData)
            enabled: !disabled
            ListView.onPooled: column.gone(entry.index, entry)
            Accessible.role: section ? Accessible.Heading : Accessible.MenuItem
            Accessible.name: modelData.text ?? ""
            Accessible.description: tooltip
            Accessible.ignored: separator
            Accessible.onPressAction: column.chosen(entry.index)
            Hook { // the style's seam, edge to edge
                visible: entry.separator
                anchors.verticalCenter: parent.verticalCenter
                x: -column.padding
                width: column.width
                height: Theme.edge(column.dpr)
                part: Theme.separator
                dpr: column.dpr
            }
            Image {
                id: icon
                visible: !entry.separator && status === Image.Ready
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                source: entry.modelData.icon ?? ""
                sourceSize: Qt.size(18 * column.dpr, 18 * column.dpr)
                asynchronous: true
                fillMode: Image.PreserveAspectFit
            }
            Glyph {
                visible: !entry.separator && !entry.section && !icon.visible
                x: 13
                anchors.verticalCenter: parent.verticalCenter
                glyph: entry.modelData.glyph ?? ""
                size: 16
                color: entry.disabled ? Theme.faint : Theme.text
            }
            Text {
                x: entry.section ? 12 : 42
                width: Math.max(0, entry.width - x - 14 - entry.trailingWidth - 12)
                anchors.verticalCenter: parent.verticalCenter
                text: entry.modelData.text ?? ""
                elide: Text.ElideRight
                color: entry.disabled ? Theme.faint : Theme.text
                font.pixelSize: entry.section ? Theme.menuCaptionSize : Theme.menuTextSize
            }
            Text {
                id: detail
                anchors.right: parent.right
                anchors.rightMargin: entry.branch ? 34 : 14
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, 96)
                text: entry.modelData.detail ?? ""
                elide: Text.ElideRight
                color: Theme.faint
                font.pixelSize: Theme.menuCaptionSize
            }
            Rectangle { // the arrow is a button of its own on a row that goes somewhere itself
                visible: arrow.containsMouse && entry.modelData.action !== undefined
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                height: entry.height - 8
                radius: Theme.menuItemRadius
                color: Theme.controlHover
            }
            Glyph {
                visible: entry.branch
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                glyph: "\uE76C"
                size: 12
                color: entry.disabled ? Theme.faint : Theme.text
            }
            MouseArea {
                anchors.fill: parent
                enabled: !entry.separator && !entry.disabled
                hoverEnabled: true
                onEntered: column.pointed(entry.index, entry)
                onPositionChanged: column.pointed(entry.index, entry)
                onExited: column.unpointed(entry.index, entry)
                onClicked: column.chosen(entry.index)

                MouseArea {
                    id: arrow
                    objectName: "folderMenuArrow" + entry.index
                    anchors.right: parent.right
                    width: 36
                    height: parent.height
                    enabled: entry.branch
                    hoverEnabled: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTranslate("FolderMenuWindow", "展开“%1”").arg(entry.modelData.text ?? "")
                    Accessible.ignored: !entry.branch
                    Accessible.onPressAction: column.expanded(entry.index)
                    // The row's own area stays hovered under it; a move here is the row's too.
                    onPositionChanged: column.pointed(entry.index, entry)
                    onClicked: column.expanded(entry.index)
                }
            }
        }
    }
    ListScrollBar {
        dpr: column.dpr
        visible: flick.contentHeight > flick.height
        flickable: flick
        x: column.width - width
        y: flick.y
        height: flick.height
    }
}
