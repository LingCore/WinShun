pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The folder menu, column after column as Listary's (GitHub #348). This
// window is the first column, the menu's own rows; a row that leads
// somewhere (a group, a folder) opens the next column beside it, every
// column a window of its own that takes no focus either, so the window the
// menu is for keeps it. A column's window is made the first time it is
// needed, then kept, hidden, for the next.
//
// The keys act on the last column. The pointer resting on a row opens what
// it leads to, closing the columns after; on its way to the next column it
// passes rows that it does not choose (menu aim).
Window {
    id: menu
    required property FolderMenu folderMenu

    // The columns after the first, each the page that a row of the column
    // before it led to: { text, folder, children, pageTitle, reading,
    // byPointer, from } (from: that row).
    property var pages: []
    // The row chosen in the last column, where the keys act. A column before
    // it shows the row its next column came from, or the row the pointer
    // settled on in it (pointerLevel, pointerRow).
    property int highlighted: -1
    property int pointerLevel: -1
    property int pointerRow: -1
    // The row the pointer is over now, whether it settled there or not.
    property int hoverLevel: -1
    property int hoverRow: -1
    // A folder being read, its column not up yet: { level, from, folder, text, byPointer }.
    property var pending: null
    // The folders inside the folders read while the menu is up.
    property var listings: ({})
    // The windows of the columns after the first, made as needed.
    property var windows: []
    // The pointer resting on a row opens what it leads to after a moment
    // (dwell). Where the system's pointer was when a column last came,
    // went or changed: rows that come under a pointer at rest choose nothing
    // (a column put up under it would open the next one, and so on down,
    // GitHub #348). The system's, not a row's mapToGlobal: that moves with
    // the window and its fractions.
    property int dwellLevel: -1
    property int dwellRow: -1
    property point restingCursor
    // Where the pointer has been lately, and the row it passed on its way
    // to the next column (aimLevel, aimRow), chosen only if it stops there.
    property var trace: []
    property int aimLevel: -1
    property int aimRow: -1
    property Item hoveredEntry: null
    property string hoveredText
    property int tipLevel: -1
    property point popupAnchor
    property rect availableArea
    readonly property int maxColumns: 8
    readonly property int depth: pages.length + 1
    // The last column's page (null: the first column) and rows.
    readonly property var page: pages.length > 0 ? pages[pages.length - 1] : null
    readonly property var entries: page === null ? folderMenu.entries : page.children
    readonly property real padding: Theme.menuPadding
    readonly property int itemHeight: Theme.menuItemHeight
    readonly property real dpr: Screen.devicePixelRatio
    readonly property real maxHeight: Math.min(600, availableArea.height > 0 ? availableArea.height - 16 : 600)
    readonly property int direction: 1 // the side its next column opens to

    objectName: "folderMenuWindow"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.menuBackground
    // Each column as wide as its rows want, within these (GitHub #384).
    readonly property int minWidth: Theme.metrics.level < 2 ? 160 : Theme.metrics.size === "roomy" ? 200 : 180
    readonly property int maxWidth: Theme.metrics.level < 2 ? 340 : Theme.metrics.size === "roomy" ? 460 : 420
    function columnWidth(natural: real): real {
        const width = Math.min(Math.max(natural, minWidth), maxWidth)
        return availableArea.width > 0 ? Math.min(width, availableArea.width - 16) : width
    }
    width: columnWidth(first.naturalWidth)
    height: Math.min(first.listHeight + 2 * padding, maxHeight)
    onHeightChanged: if (visible) {
        y = within(popupTop)
        layout()
    }

    // Where the first column went: beside the pointer, flipped where the
    // screen ends. It stays there while columns come and go (#349).
    property real popupTop
    function within(y) {
        const area = availableArea
        return area.width > 0 ? Math.max(area.y + 8, Math.min(y, area.y + area.height - height - 8)) : y
    }
    function place() {
        let x = popupAnchor.x
        let y = popupAnchor.y
        const area = availableArea
        if (area.width > 0) {
            if (x + width > area.x + area.width) x -= width
            if (y + height > area.y + area.height) y -= height
            x = Math.max(area.x + 8, Math.min(x, area.x + area.width - width - 8))
        }
        menu.x = x
        menu.y = within(y)
        popupTop = menu.y
        layout()
    }
    function reset() {
        pending = null
        listings = {}
        slow.stop()
        dwell.stop()
        aim.stop()
        dwellLevel = dwellRow = -1
        aimLevel = aimRow = -1
        trace = []
        pages = []
        highlighted = -1
        pointerLevel = pointerRow = -1
        hoverLevel = hoverRow = -1
        hoveredEntry = null
        layout()
    }
    function popup(pos) {
        reset()
        rest()
        popupAnchor = pos
        availableArea = folderMenu.screenArea(pos)
        place()
        folderMenu.showColumn(menu)
    }

    // The window showing column `level` (0: this one), or null where it is
    // not shown: past maxColumns the last window shows the last column.
    function levelAt(visual) { return visual < maxColumns - 1 ? visual : depth - 1 }
    function visualOf(level) {
        return level < maxColumns - 1 ? level : level === depth - 1 ? maxColumns - 1 : -1
    }
    function frameAt(visual) { return visual === 0 ? menu : windows[visual - 1] ?? null }
    function columnAt(visual) {
        const frame = frameAt(visual)
        return visual === 0 ? first : frame === null ? null : frame.column
    }
    function rowsOf(level) {
        return level === 0 ? first.entries : level < depth ? pages[level - 1].children : []
    }
    // The row column `level` shows highlighted.
    function rowShown(level) {
        if (level >= depth) return -1
        if (level === depth - 1) return highlighted
        return pointerLevel === level ? pointerRow : pages[level].from
    }
    function fainter(level) { return level < depth - 1 && highlighted >= 0 && pointerLevel !== level }

    // Each column after the first beside the one before it, on the side
    // that one opened to, on the other where the screen ends, against the
    // screen's edge (over the one before) where neither has room; its first
    // row level with the row it came from; all inside the work area.
    function layout() {
        const shown = Math.min(depth, maxColumns)
        let raised = false
        for (let visual = 1; visual < shown; ++visual) {
            if (visual > windows.length)
                windows = windows.concat([columnWindow.createObject(menu, { visual: visual })])
            const frame = windows[visual - 1]
            const shownPage = pages[levelAt(visual) - 1]
            if (frame.page !== shownPage)
                frame.page = shownPage
            placeColumn(frame, visual)
            if (!frame.visible) {
                folderMenu.showColumn(frame) // cloaked until its first frame, in front
                raised = true
            }
        }
        if (raised && pathTip.visible)
            pathTip.raise() // the path stays in front of the columns
        for (let visual = shown; visual <= windows.length; ++visual) {
            const frame = windows[visual - 1]
            if (!frame.visible) continue
            folderMenu.hideColumn(frame)
            frame.page = null
        }
        if (tipLevel >= depth) {
            hoveredEntry = null
            tipLevel = -1
        }
    }
    function placeColumn(frame, visual) {
        const before = frameAt(visual - 1)
        const area = availableArea
        // Its top less the padding, from the top of the column before: its
        // first row level with the row it came from.
        const offset = columnAt(visual - 1).rowTop(pages[levelAt(visual - 1)].from) - padding - before.y
        const right = before.x + before.width - 1 // over its edge: one line between the two
        const left = before.x - frame.width + 1
        const roomRight = area.width <= 0 || right + frame.width <= area.x + area.width - 8
        const roomLeft = area.width <= 0 || left >= area.x + 8
        let toLeft = before.direction < 0
        if (toLeft ? !roomLeft && roomRight : !roomRight && roomLeft)
            toLeft = !toLeft
        const beside = toLeft ? left : right
        const level = Math.round(before.y + offset)
        let x = beside
        let y = level
        if (area.width > 0) {
            x = Math.max(area.x + 8, Math.min(x, area.x + area.width - frame.width - 8))
            y = Math.max(area.y + 8, Math.min(y, area.y + area.height - frame.height - 8))
        }
        frame.direction = toLeft ? -1 : 1
        // On the screen's own pixels: the border the two share one line, at
        // every scale (Qt's whole pixels are 1.5 of them at 150 %).
        folderMenu.placeColumn(frame, before, Qt.point(x, y), x === beside ? frame.direction : 0, offset, y === level, area)
    }

    function selectable(level, i) {
        const entry = rowsOf(level)[i]
        return entry !== undefined && entry.separator !== true && entry.disabled !== true
    }
    function branch(level, i) {
        const entry = rowsOf(level)[i]
        return entry !== undefined && (entry.children !== undefined || entry.folder !== undefined)
    }
    function step(level, from, delta) {
        const n = rowsOf(level).length
        let i = from
        for (let k = 0; k < n; ++k) {
            i = ((i + delta) % n + n) % n
            if (selectable(level, i)) return i
        }
        return -1
    }
    // A column, its rows or where it is, has changed: rows under a pointer
    // at rest choose nothing until it moves.
    function rest() { restingCursor = folderMenu.cursorPos() }
    function folderPage(asked, children) {
        return { text: asked.text, folder: asked.folder, children: children, from: asked.from, byPointer: asked.byPointer }
    }
    // `page` as the column after `level`, the columns after that closed;
    // its first row chosen when the keys asked for it.
    function showPage(level, page, byPointer) {
        pending = null
        slow.stop()
        page.byPointer = byPointer
        pages = pages.slice(0, level).concat([page])
        if (pointerLevel >= level) pointerLevel = pointerRow = -1
        highlighted = byPointer ? -1 : step(level + 1, -1, 1)
        rest()
        layout()
    }
    // The columns after `level` closed; `row` chosen in it, now the last.
    function closeAfter(level, row) {
        if (pending !== null && pending.level > level) {
            pending = null
            slow.stop()
        }
        // Chosen first: the row that column shows stays the same throughout,
        // or the list would bring it back into sight (a scroll closing columns).
        highlighted = row
        if (level < depth - 1) {
            pages = pages.slice(0, level)
            if (pointerLevel >= level) pointerLevel = pointerRow = -1
            rest()
            layout()
        }
    }
    // The column a row leads to: a group's rows, or the folders inside a
    // folder (read first; the column says so only when that takes a while).
    function open(level, i, byPointer) {
        if (level >= depth || !selectable(level, i) || !branch(level, i)) return
        if (level < depth - 1 && pages[level].from === i) { // its column is up already
            if (depth > level + 2) closeAfter(level + 1, pages[level + 1].from)
            return
        }
        const entry = rowsOf(level)[i]
        if (entry.children !== undefined) {
            showPage(level, { text: entry.text, pageTitle: entry.pageTitle, children: entry.children, from: i }, byPointer)
            return
        }
        const asked = { level: level, from: i, folder: entry.folder, text: entry.text, byPointer: byPointer }
        const known = listings[entry.folder]
        if (known !== undefined) {
            showPage(level, folderPage(asked, known), byPointer)
            return
        }
        // The columns after it stay until its own comes, put up in their place.
        pending = asked
        slow.restart()
        folderMenu.listFolder(entry.folder)
    }
    function activate(level, i, byPointer) {
        if (level >= depth || !selectable(level, i)) return
        const entry = rowsOf(level)[i]
        if (entry.action !== undefined) folderMenu.trigger(entry.action)
        else open(level, i, byPointer)
    }
    function back() {
        if (depth > 1) closeAfter(depth - 2, pages[depth - 2].from)
    }
    function listed(folder, rows) {
        const children = rows.length > 0 ? rows : [{ text: qsTr("没有子文件夹"), disabled: true }]
        const known = Object.assign({}, listings)
        known[folder] = children
        listings = known
        if (pending !== null && pending.folder === folder) {
            const asked = pending
            showPage(asked.level, folderPage(asked, children), asked.byPointer)
            return
        }
        for (let k = pages.length - 1; k >= 0; --k) { // a column saying it reads this folder: its folders now
            const reading = pages[k]
            if (reading.reading !== true || reading.folder !== folder) continue
            const updated = pages.slice()
            updated[k] = folderPage(reading, children)
            pages = updated
            if (k === pages.length - 1 && reading.byPointer !== true && highlighted < 0)
                highlighted = step(k + 1, -1, 1)
            rest()
            layout()
            return
        }
    }
    Timer { // the folder has not answered yet: its column, saying so
        id: slow
        interval: 200
        onTriggered: {
            const asked = menu.pending
            if (asked === null) return
            const reading = menu.folderPage(asked, [{ text: qsTr("正在读取…"), disabled: true }])
            reading.reading = true
            menu.showPage(asked.level, reading, asked.byPointer)
        }
    }

    // The pointer came onto a row, or moved on it.
    function pointed(level, i, row) {
        const same = hoverLevel === level && hoverRow === i
        hoverLevel = level
        hoverRow = i
        if (row && hoveredEntry !== row) {
            hoveredEntry = row
            hoveredText = row.tooltip
            tipLevel = level
        }
        const at = folderMenu.cursorPos()
        const last = trace.length > 0 ? trace[trace.length - 1] : null
        const still = last !== null && last.x === at.x && last.y === at.y
        record(at)
        if (at.x === restingCursor.x && at.y === restingCursor.y) return
        if (same && still) return // told again, not moved: as it was
        const from = apex(at)
        if (aiming(level, i, at, from)) {
            aimLevel = level
            aimRow = i
            aim.restart()
            return
        }
        aim.stop()
        settle(level, i)
    }
    function unpointed(level, i, row) {
        forgetRow(level, i, row)
        if (level === depth - 1) {
            if (highlighted === i) highlighted = -1
        } else if (pointerLevel === level && pointerRow === i) {
            pointerLevel = pointerRow = -1 // the row its next column came from again
        }
    }
    // Column `level`'s list moved (the wheel, its bar, or to show a row): the
    // columns after it follow the row they came from while it is in sight,
    // and close once it is not. What the list brings under a pointer at rest
    // opens nothing until the pointer moves (#348).
    function scrolled(level) {
        if (level >= depth) return
        rest()
        if (level === depth - 1) return
        const shown = columnAt(visualOf(level))
        if (shown !== null && shown.rowInSight(pages[level].from)) layout()
        else closeAfter(level, rowShown(level))
    }
    function forgetRow(level, i, row) {
        if (hoverLevel === level && hoverRow === i) hoverLevel = hoverRow = -1
        if (hoveredEntry === row) hoveredEntry = null
        if (dwellLevel === level && dwellRow === i) dwell.stop()
        if (aimLevel === level && aimRow === i) aim.stop()
    }
    // The pointer chose row i: highlighted, and what it leads to opens if it
    // stays (or the columns after close, for a row of a column before the last).
    function settle(level, i) {
        if (level >= depth) return
        if (pending !== null && level > pending.level) { // on into the columns there were: not that folder
            pending = null
            slow.stop()
        }
        if (level === depth - 1) {
            highlighted = i
            if (branch(level, i)) wait(level, i)
            else dwell.stop()
            return
        }
        pointerLevel = level
        pointerRow = i
        if (i === pages[level].from && depth === level + 2) dwell.stop() // its column is the last already
        else wait(level, i)
    }
    function wait(level, i) {
        if (dwellLevel === level && dwellRow === i && dwell.running) return
        dwellLevel = level
        dwellRow = i
        dwell.restart()
    }
    Timer {
        id: dwell
        interval: 350
        onTriggered: {
            const level = menu.dwellLevel
            const i = menu.dwellRow
            if (level >= menu.depth || !menu.selectable(level, i)) return
            if (level < menu.depth - 1 && i === menu.pages[level].from) {
                if (menu.depth > level + 2) // back on the row its column came from: the columns after that one close
                    menu.closeAfter(level + 1, menu.pages[level + 1].from)
            } else if (menu.branch(level, i))
                menu.open(level, i, true)
            else
                menu.closeAfter(level, i)
        }
    }
    // Menu aim: the pointer heading for the column after this one, inside
    // the triangle from where it was a few pixels ago to that column's near
    // corners, passes the rows on its way; one is chosen only if it stops.
    function apex(at) {
        for (let k = trace.length - 1; k >= 0; --k) {
            const p = trace[k]
            if (Math.abs(p.x - at.x) + Math.abs(p.y - at.y) >= 5) return p
        }
        return null
    }
    function record(at) {
        const last = trace.length > 0 ? trace[trace.length - 1] : null
        if (last === null || last.x !== at.x || last.y !== at.y)
            trace = trace.concat([Qt.point(at.x, at.y)]).slice(-6)
    }
    function aiming(level, i, at, from) {
        if (from === null || level >= depth - 1 || i === pages[level].from) return false
        const own = frameAt(visualOf(level))
        const next = frameAt(visualOf(level + 1))
        if (own === null || next === null || !next.visible) return false
        if (from.x < own.x || from.x >= own.x + own.width || from.y < own.y || from.y >= own.y + own.height) return false
        const nearX = next.x >= own.x ? next.x : next.x + next.width
        const a = Qt.point(nearX, next.y - 6)
        const b = Qt.point(nearX, next.y + next.height + 6)
        const side = (p, q, r) => (p.x - r.x) * (q.y - r.y) - (q.x - r.x) * (p.y - r.y)
        const d1 = side(at, from, a)
        const d2 = side(at, a, b)
        const d3 = side(at, b, from)
        return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0))
    }
    Timer { // stopped on a row it passed: chosen after all
        id: aim
        interval: 150
        onTriggered: {
            if (menu.hoverLevel === menu.aimLevel && menu.hoverRow === menu.aimRow)
                menu.settle(menu.aimLevel, menu.aimRow)
        }
    }

    function jumpTo(level, text) {
        const wanted = text.toLowerCase()
        const rows = rowsOf(level)
        const n = rows.length
        for (let k = 1; k <= n; ++k) {
            const i = ((highlighted < 0 ? -1 : highlighted) + k) % n
            if (selectable(level, i) && (rows[i].text ?? "").toLowerCase().startsWith(wanted)) {
                highlighted = i
                return
            }
        }
    }
    function handleKey(key, text) {
        const level = depth - 1
        // The keys take over: what the pointer rests on opens nothing until it moves.
        dwell.stop()
        aim.stop()
        rest()
        pointerLevel = pointerRow = -1
        switch (key) {
        case Qt.Key_Down: highlighted = step(level, highlighted, 1); return
        case Qt.Key_Up: highlighted = step(level, highlighted < 0 ? 0 : highlighted, -1); return
        case Qt.Key_Home:
        case Qt.Key_PageUp: highlighted = step(level, -1, 1); return
        case Qt.Key_End:
        case Qt.Key_PageDown: highlighted = step(level, 0, -1); return
        case Qt.Key_Right:
            if (highlighted >= 0) open(level, highlighted, false)
            return
        case Qt.Key_Left:
        case Qt.Key_Backspace: back(); return
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (highlighted >= 0) activate(level, highlighted, false)
            return
        case Qt.Key_Escape:
        case Qt.Key_Menu: folderMenu.dismiss(); return
        default:
            if (text.length > 0 && text.trim().length > 0) jumpTo(level, text)
        }
    }
    Connections {
        target: menu.folderMenu
        function onPopupRequested(pos) { menu.popup(pos) }
        function onHideRequested() {
            menu.reset()
            menu.folderMenu.hideColumn(menu)
        }
        function onKeyPressed(key, text) { menu.handleKey(key, text) }
        function onFolderListed(folder, entries) { menu.listed(folder, entries) }
    }

    FolderMenuColumn {
        id: first
        // The window's size, not its content item's: that one stays 0x0 for a
        // window off the screen (the previews), which gets no resize.
        width: menu.width
        height: menu.height
        entries: menu.folderMenu.entries
        highlighted: menu.rowShown(0)
        faint: menu.fainter(0)
        dpr: menu.dpr
        onPointed: (index, row) => menu.pointed(0, index, row)
        onUnpointed: (index, row) => menu.unpointed(0, index, row)
        onGone: (index, row) => menu.forgetRow(0, index, row)
        onChosen: index => menu.activate(0, index, true)
        onExpanded: index => menu.open(0, index, true)
        onScrolled: menu.scrolled(0)
    }
    Component {
        id: columnWindow
        Window {
            id: next
            required property int visual
            readonly property int level: menu.levelAt(visual)
            property var page: null
            property int direction: 1
            readonly property alias column: rows
            readonly property real dpr: Screen.devicePixelRatio
            objectName: "folderMenuColumn" + visual
            flags: menu.flags
            color: Theme.menuBackground
            width: menu.columnWidth(rows.naturalWidth)
            height: Math.min(rows.listHeight + 2 * rows.padding, menu.maxHeight)

            FolderMenuColumn {
                id: rows
                width: next.width
                height: next.height
                entries: next.page === null ? [] : next.page.children
                highlighted: menu.rowShown(next.level)
                faint: menu.fainter(next.level)
                dpr: next.dpr
                Accessible.name: next.page === null ? "" : (next.page.pageTitle ?? next.page.text ?? "")
                onPointed: (index, row) => menu.pointed(next.level, index, row)
                onUnpointed: (index, row) => menu.unpointed(next.level, index, row)
                onGone: (index, row) => menu.forgetRow(next.level, index, row)
                onChosen: index => menu.activate(next.level, index, true)
                onExpanded: index => menu.open(next.level, index, true)
                onScrolled: menu.scrolled(next.level)
            }
            WindowEdge { body: false } // the menu's colour is the window's
        }
    }
    MenuPathTip {
        id: pathTip
        host: menu
        availableArea: menu.availableArea
        target: menu.hoveredText.length > 0 ? menu.hoveredEntry : null
        text: menu.hoveredText
    }

    WindowEdge { body: false } // the menu's colour is the window's
}
