pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import WinShun

GridView {
    id: gallery
    required property Launcher launcher
    property NameTip nameTip: null
    readonly property int columns: Math.max(1, Math.floor(width / (launcher.thumbnailSize + 24)))
    signal rowClicked(int index, int modifiers)
    signal rowDoubleClicked(int index, int modifiers)
    signal rowMenuRequested(int index, point globalPos, int modifiers)
    signal rowDragRequested(int index)

    clip: true
    model: visible ? launcher.results : null
    cellWidth: Math.floor(width * Screen.devicePixelRatio / columns) / Screen.devicePixelRatio
    cellHeight: launcher.thumbnailSize + 76
    boundsBehavior: Flickable.StopAtBounds
    highlightMoveDuration: 0
    keyNavigationEnabled: false
    cacheBuffer: cellHeight
    property int previousColumns: 1
    property int previousCellHeight: 236
    property bool reflowQueued: false
    function stopScrolling() { scroll.cancel(); cancelFlick() }
    onCurrentIndexChanged: positionViewAtIndex(currentIndex, GridView.Contain)
    onContentYChanged: if (nameTip) nameTip.dismiss()
    function preserveViewport() {
        if (reflowQueued)
            return
        const row = Math.max(0, contentY) / previousCellHeight
        const index = Math.floor(row) * previousColumns
        const fraction = row - Math.floor(row)
        reflowQueued = true
        Qt.callLater(() => {
            contentY = Math.max(0, Math.min(contentHeight - height,
                Math.floor(index / columns) * cellHeight + fraction * cellHeight))
            previousColumns = columns
            previousCellHeight = cellHeight
            reflowQueued = false
        })
    }
    onCellHeightChanged: preserveViewport()
    onColumnsChanged: preserveViewport()

    function loadMoreAtEnd() {
        if (visible && atYEnd && count > 0 && launcher.moreAvailable && !launcher.searching)
            launcher.loadMore()
    }
    onAtYEndChanged: Qt.callLater(loadMoreAtEnd)
    Connections {
        target: gallery.launcher
        function onStatusChanged() { Qt.callLater(gallery.loadMoreAtEnd) }
        function onPanelRowRestored(index) { if (index >= 0 && gallery.visible) gallery.currentIndex = index }
    }
    ListWheelScroll {
        id: scroll
        view: gallery
        animate: SystemTheme.animations
        acceptedModifiers: Qt.NoModifier
    }
    WheelHandler {
        target: null
        acceptedModifiers: Qt.AltModifier
        property real carry: 0
        onWheel: event => {
            carry += event.angleDelta.y
            const steps = Math.trunc(carry / 120)
            if (steps !== 0) {
                gallery.stopScrolling()
                gallery.launcher.thumbnailSize += steps * 16
                carry -= steps * 120
            }
        }
    }
    delegate: ResultTile {
        width: gallery.cellWidth
        height: gallery.cellHeight
        nameTip: gallery.nameTip
        onClicked: (index, modifiers) => gallery.rowClicked(index, modifiers)
        onDoubleClicked: (index, modifiers) => gallery.rowDoubleClicked(index, modifiers)
        onMenuRequested: (index, globalPos, modifiers) => gallery.rowMenuRequested(index, globalPos, modifiers)
        onDragRequested: index => gallery.rowDragRequested(index)
    }
}
