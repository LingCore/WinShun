import QtQuick
import WinShun

// Cards as tiles, as many to a row as fit (each at least `minTileWidth`, at
// most `maxColumns`), each row as tall as its tallest: the 插件 page's
// 内置功能, 已安装的插件 and 插件库. It sizes its tiles itself (they set
// neither width nor height): one unfolded (`wide: true`, on it or on a
// Loader's item) takes the whole width at its own height, and those after
// it go on below. Read at a glance across, rather than down one long column.
Flow {
    id: tiles

    property real minTileWidth: 248
    property int maxColumns: 4
    property real dpr: Screen.devicePixelRatio

    readonly property int columns: Math.max(1, Math.min(maxColumns, Math.floor((width + spacing) / (minTileWidth + spacing))))
    // On whole device pixels, rounded down: the row never wraps early.
    readonly property real tileWidth: Math.floor((width - (columns - 1) * spacing) / columns * dpr) / dpr

    function placed(tile) { return tile.visible && tile.itemAt === undefined } // not a Repeater, not unseen
    function isWide(tile) { return tile.wide ?? tile.item?.wide ?? false }

    // Each child's width, then its height, by index (null: not placed). Apart:
    // a tile's words wrap to its width and make its height, which must not
    // come round to its width again (a binding loop, as a tile is first sized).
    readonly property var widths: {
        const result = new Array(children.length).fill(null)
        for (let i = 0; i < children.length; ++i) {
            if (placed(children[i]))
                result[i] = isWide(children[i]) ? width : tileWidth
        }
        return result
    }
    readonly property var heights: { // a row as tall as its tallest
        const result = new Array(children.length).fill(null)
        let row = []
        let tallest = 0
        const close = () => {
            for (const i of row)
                result[i] = tallest
            row = []
            tallest = 0
        }
        for (let i = 0; i < children.length; ++i) {
            const tile = children[i]
            if (!placed(tile))
                continue
            if (isWide(tile)) {
                close()
                result[i] = Theme.snapUp(tile.implicitHeight, dpr)
                continue
            }
            row.push(i)
            tallest = Math.max(tallest, Theme.snapUp(tile.implicitHeight, dpr))
            if (row.length === columns)
                close()
        }
        close()
        return result
    }
    onWidthsChanged: {
        for (let i = 0; i < children.length && i < widths.length; ++i) {
            if (widths[i] !== null)
                children[i].width = widths[i]
        }
    }
    onHeightsChanged: {
        for (let i = 0; i < children.length && i < heights.length; ++i) {
            if (heights[i] !== null)
                children[i].height = heights[i]
        }
    }

    spacing: 8
}
