pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The launcher window: search bar, up to eight results, status footer.
// Keyboard first: everything works without touching the mouse.
Window {
    id: window

    required property Launcher launcher
    required property Placement placement
    required property WindowFrame frame

    readonly property int rowHeight: 60
    readonly property int maxRows: 8
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
        const wanted = Math.min(launcher.results.count, maxRows)
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
        onTriggered: window.shownRows = Math.min(window.launcher.results.count, window.maxRows)
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
    color: Theme.background
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    title: qsTr("Win顺")

    Binding {
        target: window.launcher.results
        property: "highlightColor"
        value: Theme.accent
    }

    Binding { // with every row shown: kept room for below the window
        target: window.placement
        property: "fullHeight"
        value: searchBar.implicitHeight + 1 + window.maxRows * window.rowHeight + 8 + footer.implicitHeight
    }

    function moveSelection(delta) {
        if (list.count > 0)
            list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + delta))
    }

    function act(action) {
        if (list.count > 0 && list.currentIndex >= 0)
            window.launcher.trigger(list.currentIndex, action)
    }

    readonly property ContextMenu contextMenu: menuLoader.item as ContextMenu
    readonly property bool menuOpen: contextMenu !== null && contextMenu.visible

    // `globalPos` is where the mouse was; without it (Menu key, Shift+F10)
    // the menu opens under the current row.
    function openContextMenu(index, globalPos) {
        if (index < 0 || index >= list.count)
            return
        list.currentIndex = index
        const fromKeyboard = globalPos === undefined
        if (fromKeyboard) {
            list.positionViewAtIndex(index, ListView.Contain)
            globalPos = list.currentItem.mapToGlobal(56, rowHeight - 6)
        }
        menuLoader.active = true
        // Not a line starting with "(": it would continue the line above, "true(...)".
        const menu = menuLoader.item as ContextMenu
        menu.popup(index, globalPos, fromKeyboard)
    }

    function closeContextMenu() {
        if (contextMenu)
            contextMenu.dismiss()
    }

    // Hidden: also free the menu window until it is needed again.
    onVisibleChanged: if (!visible) { closeContextMenu(); menuLoader.active = false }

    function handleKey(event) {
        if (menuOpen && contextMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        switch (event.key) {
        case Qt.Key_Down: moveSelection(1); break
        case Qt.Key_Up: moveSelection(-1); break
        case Qt.Key_PageDown: moveSelection(maxRows); break
        case Qt.Key_PageUp: moveSelection(-maxRows); break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            act(ctrl && shift ? Launcher.RunAsAdmin : ctrl ? Launcher.Reveal : Launcher.Open)
            break
        case Qt.Key_Escape: window.launcher.dismiss(); break
        case Qt.Key_Tab: window.launcher.cycleScope(1); break
        case Qt.Key_Backtab: window.launcher.cycleScope(-1); break
        case Qt.Key_C:
            // Ctrl+C copies selected text in the box; otherwise the file itself.
            if (!ctrl || (!shift && searchBar.hasSelection))
                return
            act(shift ? Launcher.CopyPath : Launcher.CopyItem)
            break
        case Qt.Key_1:
        case Qt.Key_2:
        case Qt.Key_3:
        case Qt.Key_4:
        case Qt.Key_5:
            if (!ctrl)
                return
            window.launcher.scope = event.key - Qt.Key_1
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
        if (menuOpen && launcher.results.pathAt(contextMenu.row) !== contextMenu.path)
            closeContextMenu()
    }

    Column {
        id: layout
        width: window.width

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

                // Rows only ever stand on whole device pixels (and ResultRow
                // puts its text on whole device pixels within). Between them
                // (at 150 % most positions are), text and icons round to the
                // pixel grid each its own way, so while the list glides they
                // take turns moving by a pixel and the rows shake. So the
                // wheel is ours, with every step of its glide aligned, and a
                // position set any other way (scroll bar, keyboard, new
                // results) is aligned at once. A touch flick, Flickable's
                // own, is aligned when it ends.
                function aligned(y) {
                    const dpr = Screen.devicePixelRatio
                    const offset = mapToItem(null, 0, 0).y * dpr // where the list itself stands
                    const minY = originY - topMargin
                    const maxY = Math.max(minY, originY + contentHeight + bottomMargin - height)
                    let snapped = (Math.round(Math.max(minY, Math.min(maxY, y)) * dpr - offset) + offset) / dpr
                    if (snapped > maxY + 1e-6)
                        snapped -= 1 / dpr
                    if (snapped < minY - 1e-6)
                        snapped += 1 / dpr
                    return snapped
                }
                function align() {
                    const y = aligned(contentY)
                    if (Math.abs(y - contentY) > 1e-6)
                        contentY = y
                }
                onContentYChanged: if (!moving) align()
                onMovementEnded: align()
                onCountChanged: align() // new results at the same contentY
                onHeightChanged: align()

                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    // As far per notch as Flickable's own wheel (24 px a line), gliding there.
                    onWheel: (event) => {
                        const from = glide.running ? glide.to : list.contentY
                        const to = list.aligned(from - event.angleDelta.y / 120 * Qt.styleHints.wheelScrollLines * 24)
                        glide.stop()
                        if (Math.abs(to - list.contentY) < 1e-6)
                            return
                        glide.from = list.contentY
                        glide.to = to
                        glide.start()
                    }
                }
                NumberAnimation {
                    id: glide
                    target: list
                    property: "contentY"
                    duration: 220
                    easing.type: Easing.OutCubic
                }

                delegate: ResultRow {
                    width: list.width
                    height: window.rowHeight
                    onClicked: (index, modifiers) => {
                        list.currentIndex = index
                        window.launcher.trigger(index, (modifiers & Qt.ControlModifier) ? Launcher.Reveal : Launcher.Open)
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
                          : window.launcher.scope === Launcher.Apps
                            ? qsTr("没有找到名为“%1”的应用").arg(window.trimmedQuery)
                            : qsTr("没有找到“%1”").arg(window.trimmedQuery)
                    textFormat: Text.PlainText // holds what was typed
                    color: Theme.subtext
                    font.pixelSize: Theme.fontTitle
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: window.contentMode ? qsTr("按 Tab 回到文件名搜索")
                        : window.launcher.scope === Launcher.Apps ? qsTr("按 Tab 搜索文件和文件夹")
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
            launcher: window.launcher
            onTriggered: (row, action) => window.launcher.trigger(row, action)
        }
    }
}
