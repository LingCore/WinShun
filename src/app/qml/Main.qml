pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The launcher window: search bar, up to eight results, status footer.
// Keyboard first: everything works without touching the mouse. The same
// window shows the clipboard history instead (Win+V, ClipboardPage.qml).
Window {
    id: window

    required property Launcher launcher
    required property Clipboard clipboard
    required property Placement placement
    required property WindowFrame frame

    readonly property bool clipboardMode: clipboard.active
    // The clipboard page's list: as many rows as fit below, like the results.
    readonly property int clipRows: Math.max(minRows, Math.min(maxRows,
                                             Math.floor((placement.room - clipPage.chromeHeight) / rowHeight)))

    readonly property int rowHeight: 60
    readonly property int maxRows: 8
    readonly property int minRows: 3 // the window settles no lower than these fit below
    // Everything but the rows: header, divider, the list's margins, footer.
    readonly property int chromeHeight: searchBar.implicitHeight + 1 + 8 + footer.implicitHeight
    // As many rows as fit between the window's top and the bottom of the screen.
    readonly property int fitRows: Math.max(minRows, Math.min(maxRows,
                                            Math.floor((placement.room - chromeHeight) / rowHeight)))
    readonly property bool contentMode: launcher.scope === Launcher.Content
    readonly property string trimmedQuery: launcher.query.trim()
    // After the list area has closed (see settleRows), not while it is held open.
    readonly property bool showEmptyState: trimmedQuery.length > 0 && list.count === 0 && !launcher.searching
                                           && shownRows === 0

    // Rows the list area is sized for. It grows at once but shrinks only
    // once the search has settled (or after a grace period), so a list that
    // is about to fill up again does not collapse and spring back.
    property int shownRows: 0
    function settleRows() {
        const wanted = Math.min(launcher.results.count, fitRows)
        if (wanted >= shownRows || !launcher.searching) {
            shownRows = wanted
            shrinkGrace.stop()
        } else if (!shrinkGrace.running) {
            shrinkGrace.start()
        }
    }
    Timer {
        id: shrinkGrace
        interval: 2000 // a content scan's first matches often take over a second
        onTriggered: window.shownRows = Math.min(window.launcher.results.count, window.fitRows)
    }
    onFitRowsChanged: { // the window was put somewhere else
        shownRows = Math.min(shownRows, fitRows) // no room for them: at once
        settleRows()
    }

    // The previous query's rows while a slow search (内容) has found nothing
    // yet: dimmed, but only once that lasts long enough to notice.
    property bool dimRows: false
    readonly property bool stale: launcher.stale
    onStaleChanged: {
        if (stale) {
            dimDelay.start()
        } else {
            dimDelay.stop()
            dimRows = false
        }
    }
    Timer {
        id: dimDelay
        interval: 250
        onTriggered: window.dimRows = window.stale
    }

    width: 760
    height: clipboardMode ? clipPage.implicitHeight : layout.implicitHeight
    // See-through to Mica, as the settings window (see there).
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.background
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    title: qsTr("Win顺")

    Binding {
        target: window.launcher.results
        property: "highlightColor"
        value: Theme.accent
    }
    Binding {
        target: window.clipboard.items
        property: "highlightColor"
        value: Theme.accent
    }

    Binding { // with the fewest rows: kept room for below the window
        target: window.placement
        property: "roomNeeded"
        value: (window.clipboardMode ? clipPage.chromeHeight : window.chromeHeight) + window.minRows * window.rowHeight
    }

    onClipboardModeChanged: {
        closeContextMenu()
        if (clipboardMode)
            clipPage.focusSearch()
        else
            searchBar.focusAndSelect()
    }

    function moveSelection(delta) {
        if (list.count > 0)
            list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + delta))
        selectionAnchor = -1
    }

    // Several rows: Ctrl+click toggles one, Shift+click or Shift+arrows take
    // the rows from the anchor (where the range started) to here.
    property int selectionAnchor: -1
    readonly property int selectedCount: launcher.results.selectedCount
    function toggleRow(index) {
        window.launcher.toggleSelected(index)
        list.currentIndex = index
        selectionAnchor = index
    }
    function selectTo(index, add) {
        if (list.count === 0 || index < 0)
            return
        if (selectionAnchor < 0 || selectionAnchor >= list.count)
            selectionAnchor = Math.max(0, list.currentIndex)
        window.launcher.selectRange(selectionAnchor, index, add)
        list.currentIndex = index
    }
    function extendSelection(delta) {
        if (list.count > 0)
            selectTo(Math.max(0, Math.min(list.count - 1, list.currentIndex + delta)), false)
    }

    // On the selection when there is one, else on the current row.
    function act(action) {
        if (selectedCount > 0)
            window.launcher.triggerSelection(action)
        else if (list.count > 0 && list.currentIndex >= 0)
            window.launcher.trigger(list.currentIndex, action)
    }

    readonly property ContextMenu contextMenu: menuLoader.item as ContextMenu
    readonly property bool menuOpen: contextMenu !== null && contextMenu.visible

    // `globalPos` is where the mouse was; without it (Menu key, Shift+F10)
    // the menu opens under the current row.
    function openContextMenu(index, globalPos) {
        if (index < 0 || index >= list.count)
            return
        if (!window.launcher.results.isSelected(index))
            window.launcher.clearSelection() // as in Explorer: the menu is for this row alone
        list.currentIndex = index
        const fromKeyboard = globalPos === undefined
        if (fromKeyboard) {
            list.positionViewAtIndex(index, ListView.Contain)
            globalPos = list.currentItem.mapToGlobal(56, rowHeight - 6)
        }
        menuLoader.active = true
        // Not a line starting with "(": it would continue the line above, "true(...)".
        const menu = menuLoader.item as ContextMenu
        menu.popup(index, launcher.results.pathAt(index), launcher.menuItems(index), globalPos, fromKeyboard)
    }

    function closeContextMenu() {
        if (contextMenu)
            contextMenu.dismiss()
    }

    // Hidden: also free the menu window until it is needed again.
    onVisibleChanged: {
        if (!visible) {
            closeContextMenu()
            menuLoader.active = false
            clipPage.windowHidden()
        }
    }

    function handleKey(event) {
        if (menuOpen && contextMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        switch (event.key) {
        case Qt.Key_Down: shift ? extendSelection(1) : moveSelection(1); break
        case Qt.Key_Up: shift ? extendSelection(-1) : moveSelection(-1); break
        case Qt.Key_PageDown: shift ? extendSelection(fitRows) : moveSelection(fitRows); break
        case Qt.Key_PageUp: shift ? extendSelection(-fitRows) : moveSelection(-fitRows); break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            act(ctrl && shift ? Launcher.RunAsAdmin : ctrl ? Launcher.Reveal : Launcher.Open)
            break
        case Qt.Key_Escape:
            if (selectedCount > 0)
                window.launcher.clearSelection()
            else
                window.launcher.dismiss()
            break
        case Qt.Key_Tab: window.launcher.cycleScope(1); break
        case Qt.Key_Backtab: window.launcher.cycleScope(-1); break
        case Qt.Key_C:
            // Ctrl+C copies selected text in the box; otherwise the file itself
            // (always the files, once rows have been picked).
            if (!ctrl || (!shift && searchBar.hasSelection && selectedCount === 0))
                return
            act(shift ? Launcher.CopyPath : Launcher.CopyItem)
            break
        default:
            return
        }
        event.accepted = true
    }

    function selectFirst() {
        if (list.count > 0)
            list.currentIndex = 0
        list.positionViewAtBeginning()
    }

    Connections {
        target: window.launcher
        function onShown() {
            // Still cloaked here (App::showLauncher): nobody sees the jump.
            window.selectFirst()
            searchBar.focusAndSelect()
        }
        function onResultsReplaced() {
            window.selectionAnchor = -1
            window.closeContextMenu() // its row now holds another result
            window.selectFirst()
        }
        function onStatusChanged() { Qt.callLater(window.settleRows) }
        function onContextMenuKeyPressed() { window.openContextMenu(list.currentIndex) } // Menu key, Shift+F10
        function onQueryChanged() {
            if (searchBar.text !== window.launcher.query)
                searchBar.text = window.launcher.query
        }
    }

    Connections {
        target: window.launcher.results
        function onCountChanged() { Qt.callLater(window.settleRows) }
        // A refresh rewrites rows in place: the menu stays open only while
        // its row still holds the same file.
        function onDataChanged() { window.checkContextMenu() }
        function onRowsRemoved() { window.checkContextMenu() }
    }

    Connections {
        target: window.frame
        function onDoubleClicked() { window.placement.moveHome() } // on the header or footer
    }
    Connections {
        target: window.placement
        function onMovingChanged() { // the menu would stay behind
            if (window.placement.moving)
                window.closeContextMenu()
        }
    }

    function checkContextMenu() {
        if (menuOpen && launcher.results.pathAt(contextMenu.row) !== contextMenu.key)
            closeContextMenu()
    }

    Column {
        id: layout
        width: window.width
        visible: !window.clipboardMode

        SearchBar {
            id: searchBar
            width: parent.width
            launcher: window.launcher
            placement: window.placement
            frame: window.frame
            onKeyPressed: (event) => window.handleKey(event)
        }

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.divider
            visible: window.shownRows > 0 || window.showEmptyState
        }

        Item {
            id: listArea
            width: parent.width
            height: window.shownRows > 0 ? window.shownRows * window.rowHeight + 8 : 0
            visible: window.shownRows > 0

            ListView {
                id: list
                anchors.fill: parent
                topMargin: 4
                bottomMargin: 4
                clip: true
                opacity: window.dimRows ? 0.45 : 1
                Behavior on opacity { NumberAnimation { duration: 150 } }
                model: window.launcher.results
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                reuseItems: true
                onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

                delegate: ResultRow {
                    id: resultRow
                    width: list.width
                    height: window.rowHeight
                    // Drawn on whole device pixels, wherever the list has
                    // scrolled to (ResultRow puts its text on whole device
                    // pixels within). Between them (at 150 % most scroll
                    // positions are), text and icons round to the pixel grid
                    // each its own way, so while the list glides they take
                    // turns moving by a pixel and the rows shake. Only the
                    // picture moves, by under a pixel: contentY stays as
                    // Flickable has it, so wheel, drag and flick are its own.
                    transform: Translate {
                        y: {
                            const dpr = Screen.devicePixelRatio
                            // Not mapToItem(): it would not be asked again once the layout places the list.
                            const listTop = layout.y + listArea.y + list.y
                            const deviceY = (listTop + list.contentItem.y + resultRow.y) * dpr
                            return (Math.round(deviceY) - deviceY) / dpr
                        }
                    }
                    onClicked: (index, modifiers) => {
                        const ctrl = (modifiers & Qt.ControlModifier) !== 0
                        if (modifiers & Qt.ShiftModifier) {
                            window.selectTo(index, ctrl)
                        } else if (ctrl) {
                            window.toggleRow(index)
                        } else {
                            list.currentIndex = index
                            window.launcher.trigger(index, Launcher.Open) // a selected row opens the selection
                        }
                    }
                    onActionRequested: (index, action) => {
                        list.currentIndex = index
                        window.launcher.trigger(index, action)
                    }
                    onMenuRequested: (index, globalPos) => window.openContextMenu(index, globalPos)
                }
            }

            ListScrollBar {
                flickable: list
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
            }

            Text { // held open while file contents are still being searched
                anchors.centerIn: parent
                visible: list.count === 0 && window.launcher.searching
                text: qsTr("正在搜索文件内容…")
                color: Theme.faint
                font.pixelSize: Theme.fontBody
            }
        }

        Item {
            width: parent.width
            height: 80
            visible: window.showEmptyState

            Column {
                anchors.centerIn: parent
                spacing: 4

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: window.contentMode
                          ? qsTr("没有 %1包含“%2”").arg(window.launcher.contentFilesLabel).arg(window.trimmedQuery)
                          : qsTr("没有找到“%1”").arg(window.trimmedQuery)
                    textFormat: Text.PlainText // holds what was typed
                    color: Theme.subtext
                    font.pixelSize: Theme.fontTitle
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: window.contentMode ? qsTr("按 Tab 回到文件名搜索")
                        : window.launcher.searchesContent
                          ? qsTr("文件名和 %1的内容里都没有").arg(window.launcher.contentFilesLabel)
                          : qsTr("按 Tab 切换范围，或搜索文件内容")
                    color: Theme.faint
                    font.pixelSize: Theme.fontBody
                }
            }
        }

        Footer {
            id: footer
            width: parent.width
            launcher: window.launcher
            frame: window.frame
        }
    }

    ClipboardPage {
        id: clipPage
        width: window.width
        visible: window.clipboardMode
        clipboard: window.clipboard
        frame: window.frame
        rows: window.clipRows
    }

    // While the menu is open, a click anywhere in the launcher closes it
    // (a right click on a row then opens it again there).
    MouseArea {
        anchors.fill: parent
        enabled: window.menuOpen
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            window.closeContextMenu()
            mouse.accepted = mouse.button !== Qt.RightButton
        }
        onWheel: (wheel) => window.closeContextMenu()
    }

    Loader {
        id: menuLoader
        active: false
        sourceComponent: ContextMenu {
            host: window.launcher
            onTriggered: (row, action) => window.launcher.trigger(row, action)
        }
    }
}
