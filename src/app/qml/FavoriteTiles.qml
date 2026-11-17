pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The favorites as tiles (Settings::favoriteTiles, feedback 2026-11-12 and
// -15): with nothing typed in 全部, an icon each with its name under it, as
// many to a line as fit, over the recent rows. Each tile is a row of the
// list (the first Launcher.tileRows ones, which the list leaves out), so the
// mouse here does what it does on a row (the window's rowClicked and the
// rest), the keyboard's current row and the picked ones show on them, and
// ← → ↑ ↓ go along the grid (Main.qml). Held a moment, a tile comes off and
// goes where it is dragged, the others making room; let go there, the
// favorites keep that order (Favorites::move). More than three lines: the
// wheel moves them by whole lines.
Item {
    id: block

    required property Launcher launcher
    property real dpr: 1 // the window's, passed by the launcher (GitHub #111)
    // Where the block's top is in the window, for tiles on whole device pixels.
    property real viewY: 0
    property int currentRow: -1 // the list's current row: a tile when one of them
    property int selectedCount: 0 // changes as rows are picked: the tiles ask again

    signal clicked(int index, int modifiers)
    signal doubleClicked(int index, int modifiers)
    signal menuRequested(int index, point globalPos, int modifiers)
    signal previewRequested(int index)
    signal hoverMoved(int index, point globalPos)

    // Tiles, icons and words on multiples of 4 logical pixels: whole device
    // pixels at 125-200 % (as the 应用 grid's).
    readonly property int tileHeight: 76
    readonly property int iconSize: 32
    readonly property int maxLines: 3
    readonly property int count: tiles.count
    readonly property int columns: Math.max(1, Math.floor((width - 32) / 88))
    readonly property int tileWidth: Math.max(8, Math.floor((width - 32) / columns / 8) * 8)
    readonly property int sideMargin: Math.max(0, Math.floor((width - columns * tileWidth) / 8) * 4)
    readonly property int lines: Math.ceil(count / columns)
    readonly property int shownLines: Math.min(lines, maxLines)

    implicitHeight: count > 0 ? Theme.labelRowHeight + shownLines * tileHeight + 4 : 0

    // Dragging: the tile held, and the place it would go to.
    property int dragFrom: -1
    property int dropAt: -1
    property point dragPoint // the held tile's top left, in the tiles' area
    property int firstLine: 0 // scrolled by whole lines
    onLinesChanged: firstLine = Math.max(0, Math.min(firstLine, lines - shownLines))
    onCurrentRowChanged: reveal(currentRow)

    function reveal(index) { // its line in view
        if (index < 0 || index >= count)
            return
        const line = Math.floor(index / columns)
        if (line < firstLine)
            firstLine = line
        else if (line >= firstLine + shownLines)
            firstLine = line - shownLines + 1
    }
    function tileAt(index) { // the tile of a row, for what is put over it (renaming)
        return tiles.itemAt(index)
    }
    function slotAt(x, y) { // the place under a point of the tiles' area
        const column = Math.max(0, Math.min(columns - 1, Math.floor((x - sideMargin) / tileWidth)))
        const line = Math.max(0, Math.floor(y / tileHeight))
        return Math.max(0, Math.min(count - 1, line * columns + column))
    }
    function slotOf(index) { // where a tile stands, the others making room for the one dragged
        if (dragFrom < 0 || dropAt < 0)
            return index
        if (dragFrom < dropAt && index > dragFrom && index <= dropAt)
            return index - 1
        if (dragFrom > dropAt && index >= dropAt && index < dragFrom)
            return index + 1
        return index
    }

    Text {
        x: 20
        height: Theme.labelRowHeight
        verticalAlignment: Text.AlignVCenter
        text: qsTr("收藏")
        color: Theme.subtext
        font.pixelSize: Theme.uiCaption
    }

    Item {
        id: area
        y: Theme.labelRowHeight
        width: parent.width
        height: block.shownLines * block.tileHeight
        clip: block.lines > block.maxLines

        property real wheelRest: 0
        WheelHandler {
            target: null
            enabled: block.lines > block.maxLines
            acceptedModifiers: Qt.NoModifier
            onWheel: (event) => {
                area.wheelRest += event.angleDelta.y
                const steps = area.wheelRest > 0 ? Math.floor(area.wheelRest / 120) : Math.ceil(area.wheelRest / 120)
                if (steps === 0)
                    return
                area.wheelRest -= steps * 120
                block.firstLine = Math.max(0, Math.min(block.lines - block.shownLines, block.firstLine - steps))
            }
        }

        Item {
            id: content
            y: -block.firstLine * block.tileHeight
            width: parent.width
            height: block.lines * block.tileHeight

            Repeater {
                id: tiles
                model: block.launcher.favoriteTiles

                delegate: Item {
                    id: tile

                    required property int index
                    required property string name
                    required property string path
                    required property string icon
                    required property bool missing
                    readonly property bool held: block.dragFrom === index
                    readonly property bool current: block.currentRow === index
                    readonly property bool picked: block.selectedCount > 0 && block.launcher.results.isSelected(index)
                    readonly property int slot: block.slotOf(index)

                    width: block.tileWidth
                    height: block.tileHeight
                    x: held ? block.dragPoint.x : block.sideMargin + (slot % block.columns) * block.tileWidth
                    y: held ? block.dragPoint.y : Math.floor(slot / block.columns) * block.tileHeight
                    z: held ? 1 : 0
                    Behavior on x { enabled: block.dragFrom >= 0 && !tile.held; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                    Behavior on y { enabled: block.dragFrom >= 0 && !tile.held; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                    // Vertically on whole device pixels too (see the rows').
                    transform: Translate {
                        y: {
                            if (tile.held)
                                return 0
                            const deviceY = (block.viewY + area.y + content.y + tile.y) * block.dpr
                            return (Math.round(deviceY) - deviceY) / block.dpr
                        }
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: tile.name
                    Accessible.description: tile.path
                    Accessible.onPressAction: block.clicked(tile.index, Qt.NoModifier)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: Theme.largeControlRadius
                        color: tile.held || tile.current || tile.picked ? Theme.selection
                             : tileArea.containsMouse && block.dragFrom < 0 ? Theme.hover : "transparent"
                        border.width: tile.held ? Theme.edge(block.dpr) : 0
                        border.color: Theme.windowEdge
                    }
                    Rectangle { // accent pill on the current one, as on the current row
                        visible: tile.current && !tile.held
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: parent.height - 6
                        width: 18
                        height: 3
                        radius: Theme.pillRadius(height)
                        color: Theme.accentLine
                    }
                    Image { // as a row's icon (ResultRow.qml): its own pixels, unsmoothed
                        readonly property int pixels: Math.round(block.iconSize * block.dpr)

                        x: (tile.width - block.iconSize) / 2 + 1 / 64
                        y: 10 + 1 / 64
                        width: pixels / block.dpr
                        height: pixels / block.dpr
                        source: tile.icon
                        sourceSize.width: pixels / block.dpr
                        sourceSize.height: pixels / block.dpr
                        smooth: false
                        asynchronous: true
                        fillMode: Image.PreserveAspectFit
                        opacity: tile.missing ? 0.45 : 1
                    }
                    Text {
                        x: 6
                        y: 48
                        width: parent.width - 12
                        horizontalAlignment: Text.AlignHCenter
                        text: tile.name
                        textFormat: Text.PlainText
                        color: tile.missing ? Theme.faint : Theme.text
                        font.pixelSize: Theme.uiCaption
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }
                    MouseArea {
                        id: tileArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                        pressAndHoldInterval: 350
                        property point grip // where in the tile it was held
                        property bool wasHeld // the release ends a drag, it opens nothing
                        onPressed: wasHeld = false
                        onPressAndHold: (mouse) => {
                            if (mouse.button !== Qt.LeftButton || block.count < 2)
                                return
                            wasHeld = true
                            grip = Qt.point(mouse.x, mouse.y)
                            block.dragPoint = Qt.point(tile.x, tile.y)
                            block.dropAt = tile.index
                            block.dragFrom = tile.index
                        }
                        onPositionChanged: (mouse) => {
                            if (!tile.held) {
                                if (!pressed)
                                    block.hoverMoved(tile.index, mapToGlobal(mouse.x, mouse.y))
                                return
                            }
                            const p = mapToItem(content, mouse.x, mouse.y)
                            block.dragPoint = Qt.point(p.x - grip.x, p.y - grip.y)
                            block.dropAt = block.slotAt(p.x, p.y)
                        }
                        onReleased: {
                            if (!tile.held)
                                return
                            const from = block.dragFrom
                            const to = block.dropAt
                            block.dragFrom = -1
                            block.dropAt = -1
                            if (to >= 0 && to !== from)
                                block.launcher.moveFavoriteTile(from, to)
                        }
                        onCanceled: {
                            block.dragFrom = -1
                            block.dropAt = -1
                        }
                        onClicked: (mouse) => {
                            if (wasHeld)
                                return
                            if (mouse.button === Qt.RightButton)
                                block.menuRequested(tile.index, mapToGlobal(mouse.x, mouse.y), mouse.modifiers)
                            else if (mouse.button === Qt.MiddleButton)
                                block.previewRequested(tile.index)
                            else
                                block.clicked(tile.index, mouse.modifiers)
                        }
                        onDoubleClicked: (mouse) => {
                            if (mouse.button === Qt.LeftButton && !wasHeld)
                                block.doubleClicked(tile.index, mouse.modifiers)
                        }
                    }
                }
            }
        }
    }
}
