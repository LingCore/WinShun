pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The launcher window: search bar, up to eight results, status footer.
// Keyboard first: everything works without touching the mouse. The clipboard
// history (Win+V) has a window of its own (ClipboardWindow.qml); opened from
// the search box, it drops down under it, and the rows and the footer fold
// away until it is gone: the box shows what a paste goes into.
Window {
    id: window

    required property Launcher launcher
    required property Clipboard clipboard
    required property Placement placement
    required property WindowFrame frame

    // The clipboard dropped down under the search box (see above).
    readonly property bool clipboardBelow: clipboard.active && clipboard.field === searchBar.field

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
    // What was typed, on one line to show it (with a line break in it, it
    // would spread over the window).
    readonly property string shownQuery: trimmedQuery.replace(/\s+/g, " ")
    // After the list area has closed (see settleRows), not while it is held open.
    readonly property bool showEmptyState: trimmedQuery.length > 0 && list.count === 0 && !launcher.searching
                                           && shownRows === 0
    // Opened from the taskbar (its button, Win+S), the window stands on the
    // taskbar: the list area keeps the height of all the rows that fit, so
    // the search box stays where it is as results come and go.
    readonly property bool fullHeight: placement.atTaskbar && !clipboardBelow
    readonly property int listRows: fullHeight ? fitRows : shownRows
    // There, with nothing typed: the apps opened most in a row over the
    // recent ones, as Windows' own search starts out.
    readonly property bool showHome: fullHeight && trimmedQuery.length === 0 && launcher.frequentApps.length > 0
    // The app the keyboard is on (↑ from the first row); -1: in the list.
    property int homeIndex: -1
    onShowHomeChanged: if (!showHome) homeIndex = -1
    function enterHome() {
        homeIndex = 0
        list.currentIndex = -1
    }
    function leaveHome() {
        homeIndex = -1
        if (list.count > 0)
            list.currentIndex = 0
    }

    // Nothing found: under the search box, or in the list area at full height.
    readonly property string emptyTitle: contentMode
        ? qsTr("没有 %1包含“%2”").arg(launcher.contentFilesLabel).arg(shownQuery)
        : qsTr("没有找到“%1”").arg(shownQuery)
    readonly property string emptyHint: contentMode ? qsTr("按 Tab 回到文件名搜索")
        : launcher.searchesContent ? qsTr("文件名和 %1的内容里都没有").arg(launcher.contentFilesLabel)
        : qsTr("按 Tab 切换范围，或搜索文件内容")
    component EmptyState: Column {
        property string title
        property string hint
        spacing: 4

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            // A long query keeps its start and its end, and the quotes.
            elide: Text.ElideMiddle
            text: parent.title
            textFormat: Text.PlainText // holds what was typed
            color: Theme.subtext
            font.pixelSize: Theme.fontTitle
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: parent.hint
            color: Theme.faint
            font.pixelSize: Theme.fontBody
        }
    }

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
    height: layout.implicitHeight
    // See-through to Mica, as the settings window (see there).
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.background
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    title: qsTr("Win顺")

    Binding {
        target: window.launcher.results
        property: "highlightColor"
        value: Theme.accent
    }
    Binding { // with the fewest rows: kept room for below the window
        target: window.placement
        property: "roomNeeded"
        value: window.chromeHeight + window.minRows * window.rowHeight
    }

    onClipboardBelowChanged: closeContextMenu()

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

    // Ctrl+1… (or Alt+1…, Launcher.numberKeys) opens the nth row shown,
    // counted from the first one at least half in view. The modifier held
    // shows the numbers: Alt at once, Ctrl after a moment held alone (it also
    // starts Ctrl+C, Ctrl+Enter, a Ctrl+click and the double Ctrl). The keys
    // themselves come from NumberKeys (before other programs' hotkeys), or
    // here when it is not running.
    readonly property int numberKey: launcher.numberKeys === "ctrl" ? Qt.Key_Control
                                   : launcher.numberKeys === "alt" ? Qt.Key_Alt : -1
    readonly property int firstShownRow: Math.max(0, Math.floor((list.contentY + rowHeight / 2) / rowHeight))
    readonly property int lastShownRow: Math.min(list.count - 1, firstShownRow + 8,
                                                 Math.floor((list.contentY + list.height - rowHeight / 2) / rowHeight))
    property bool showNumbers: false
    Timer {
        id: numbersDelay
        interval: 500
        onTriggered: window.showNumbers = true
    }
    function hideNumbers() {
        numbersDelay.stop()
        showNumbers = false
    }
    function numberOf(index) {
        return showNumbers && index >= firstShownRow && index <= lastShownRow ? index - firstShownRow + 1 : 0
    }
    function openNumber(number) {
        const row = firstShownRow + number - 1
        if (number < 1 || row > lastShownRow)
            return
        hideNumbers()
        window.launcher.clearSelection() // that row alone
        list.currentIndex = row
        window.launcher.trigger(row, Launcher.Open)
    }
    // Exactly the modifier of the number keys (the number pad counts with Ctrl only: Alt+0169 types ©).
    function isNumberChord(event) {
        if (event.key < Qt.Key_1 || event.key > Qt.Key_9)
            return false
        const keypad = (event.modifiers & Qt.KeypadModifier) !== 0
        const modifiers = event.modifiers & ~Qt.KeypadModifier
        return numberKey === Qt.Key_Control ? modifiers === Qt.ControlModifier
             : numberKey === Qt.Key_Alt ? modifiers === Qt.AltModifier && !keypad : false
    }
    function handleKeyRelease(event) {
        if (event.key === numberKey)
            hideNumbers()
    }
    onActiveChanged: if (!active) hideNumbers()

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
        }
    }

    function handleKey(event) {
        if (menuOpen && contextMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        if (event.key === numberKey && numberKey >= 0) {
            if (event.isAutoRepeat)
                return
            if (numberKey === Qt.Key_Alt) {
                showNumbers = true
                event.accepted = true // alone, it would open the window menu
            } else if (event.modifiers === Qt.ControlModifier) {
                numbersDelay.restart()
            }
            return
        }
        hideNumbers() // a key with it: Ctrl+C, Ctrl+Enter...
        if (isNumberChord(event) && searchBar.composition.length === 0) {
            window.launcher.pressNumber(event.key - Qt.Key_0)
            event.accepted = true
            return
        }
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        if (homeIndex >= 0) { // on the row of apps (nothing typed, so ← → move nothing else)
            switch (event.key) {
            case Qt.Key_Left: homeIndex = Math.max(0, homeIndex - 1); break
            case Qt.Key_Right: homeIndex = Math.min(launcher.frequentApps.length - 1, homeIndex + 1); break
            case Qt.Key_Down: leaveHome(); break
            case Qt.Key_Up: break
            case Qt.Key_Return:
            case Qt.Key_Enter: launcher.openFrequentApp(homeIndex); break
            case Qt.Key_Escape: launcher.dismiss(); break
            default: return
            }
            event.accepted = true
            return
        }
        switch (event.key) {
        case Qt.Key_Down: shift ? extendSelection(1) : moveSelection(1); break
        case Qt.Key_Up:
            if (!shift && showHome && list.currentIndex <= 0)
                enterHome()
            else
                shift ? extendSelection(-1) : moveSelection(-1)
            break
        case Qt.Key_PageDown: shift ? extendSelection(fitRows) : moveSelection(fitRows); break
        case Qt.Key_PageUp: shift ? extendSelection(-fitRows) : moveSelection(-fitRows); break
        case Qt.Key_Return:
        case Qt.Key_Enter: {
            const action = ctrl && shift ? Launcher.RunAsAdmin : ctrl ? Launcher.Reveal : Launcher.Open
            if (!window.launcher.holdUntilShown(action, 0)) // typed a moment ago: on the rows for it
                act(action)
            break
        }
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
        if (list.count > 0 && homeIndex < 0)
            list.currentIndex = 0
        list.positionViewAtBeginning()
    }

    Connections {
        target: window.launcher
        function onNumberPressed(number) { window.openNumber(number) }
        function onHeldKey(action, number) {
            if (number > 0)
                window.openNumber(number)
            else
                window.act(action)
        }
        function onShown() {
            // Still cloaked here (App::showLauncher): nobody sees the jump.
            window.homeIndex = -1
            window.hideNumbers()
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
        // Opening, the content comes in a little behind the window (Placement.slideIn).
        transform: Translate {
            x: window.placement.contentShift.x
            y: window.placement.contentShift.y
        }

        SearchBar {
            id: searchBar
            width: parent.width
            launcher: window.launcher
            placement: window.placement
            frame: window.frame
            onKeyPressed: (event) => window.handleKey(event)
            onKeyReleased: (event) => window.handleKeyRelease(event)
        }

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.divider
            visible: !window.clipboardBelow && (window.listRows > 0 || window.showEmptyState)
        }

        Item {
            id: listArea
            width: parent.width
            height: window.listRows > 0 ? window.listRows * window.rowHeight + 8 : 0
            visible: window.listRows > 0 && !window.clipboardBelow

            Column { // the apps opened most (showHome), over the recent ones
                id: home
                width: parent.width
                visible: window.showHome
                topPadding: 4

                Text {
                    x: 20
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("常用应用")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }

                Row {
                    id: tiles
                    x: 20 // tiles, icons and text on multiples of 4 logical pixels: whole device pixels at 125–200 %

                    Repeater {
                        model: window.launcher.frequentApps

                        delegate: Item {
                            id: tile

                            required property int index
                            required property var modelData
                            readonly property bool current: window.homeIndex === index

                            width: 120
                            height: 96
                            // Vertically on whole device pixels too (see the rows').
                            transform: Translate {
                                y: {
                                    const dpr = Screen.devicePixelRatio
                                    const deviceY = (layout.y + listArea.y + home.y + tiles.y) * dpr
                                    return (Math.round(deviceY) - deviceY) / dpr
                                }
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.name

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
                                radius: 6
                                color: tile.current ? Theme.selection : tileArea.containsMouse ? Theme.hover : "transparent"
                            }
                            Rectangle { // accent pill on the current one, as on the current row
                                visible: tile.current
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: parent.height - 6
                                width: 18
                                height: 3
                                radius: 1.5
                                color: Theme.accent
                            }
                            Image { // as a row's icon (ResultRow.qml): its own pixels, unsmoothed
                                readonly property real dpr: Screen.devicePixelRatio
                                readonly property int pixels: Math.round(32 * dpr)

                                x: 44 + 1 / 64
                                y: 16 + 1 / 64
                                width: pixels / dpr
                                height: pixels / dpr
                                source: tile.modelData.icon
                                sourceSize.width: pixels / dpr
                                sourceSize.height: pixels / dpr
                                smooth: false
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                            }
                            Text {
                                x: 6
                                y: 56
                                width: parent.width - 12
                                horizontalAlignment: Text.AlignHCenter
                                text: tile.modelData.name
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideRight
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                lineHeight: 0.95
                            }
                            MouseArea {
                                id: tileArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: window.launcher.openFrequentApp(tile.index)
                            }
                        }
                    }
                }

                Text {
                    x: 20
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                    visible: list.count > 0
                    text: qsTr("最近")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
            }

            ListView {
                id: list
                anchors.fill: parent
                anchors.topMargin: window.showHome ? home.height : 0
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
                    showModified: window.launcher.showModified
                    hint: window.numberOf(index)
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
                        window.hideNumbers() // Ctrl held for a Ctrl+click
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
                anchors.top: list.top
                anchors.bottom: parent.bottom
            }

            Text { // held open while file contents are still being searched
                anchors.centerIn: parent
                visible: list.count === 0 && window.launcher.searching
                text: qsTr("正在搜索文件内容…")
                color: Theme.faint
                font.pixelSize: Theme.fontBody
            }

            EmptyState { // at full height, in the list area
                anchors.centerIn: parent
                width: parent.width - 80
                visible: window.fullHeight && window.showEmptyState
                title: window.emptyTitle
                hint: window.emptyHint
            }

            Text { // at full height, nothing typed and nothing opened yet (under the apps, if any)
                anchors.horizontalCenter: parent.horizontalCenter
                y: list.y + Math.round((list.height - height) / 2)
                width: parent.width - 80
                horizontalAlignment: Text.AlignHCenter
                visible: window.fullHeight && window.trimmedQuery.length === 0 && list.count === 0
                text: qsTr("输入名称或拼音，搜索文件、应用和设置")
                color: Theme.faint
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
            }
        }

        Item {
            width: parent.width
            height: 80
            visible: window.showEmptyState && !window.clipboardBelow && !window.fullHeight

            EmptyState {
                anchors.centerIn: parent
                width: parent.width - 80
                title: window.emptyTitle
                hint: window.emptyHint
            }
        }

        Footer {
            id: footer
            width: parent.width
            visible: !window.clipboardBelow
            launcher: window.launcher
            frame: window.frame
        }
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

    WindowEdge {}
}
