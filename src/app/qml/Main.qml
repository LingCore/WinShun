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

    // Grown into the search panel (Launcher.panel, Panel.qml): the filters
    // down the left, the rows in columns. The app sizes the window for it
    // (App::setPanel), so the bindings on the window's size let go then;
    // the rows' list and the search bar are the panel's own, and everything
    // here that works a list or the bar goes through `view` and `searchBar`.
    readonly property bool panel: launcher.panel
    readonly property var view: panel ? panelPage.view : boxList
    readonly property int gridColumns: panel ? panelPage.gridColumns : 1
    // Keep the input method in the text field while arrows browse the grid.
    // Typing or clicking the field returns horizontal arrows to its caret.
    property bool galleryNavigation: false
    readonly property SearchBar searchBar: panel ? panelPage.searchBar : boxBar
    readonly property int viewRowHeight: panel ? panelPage.rowHeight : rowHeight

    readonly property int rowHeight: Theme.rowHeight // 界面大小
    readonly property int maxRows: 8
    readonly property int minRows: 3 // the window settles no lower than these fit below
    // Everything but the rows: header, the strip of kinds of file, divider,
    // the list's margins, footer. Budget the full strip in every scope so
    // opening it does not change the row count. At the taskbar the list
    // gives the strip its room instead.
    readonly property int chromeHeight: searchBar.implicitHeight + (fullHeight ? 0 : typeStrip.fullHeight) + 1 + 8
                                        + footer.implicitHeight
    // TypeStrip's animated height sizes this window through the column, so
    // its outer edge, rows and footer follow the same animation. At the
    // taskbar the list gives the strip its room and the window stays full height.
    // As many rows as fit between the window's top and the bottom of the screen
    // (with the caption over the rows found elsewhere, when it is among them).
    readonly property int fitRows: Math.max(minRows, Math.min(maxRows,
                                            Math.floor((placement.room - chromeHeight - captionRoom - (fullHeight ? 0 : detailSpace)) / rowHeight)))
    // Rows whose title is the payload (a translation, a sum: a plugin's row
    // that only copies, ResultModel.copyTexts) and one of them cut short by
    // its line (GitHub #117): the list gets a text area (detailBlock) under
    // the rows, tall enough for the longest of them, and it shows the text of
    // the current row when that is one. So the window changes its height only
    // when the results change, not as the selection moves. The rows keep
    // their one height; the area is budgeted in fitRows (within the screen),
    // or, at the taskbar, takes room from the list.
    property var copyTexts: [] // per row, "" for any other
    function refreshCopyTexts() { copyTexts = panel ? [] : launcher.results.copyTexts() }
    Connections {
        target: window.launcher.results
        function onModelReset() { Qt.callLater(window.refreshCopyTexts) }
        function onRowsInserted() { Qt.callLater(window.refreshCopyTexts) }
        function onRowsRemoved() { Qt.callLater(window.refreshCopyTexts) }
        function onRowsMoved() { Qt.callLater(window.refreshCopyTexts) }
        function onLayoutChanged() { Qt.callLater(window.refreshCopyTexts) }
        function onDataChanged() { Qt.callLater(window.refreshCopyTexts) }
    }
    QtObject { Component.onCompleted: window.refreshCopyTexts() }
    FontMetrics { id: detailMetrics; font.pixelSize: Theme.uiTitle }
    // A row's title gets about the list's width less its icon and the buttons
    // (a little less, so that no cut row is missed).
    readonly property real detailLine: Math.max(0, boxList.width - 160)
    readonly property string detailLongest: {
        if (panel || clipboardBelow)
            return ""
        let best = ""
        let wide = detailLine
        for (const t of copyTexts) {
            const w = t ? detailMetrics.advanceWidth(t) : 0
            if (w > wide) {
                best = t
                wide = w
            }
        }
        return best
    }
    // What the area shows: the current row's text; empty on any other row.
    readonly property string detailText: detailLongest.length === 0 ? "" : (copyTexts[boxList.currentIndex] || "")
    readonly property int detailSpace: detailLongest.length === 0 ? 0
                                       : fullHeight ? Math.min(detailBlock.wanted, Math.max(0, (listRows - 1) * rowHeight))
                                       : detailBlock.wanted
    // Typed in a folder's window, its own rows first (GitHub #110): the rows
    // found elsewhere start here (Launcher.results.elsewhereRow; -1: none),
    // under a caption between the rows (ElsewhereCaption, the list's section
    // heading). Among the rows the window shows, the list area grows by it;
    // at the taskbar the area keeps its height, and the rows scroll.
    readonly property int elsewhereRow: launcher.results.elsewhereRow
    readonly property int captionHeight: Theme.labelRowHeight
    readonly property int captionRoom: !fullHeight && elsewhereRow >= 0
                                       && elsewhereRow < Math.min(maxRows, Math.floor((placement.room - chromeHeight) / rowHeight))
                                       ? captionHeight : 0
    // Only where it fits: at the fewest rows (minRows) the window keeps those
    // and the caption scrolls with the rows, as at the taskbar.
    readonly property int captionSpace: !fullHeight && elsewhereRow >= 0 && elsewhereRow < listRows
                                        && listRows * rowHeight + hintSpace + captionHeight <= placement.room - chromeHeight
                                        ? captionHeight : 0
    // The row at `y` in the list (its content's own coordinates), a caption
    // taken as part of the row after it (`after`), or of the one before. With
    // one, the rows are not all of a height: the list places those it has not
    // made by their average size, so the ones made are asked where they are
    // (again once laid out: contentHeight); where there is none, counted.
    function rowAtY(y, after) {
        const h = viewRowHeight
        if (panel || elsewhereRow < 0 || boxList.contentHeight <= 0)
            return Math.floor(y / h)
        const at = boxList.indexAt(boxList.contentX + 1, y)
        if (at >= 0)
            return at
        const under = boxList.itemAtIndex(elsewhereRow)
        const top = under ? under.y - captionHeight : elsewhereRow * h
        if (y >= top && y < top + captionHeight)
            return after ? elsewhereRow : elsewhereRow - 1
        return Math.floor((y < top ? y : y - captionHeight) / h)
    }
    readonly property bool contentMode: launcher.scope === Launcher.Content
    readonly property string trimmedQuery: launcher.query.trim()
    // What was typed, on one line to show it (with a line break in it, it
    // would spread over the window).
    readonly property string shownQuery: trimmedQuery.replace(/\s+/g, " ")
    // After the list area has closed (see settleRows), not while it is held
    // open. Set in settleEmpty, with what it says (emptyShown*): it stays as
    // it is while the next search runs, as the rows do (Launcher.stale), and
    // goes when that one has rows or has found nothing too. Taken away as the
    // search started (Tab from 内容 back to 全部), the window shrank to the
    // search box and grew again for the rows a frame later.
    property bool showEmptyState: false
    property string emptyShownTitle
    property string emptyShownHint
    property var emptyShownHints: []
    property bool dimEmpty: false // held long enough to notice (as dimRows): it is the last query's
    function settleEmpty() {
        if (trimmedQuery.length > 0 && view.count === 0 && !launcher.searching && shownRows === 0) {
            emptyShownTitle = emptyTitle
            emptyShownHint = emptyHint
            emptyShownHints = hints
            showEmptyState = true
            emptyDimDelay.stop()
            dimEmpty = false
        } else if (showEmptyState && trimmedQuery.length > 0 && view.count === 0 && launcher.searching) {
            if (!dimEmpty && !emptyDimDelay.running)
                emptyDimDelay.start()
        } else {
            showEmptyState = false
            emptyDimDelay.stop()
            dimEmpty = false
        }
    }
    Timer {
        id: emptyDimDelay
        interval: 250 // as dimDelay
        onTriggered: window.dimEmpty = true
    }
    onTrimmedQueryChanged: Qt.callLater(settleRows)
    onEmptyTitleChanged: Qt.callLater(settleRows)
    onEmptyHintChanged: Qt.callLater(settleRows)

    // What the search behind the rows passed over (Launcher.leftOutHints:
    // files over the size limit, system folders, other kinds of text, a USB
    // drive not indexed): said in rows after the last result, each with what
    // a click does (searched this once), or with Enter once ↓ past the last
    // row has put the keyboard on one (hintIndex; as homeIndex for the apps
    // above), ↓ ↑ going along them. With no result at all, the empty state
    // lists them instead, and ↓ ↑ Enter work there too. The one running
    // shows how far it is; Enter or a click on it stops it.
    readonly property var hints: launcher.leftOutHints
    readonly property bool showHints: hints.length > 0 && view.count > 0 && !panel
    readonly property int hintHeight: Theme.hintRowHeight
    // Room for the rows after the last result: when every result is shown
    // without scrolling (else they come into view at the end), and the window
    // keeps its own height (at the taskbar the list area is as tall as it gets).
    readonly property int hintSpace: showHints && !fullHeight && shownRows < fitRows ? hints.length * hintHeight : 0
    property int hintIndex: -1 // the hint the keyboard is on; -1: none
    property int rowsBeforeHints: 0 // view.count when the keyboard went onto the hints: where what one brings starts
    function enterHints() {
        rowsBeforeHints = view.count
        hintIndex = 0
        view.currentIndex = -1
        view.positionViewAtEnd()
    }
    function takeHint(index) {
        if (index < 0 || index >= hints.length)
            return
        if (hints[index].running)
            launcher.stopLeftOut()
        else
            launcher.searchLeftOut(hints[index].kind)
    }
    function takeHint2(index) { // the second link on the row (a hint's kind2): by mouse
        if (index >= 0 && index < hints.length && hints[index].kind2)
            launcher.searchLeftOut(hints[index].kind2)
    }
    onHintsChanged: {
        Qt.callLater(settleRows) // what the empty state lists
        // Changed under the keyboard (one taken up: it alone, running; done:
        // the rest, or none): the hint in the same place, or, none left, the
        // first row the one taken up brought in (they go after the last
        // row), else the last row.
        if (hintIndex < 0)
            return
        if (hints.length > 0) {
            hintIndex = Math.min(hintIndex, hints.length - 1)
        } else {
            hintIndex = -1
            if (view.count > 0)
                view.currentIndex = Math.min(rowsBeforeHints, view.count - 1)
        }
    }
    // Nothing is named as typed, and these rows sound like it: a heading over
    // them (Launcher.approximateHeading), above the list so the rows stay
    // where the number keys count them.
    readonly property int headingHeight: launcher.approximateHeading.length > 0 && view.count > 0 ? Theme.labelRowHeight : 0
    // Opened from the taskbar (its button, Win+S), the window stands on the
    // taskbar: the list area keeps the height of all the rows that fit, so
    // the search box stays where it is as results come and go.
    readonly property bool fullHeight: placement.atTaskbar && !clipboardBelow
    readonly property int listRows: fullHeight ? fitRows : shownRows
    // There, with nothing typed: the apps opened most in a row over the
    // recent ones, as Windows' own search starts out.
    readonly property bool showHome: fullHeight && trimmedQuery.length === 0 && launcher.frequentApps.length > 0
    // The app the keyboard is on (↑ from the first row); -1: in the view.
    property int homeIndex: -1
    onShowHomeChanged: if (!showHome) homeIndex = -1
    function enterHome() {
        homeIndex = 0
        view.currentIndex = -1
    }
    function leaveHome() {
        homeIndex = -1
        if (view.count > 0)
            view.currentIndex = 0
    }

    // Nothing found: under the search box, or in the list area at full height.
    readonly property string emptyTitle: launcher.queryError.length > 0 ? launcher.queryError
        : contentMode && launcher.whereMissing.length > 0
        ? qsTr("%1 不在索引里，没法在里面搜").arg(launcher.whereMissing)
        : contentMode && launcher.contentWhere.length > 0 && launcher.contentWords.length === 0
        ? qsTr("在 %1 里搜：接着输入要找的文字").arg(launcher.contentWhere)
        : contentMode && launcher.contentWhere.length > 0
        ? qsTr("%1 里没有 %2包含“%3”").arg(launcher.contentWhere).arg(launcher.contentFilesLabel)
                                      .arg(launcher.contentWords.replace(/\s+/g, " "))
        : contentMode
        ? qsTr("没有 %1包含“%2”").arg(launcher.contentFilesLabel).arg(shownQuery)
        : launcher.pluginKeyword.length > 0 ? qsTr("%1 没有找到“%2”").arg(launcher.pluginLabel).arg(shownQuery)
        : qsTr("没有找到“%1”").arg(shownQuery)
    readonly property string emptyHint: contentMode && launcher.contentWhere.length > 0
        ? qsTr("按 Ctrl+Tab 换个文件夹，Tab 切换范围")
        : contentMode && launcher.folderChips.length > 1 ? qsTr("按 Ctrl+Tab 只搜某个文件夹，Tab 切换范围")
        : contentMode ? qsTr("按 Tab 回到文件名搜索")
        : typeStrip.shown ? qsTr("按 Ctrl+Tab 换个类型，Tab 切换范围")
        : launcher.pluginKeyword.length > 0 ? qsTr("按 Esc 清空关键字后面的文字")
        : launcher.searchesContent ? qsTr("文件名和 %1的内容里都没有").arg(launcher.contentFilesLabel)
        : launcher.contentEnabled ? qsTr("按 Tab 切换范围，或搜索文件内容")
        : qsTr("按 Tab 切换范围")
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
            font.pixelSize: Theme.uiTitle
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: parent.hint
            color: Theme.faint
            font.pixelSize: Theme.uiBody
        }
        Repeater { // what the search passed over, each with its action (as after the last row; ↓ ↑ and Enter too)
            model: window.emptyShownHints

            delegate: Item {
                id: emptyHint

                required property int index
                required property var modelData
                // The first is current until ↓ ↑ say otherwise: it is what Enter takes.
                readonly property bool current: window.hintIndex === index || (window.hintIndex < 0 && index === 0)

                anchors.horizontalCenter: parent.horizontalCenter
                width: hintLine.width + 24
                height: Theme.labelRowHeight
                Accessible.role: Accessible.Button
                Accessible.name: modelData.text + " " + modelData.action

                Rectangle { // raised with the keyboard on it, or hovered, as a row
                    anchors.fill: parent
                    radius: Theme.largeControlRadius
                    color: emptyHint.current ? Theme.selection : emptyHintArea.containsMouse ? Theme.hover : "transparent"
                }
                Row {
                    id: hintLine
                    z: 1 // over emptyHintArea: the second link has an area of its own
                    anchors.centerIn: parent

                    Text {
                        text: emptyHint.modelData.text
                        color: Theme.faint
                        font.pixelSize: Theme.uiBody
                    }
                    Text {
                        text: " · "
                        color: Theme.faint
                        font.pixelSize: Theme.uiBody
                    }
                    Text { // the action: accent words on the plain background, as a link
                        text: emptyHint.modelData.action
                        color: Theme.accent
                        font.pixelSize: Theme.uiBody
                    }
                    Text {
                        visible: !!emptyHint.modelData.action2
                        text: "  "
                        font.pixelSize: Theme.uiBody
                    }
                    Text { // a second link on the row
                        visible: !!emptyHint.modelData.action2
                        text: emptyHint.modelData.action2
                        color: Theme.accent
                        font.pixelSize: Theme.uiBody
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: window.takeHint2(emptyHint.index)
                        }
                    }
                }
                MouseArea {
                    id: emptyHintArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: window.takeHint(emptyHint.index)
                }
            }
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
        settleEmpty() // in the same go: no frame with both the rows and the empty state
    }
    Timer {
        id: shrinkGrace
        interval: 2000 // a content scan's first matches often take over a second
        onTriggered: {
            window.shownRows = Math.min(window.launcher.results.count, window.fitRows)
            window.settleEmpty()
        }
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

    // Its size, unless it is the panel: then the app sizes it, and its user
    // resizes it. Its width is its user's (Placement.boxWidth: dragged at its
    // left or right edge, 760 until then).
    Binding {
        target: window
        property: "width"
        value: window.placement.boxWidth > 0 ? window.placement.boxWidth : 760
        when: !window.panel
        restoreMode: Binding.RestoreNone
    }
    Binding {
        target: window
        property: "height"
        value: layout.implicitHeight
        when: !window.panel
        restoreMode: Binding.RestoreNone
    }
    minimumWidth: panel ? 640 : 0
    Binding {
        target: window.placement
        property: "contentSize"
        value: Qt.size(window.placement.boxWidth > 0 ? window.placement.boxWidth : 760, layout.implicitHeight)
        when: !window.panel
        restoreMode: Binding.RestoreNone
    }
    minimumHeight: panel ? 360 : 0
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
        target: window.launcher
        property: "currentRow"
        // On a hint after the last row, that row: what the search brings
        // goes after it (never above the row the keyboard is on).
        value: window.hintIndex >= 0 ? view.count - 1 : view.currentIndex
    }
    Binding { // with the fewest rows: kept room for below the window
        target: window.placement
        property: "roomNeeded"
        value: window.chromeHeight + window.minRows * window.rowHeight
    }

    onClipboardBelowChanged: closeContextMenu()

    function moveSelection(delta) {
        if (hintIndex >= 0) { // on the hints after the last row: ↓ ↑ along them, ↑ from the first back to the last row
            if (delta > 0)
                hintIndex = Math.min(hints.length - 1, hintIndex + 1)
            else if (hintIndex > 0)
                hintIndex -= 1
            else if (view.count > 0)
                view.currentIndex = view.count - 1 // onCurrentIndexChanged leaves the hints
        } else if (view.count > 0) {
            const next = view.currentIndex + delta
            if (delta === 1 && next >= view.count && showHints)
                enterHints() // ↓ past the last row
            else
                view.currentIndex = Math.max(0, Math.min(view.count - 1, next))
        } else if (hints.length > 0) { // nothing found: along the hints under 没有找到, the first already shown as current
            hintIndex = Math.max(0, Math.min(hints.length - 1, Math.max(hintIndex, 0) + delta))
        }
        selectionAnchor = -1
    }

    // Several rows: Ctrl+click toggles one, Shift+click or Shift+arrows take
    // the rows from the anchor (where the range started) to here.
    property int selectionAnchor: -1
    readonly property int selectedCount: launcher.results.selectedCount
    function toggleRow(index) {
        window.launcher.toggleSelected(index)
        view.currentIndex = index
        selectionAnchor = index
    }
    function selectTo(index, add) {
        if (view.count === 0 || index < 0)
            return
        if (selectionAnchor < 0 || selectionAnchor >= view.count)
            selectionAnchor = Math.max(0, view.currentIndex)
        window.launcher.selectRange(selectionAnchor, index, add)
        view.currentIndex = index
    }
    function extendSelection(delta) {
        if (view.count > 0)
            selectTo(Math.max(0, Math.min(view.count - 1, view.currentIndex + delta)), false)
    }

    // Ctrl+1… (or Alt+1…, Launcher.numberKeys) opens the nth row shown,
    // counted from the first one at least half in view. The keys come here as
    // the window's own (no hook: GitHub #10); a digit another program holds
    // as a global hotkey stays its (numberkeys::heldMask).
    readonly property int numberKey: launcher.numberKeys === "ctrl" ? Qt.Key_Control
                                   : launcher.numberKeys === "alt" ? Qt.Key_Alt : -1
    readonly property int firstShownRow: Math.max(0, rowAtY(view.contentY + viewRowHeight / 2, true) * gridColumns)
    readonly property int lastShownRow: Math.min(view.count - 1, firstShownRow + 8,
                                                 rowAtY(view.contentY + view.height - viewRowHeight / 2, false) * gridColumns + gridColumns - 1)
    readonly property string numberModifier: numberKey === Qt.Key_Alt ? "Alt" : "Ctrl"
    NumberHints {
        id: numberHints
        mode: window.launcher.numberHints
        key: window.numberKey
        listening: window.visible && window.active && !window.menuOpen && !window.clipboardBelow
    }
    Connections {
        target: window.searchBar
        function onKeyReleased(event) { numberHints.release(event) }
    }
    onActiveFocusItemChanged: numberHints.reset()
    // A digit another program holds as a global hotkey is its (Launcher.numberKeysLeft):
    // the row still shows the chord, struck through (ResultRow.hintTaken).
    function numberUsable(number) {
        return number >= 1 && (launcher.numberKeysLeft & (1 << number)) === 0
    }
    function numberOf(index) {
        return numberKey >= 0 && index >= firstShownRow && index <= lastShownRow ? index - firstShownRow + 1 : 0
    }
    function openNumber(number) {
        const row = firstShownRow + number - 1
        if (!numberUsable(number) || row > lastShownRow)
            return
        window.launcher.clearSelection() // that row alone
        view.currentIndex = row
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

    // On the selection when there is one, else on the current row.
    function act(action) {
        if (selectedCount > 0)
            window.launcher.triggerSelection(action)
        else if (view.count > 0 && view.currentIndex >= 0)
            window.launcher.trigger(view.currentIndex, action)
    }

    readonly property ContextMenu contextMenu: menuLoader.item as ContextMenu
    readonly property bool menuOpen: contextMenu !== null && contextMenu.visible

    // `globalPos` is where the mouse was; without it (Menu key, Shift+F10)
    // the menu opens under the current row.
    function openContextMenu(index, globalPos) {
        if (index < 0 || index >= view.count)
            return
        // A file still waited for (a slow server, GitHub #116) is not opened
        // under the menu once it answers: the menu is what is asked for now.
        window.launcher.dropServerWait()
        if (!window.launcher.results.isSelected(index))
            window.launcher.clearSelection() // as in Explorer: the menu is for this row alone
        view.currentIndex = index
        const fromKeyboard = globalPos === undefined
        if (fromKeyboard) {
            view.positionViewAtIndex(index, ListView.Contain)
            globalPos = view.currentItem.mapToGlobal(56, viewRowHeight - 6)
        }
        nameTip.dismiss()
        menuLoader.active = true
        // Not a line starting with "(": it would continue the line above, "true(...)".
        const menu = menuLoader.item as ContextMenu
        menu.popup(index, launcher.results.pathAt(index), launcher.menuItems(index), globalPos, fromKeyboard)
    }

    function closeContextMenu() {
        if (contextMenu)
            contextMenu.dismiss()
        panelPage.closeColumns() // the panel's columns menu goes with it
    }

    // The system's own menu for a file's or folder's row (the shell's, with
    // every extension; Launcher.showSystemMenu): Shift+right-click,
    // Shift+Menu (Ctrl+Shift+F10), 更多选项 in Win顺's menu. `globalPos`
    // (logical): where to put it; none: at the pointer. False for a row that
    // has none (an app, a place, a web page, a plugin's): the caller shows
    // Win顺's menu instead.
    function openSystemMenu(index, globalPos) {
        if (index < 0 || index >= view.count || !window.launcher.hasSystemMenu(index))
            return false
        closeContextMenu()
        if (!window.launcher.results.isSelected(index))
            window.launcher.clearSelection() // as in Explorer: the menu is for this row alone
        view.currentIndex = index
        window.launcher.showSystemMenu(index, globalPos)
        return true
    }
    function rowPoint(index) { // under the row's icon, where the keyboard's menus open
        view.currentIndex = index
        view.positionViewAtIndex(index, ListView.Contain)
        return view.currentItem.mapToGlobal(56, viewRowHeight - 6)
    }

    // Hidden: also free the menu window until it is needed again.
    onVisibleChanged: {
        if (!visible) {
            closeContextMenu()
            menuLoader.active = false
            nameTip.dismiss()
        }
    }
    onActiveChanged: if (!active) nameTip.dismiss() // on its way out, or behind another window

    // The whole name and folder of a row that shows them cut, as the mouse
    // rests on it (NameTip; the box's rows and the panel's).
    NameTip {
        id: nameTip
        objectName: "nameTip" // App's --dump reads it
        host: window.launcher
    }

    // Along the strip under the box: in 文件 the kinds, in 内容 the folders.
    function stepStrip(delta) {
        if (launcher.pluginKeyword.length > 0 || !typeStrip.shown)
            return
        if (typeStrip.folders)
            launcher.cycleFolderChip(delta)
        else
            launcher.cycleFileType(delta)
    }

    function handleKey(event) {
        numberHints.press(event)
        nameTip.dismiss() // as Windows' tips go on a key
        if (menuOpen && contextMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        if (panelPage.columnMenuOpen && panelPage.columnMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        // Alone, it would open the window menu, and the next keys would go to
        // that: also on the first of a double Alt that comes too slowly.
        if (event.key === Qt.Key_Alt) {
            event.accepted = true
            return
        }
        if (isNumberChord(event) && searchBar.composition.length === 0) {
            window.launcher.pressNumber(event.key - Qt.Key_0)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_P && (event.modifiers & ~Qt.KeypadModifier) === Qt.AltModifier) {
            togglePreview()
            event.accepted = true
            return
        }
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        // Ctrl+T: pinned, or not (the pin at the box's corner; Lertaro's key,
        // linux.do #157). From the row of apps too.
        if (event.key === Qt.Key_T && ctrl && !shift && (event.modifiers & Qt.AltModifier) === 0) {
            window.launcher.togglePin()
            event.accepted = true
            return
        }
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
        if (event.key === Qt.Key_Down || event.key === Qt.Key_Up || event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
            window.launcher.keyboardMoved() // Alt+P is for this row now, not the one under a pointer left where it was
            if (panel && launcher.gallery && searchBar.composition.length === 0)
                galleryNavigation = true
        }
        switch (event.key) {
        case Qt.Key_Down: shift ? extendSelection(gridColumns) : moveSelection(gridColumns); break
        case Qt.Key_Up:
            if (!shift && showHome && view.currentIndex <= 0)
                enterHome()
            else
                shift ? extendSelection(-gridColumns) : moveSelection(-gridColumns)
            break
        case Qt.Key_PageDown: shift ? extendSelection(panel ? Math.max(1, Math.floor(view.height / viewRowHeight)) * gridColumns : fitRows) : moveSelection(panel ? Math.max(1, Math.floor(view.height / viewRowHeight)) * gridColumns : fitRows); break
        case Qt.Key_PageUp: shift ? extendSelection(panel ? -Math.max(1, Math.floor(view.height / viewRowHeight)) * gridColumns : -fitRows) : moveSelection(panel ? -Math.max(1, Math.floor(view.height / viewRowHeight)) * gridColumns : -fitRows); break
        case Qt.Key_Left:
        case Qt.Key_Right: // Alt+← →: the volume's row, in steps of 5 (held, on and on); plain ← → are the box's caret
            if (panel && launcher.gallery && galleryNavigation && !ctrl && !(event.modifiers & Qt.AltModifier)) {
                const delta = event.key === Qt.Key_Right ? 1 : -1
                shift ? extendSelection(delta) : moveSelection(delta)
                break
            }
            if ((event.modifiers & ~Qt.KeypadModifier) !== Qt.AltModifier || hintIndex >= 0
                    || searchBar.composition.length > 0
                    || !window.launcher.stepSwitch(view.currentIndex, event.key === Qt.Key_Right ? 1 : -1))
                return
            break
        case Qt.Key_Return:
        case Qt.Key_Enter: {
            // On a hint after the last row, or in the empty state with some:
            // that part is searched too (the first, with none chosen there).
            if (hintIndex >= 0 || (view.count === 0 && hints.length > 0)) {
                takeHint(Math.max(hintIndex, 0))
                break
            }
            // Held, a system switch's Enter is the first press only: the
            // repeats would switch it back and forth.
            if (event.isAutoRepeat && window.launcher.isSwitchRow(view.currentIndex))
                break
            // Shift+Enter: a plugin row's third action (Open on any other row).
            const action =ctrl && shift ? Launcher.RunAsAdmin : ctrl ? Launcher.Reveal
                         : shift ? Launcher.Alternate : Launcher.Open
            if (!window.launcher.holdUntilShown(action, 0)) // typed a moment ago: on the rows for it
                act(action)
            break
        }
        case Qt.Key_Escape:
            // In a plugin's keyword mode the words after the keyword go; in
            // 内容 in a folder chosen, the words (the folder stays); then the
            // window. An open preview stays as it is (Alt+P closes it) and
            // comes back when the launcher opens again.
            if (selectedCount > 0)
                window.launcher.clearSelection()
            else if (!window.launcher.clearToKeyword() && !window.launcher.clearContentWords())
                window.launcher.dismiss()
            break
        case Qt.Key_Tab:
            if (ctrl) { // Ctrl+Tab: along the strip under the box (GitHub #29)
                stepStrip(1)
                break
            }
            // In keyword mode the scopes mean nothing: Tab takes the row's
            // autocomplete instead. Outside it the hint row of a keyword
            // ("kill" typed alone, or "kil" that it begins with) takes it
            // too, from whichever row the keyboard is on (Launcher.completionRow).
            if (window.launcher.pluginKeyword.length > 0)
                window.launcher.autocomplete(view.currentIndex)
            else if (!window.launcher.autocomplete(view.currentIndex)) {
                // Always the scopes; the strip under the box is Ctrl+Tab's (GitHub #16, #29).
                window.launcher.cycleScope(1)
            }
            break
        case Qt.Key_S: // the footer's switch: the start list, or the order
            if (!ctrl || shift)
                return
            window.launcher.switchOrder()
            break
        case Qt.Key_E: // the search panel, and back (the button at the box's corner)
            if (!ctrl || shift)
                return
            window.launcher.togglePanel()
            break
        case Qt.Key_D: // typed in a folder's window: its rows first, or there alone (GitHub #110)
            if (!ctrl || shift || !window.launcher.switchTypingFolder())
                return
            break
        case Qt.Key_Menu: // Shift+Menu: the system's menu for the row; the Menu key alone comes back as a context-menu event (Win顺's menu)
            if (!shift || view.currentIndex < 0 || !openSystemMenu(view.currentIndex, rowPoint(view.currentIndex)))
                return
            break
        case Qt.Key_F10: // Ctrl+Shift+F10 the same (Shift+F10 is Win顺's menu, as the Menu key)
            if (!ctrl || !shift || view.currentIndex < 0 || !openSystemMenu(view.currentIndex, rowPoint(view.currentIndex)))
                return
            break
        case Qt.Key_Backtab:
            // Back through the scopes, as Shift+Tab goes back in Windows;
            // Ctrl+Shift+Tab back along the strip.
            if (ctrl)
                stepStrip(-1)
            else if (window.launcher.pluginKeyword.length === 0)
                window.launcher.cycleScope(-1)
            break
        case Qt.Key_Delete:
            // The screenshot history (jt): the row goes from it (Ctrl+Z puts it
            // back), unless the box has words after the caret, or selected.
            if (!window.launcher.screenshotMode || ctrl || shift || searchBar.hasSelection
                    || searchBar.field.cursorPosition < searchBar.field.length || view.currentIndex < 0)
                return
            act(Launcher.RemoveScreenshot)
            break
        case Qt.Key_Z:
            if (!ctrl || shift || !window.launcher.screenshotMode || !window.launcher.undoScreenshotRemoval())
                return
            break
        case Qt.Key_C:
            // Ctrl+C copies selected text in the box; otherwise the file itself
            // (always the files, once rows have been picked).
            if (!ctrl || (!shift && searchBar.hasSelection && selectedCount === 0))
                return
            if (!shift && detailEdit.selectedText.length > 0) { // picked in the long text under the rows
                detailEdit.copy()
                break
            }
            act(shift ? Launcher.CopyPath : Launcher.CopyItem)
            break
        default:
            return
        }
        event.accepted = true
    }

    // Alt+P: the row under the pointer when the mouse went there after the
    // keyboard last moved (Launcher.pointerIfMoved), else the keyboard's;
    // either way it becomes the current row, so the highlight says what is
    // shown and the arrows go on from it.
    function togglePreview() {
        if (homeIndex >= 0 || hintIndex >= 0)
            return
        const pointer = window.launcher.pointerIfMoved()
        if (pointer !== undefined && view.count > 0) {
            const p = view.mapFromGlobal(pointer.x, pointer.y)
            if (p.x >= 0 && p.y >= 0 && p.x < view.width && p.y < view.height) {
                const row = view.indexAt(p.x + view.contentX, p.y + view.contentY)
                if (row >= 0) {
                    window.launcher.clearSelection()
                    view.currentIndex = row
                }
            }
        }
        window.launcher.keyboardMoved() // pressed again where it is: the same row
        window.launcher.togglePreview(view.currentIndex)
    }

    function selectFirst() {
        hintIndex = -1
        view.stopScrolling()
        if (view.count > 0 && homeIndex < 0)
            view.currentIndex = launcher.results.defaultRow() // not keep awake's row above the recent ones
        view.positionViewAtBeginning()
        // ListView may stop at originY, leaving the top margin scrolled
        // off. A later resultsReplaced would then move a view already at
        // the true top down by that margin (see ClipboardPage.selectFirst).
        view.contentY = view.originY - view.topMargin
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
            nameTip.dismiss()
            // Still cloaked here (App::showLauncher): nobody sees the jump.
            window.launcher.keyboardMoved() // the pointer is where it was, wherever the window came up
            window.homeIndex = -1
            window.selectFirst()
            searchBar.focusAndSelect()
        }
        function onResultsReplaced() {
            nameTip.dismiss() // another row under the pointer, maybe
            window.launcher.keyboardMoved() // other rows under a pointer that did not move
            window.selectionAnchor = -1
            window.closeContextMenu() // its row now holds another result
            window.selectFirst()
        }
        function onStatusChanged() { Qt.callLater(window.settleRows) }
        function onBetterFirstRow() {
            if (window.homeIndex < 0 && view.count > 0)
                view.currentIndex = window.launcher.results.defaultRow()
        }
        function onCurrentRowMoved(index) { // a plugin's list changed under the keyboard: it stays on its row
            window.closeContextMenu() // it was for the row where it stood
            if (index >= 0 && index < view.count && window.homeIndex < 0 && window.hintIndex < 0)
                view.currentIndex = index
        }
        function onContextMenuKeyPressed(system) { // Menu key, Shift+F10; Shift+Menu when an IME kept the key itself
            const row = view.currentIndex
            if (system && row >= 0 && row < view.count && window.openSystemMenu(row, window.rowPoint(row)))
                return
            window.openContextMenu(row)
        }
        function onQueryChanged() {
            window.galleryNavigation = false
            nameTip.dismiss() // typed: the keys go to the field, not handleKey
            if (boxBar.text !== window.launcher.query)
                boxBar.text = window.launcher.query
            if (panelPage.searchBar.text !== window.launcher.query)
                panelPage.searchBar.text = window.launcher.query
        }
        function onPanelChanged() { // the other bar has the keyboard now, the words as they were
            window.galleryNavigation = false
            nameTip.dismiss()
            window.closeContextMenu()
            window.searchBar.focusAtEnd()
        }
    }

    Connections {
        target: window.launcher.results
        function onCountChanged() { Qt.callLater(window.settleRows) }
        // A refresh rewrites rows in place: the menu stays open only while
        // its row still holds the same file.
        function onDataChanged() { window.checkContextMenu() }
        function onRowsRemoved() { window.checkContextMenu() }
        function onRowsInserted() { window.checkContextMenu() } // a better content match above it
    }

    Connections {
        target: window.frame
        function onDoubleClicked() { window.placement.moveHome() } // on the header or footer
        function onHandleClicked() { window.launcher.openSettings() } // the search icon (SearchBar.qml)
    }
    Connections {
        target: window.placement
        function onMovingChanged() { // the menu and the tip would stay behind
            if (window.placement.moving) {
                window.closeContextMenu()
                nameTip.dismiss()
            }
        }
        function onSizingChanged() { // widened: rows under the pointer change
            window.closeContextMenu()
            nameTip.dismiss()
        }
    }

    function checkContextMenu() {
        if (menuOpen && launcher.results.pathAt(contextMenu.row) !== contextMenu.key)
            closeContextMenu()
    }

    // The panel over everything of the box (which keeps its layout, out of
    // sight, for when it is back): its own bar and list.
    Panel {
        id: panelPage
        anchors.fill: parent
        visible: window.panel
        launcher: window.launcher
        placement: window.placement
        frame: window.frame
        dimmed: window.dimRows
        onKeyPressed: (event) => window.handleKey(event)
        onEditingRequested: window.galleryNavigation = false
        onRowClicked: (index, modifiers) => window.rowClicked(index, modifiers)
        onRowDoubleClicked: (index, modifiers) => window.rowDoubleClicked(index, modifiers)
        onRowMenuRequested: (index, globalPos, modifiers) => window.rowMenuRequested(index, globalPos, modifiers)
        onRowDragRequested: (index) => window.launcher.dragOut(index)
        nameTip: nameTip
    }

    // A row clicked (the box's or the panel's): Ctrl toggles it in the
    // selection, Shift takes the rows up to it, plain opens it; the right
    // button with Shift the system's menu, else Win顺's.
    function rowClicked(index, modifiers) {
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        if (modifiers & Qt.ShiftModifier) {
            selectTo(index, ctrl)
        } else if (ctrl) {
            toggleRow(index)
        } else if (launcher.doubleClickToOpen || (panel && launcher.gallery)) {
            // Only selected (#44); the double click opens. A row of a picked
            // set keeps the set, so the double click opens all of it.
            if (!launcher.results.isSelected(index))
                launcher.clearSelection()
            view.currentIndex = index
            selectionAnchor = index
        } else {
            view.currentIndex = index
            launcher.clickRow(index) // a selected row opens the selection
        }
        if (panel && launcher.gallery)
            galleryNavigation = true
    }
    // A double click opens, where a click only selects; with Ctrl or Shift it
    // is two selecting clicks and nothing more.
    function rowDoubleClicked(index, modifiers) {
        if (!(launcher.doubleClickToOpen || (panel && launcher.gallery)) || (modifiers & (Qt.ControlModifier | Qt.ShiftModifier)))
            return
        view.currentIndex = index
        launcher.clickRow(index)
    }
    // A click on a system switch's own switch: it toggles, whatever the row asks.
    function rowSwitchClicked(index) {
        view.currentIndex = index
        launcher.clickRow(index, true)
    }
    function rowMenuRequested(index, globalPos, modifiers) {
        if (!(modifiers & Qt.ShiftModifier) || !openSystemMenu(index))
            openContextMenu(index, globalPos)
    }

    Column {
        id: layout
        width: window.width
        visible: !window.panel
        // Opening, the content comes in a little behind the window (Placement.slideIn).
        transform: Translate {
            x: window.placement.contentShift.x
            y: window.placement.contentShift.y
        }

        SearchBar {
            id: boxBar
            width: parent.width
            launcher: window.launcher
            placement: window.placement
            frame: window.frame
            onKeyPressed: (event) => window.handleKey(event)
        }

        TypeStrip {
            id: typeStrip
            width: parent.width
            launcher: window.launcher
            frame: window.frame
            shown: (window.launcher.scope === Launcher.Files || window.launcher.scope === Launcher.Folders
                    || window.launcher.scope === Launcher.Content)
                   && !window.clipboardBelow
        }

        Rectangle {
            width: parent.width
            height: Theme.edge(Screen.devicePixelRatio)
            color: Theme.divider
            visible: !window.clipboardBelow && (window.listRows > 0 || window.showEmptyState)
        }

        Item {
            id: listArea
            width: parent.width
            height: window.listRows > 0 ? window.listRows * window.rowHeight + 8 + window.hintSpace + window.captionSpace
                                          + (window.fullHeight ? -typeStrip.height : window.headingHeight + window.detailSpace) : 0
            visible: window.listRows > 0 && !window.clipboardBelow

            Text { // nothing is named as typed: these rows sound like it (window.headingHeight)
                id: readingHeading
                x: 20
                width: parent.width - 40
                height: window.headingHeight
                visible: height > 0
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle // a long query keeps its start and its end
                text: window.launcher.approximateHeading
                textFormat: Text.PlainText // holds what was typed
                color: Theme.subtext
                font.pixelSize: Theme.uiCaption
            }

            Column { // the apps opened most (showHome), over the recent ones
                id: home
                width: parent.width
                visible: window.showHome
                topPadding: 4

                Text {
                    x: 20
                    height: Theme.labelRowHeight
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("常用应用")
                    color: Theme.subtext
                    font.pixelSize: Theme.uiCaption
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
                                radius: Theme.largeControlRadius
                                color: tile.current ? Theme.selection : tileArea.containsMouse ? Theme.hover : "transparent"
                            }
                            Rectangle { // accent pill on the current one, as on the current row
                                visible: tile.current
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: parent.height - 6
                                width: 18
                                height: 3
                                radius: Theme.smallRadius
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
                                font.pixelSize: Theme.uiCaption
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
                    height: Theme.labelRowHeight
                    verticalAlignment: Text.AlignVCenter
                    visible: view.count > 0
                    text: window.launcher.startList === Launcher.NoStartList ? qsTr("收藏")
                          : window.launcher.hasFavorites
                          ? (window.launcher.startList === Launcher.RecentlyModified ? qsTr("收藏和最近修改") : qsTr("收藏和最近"))
                          : (window.launcher.startList === Launcher.RecentlyModified ? qsTr("最近修改") : qsTr("最近"))
                    color: Theme.subtext
                    font.pixelSize: Theme.uiCaption
                }
            }

            ListView {
                id: boxList
                anchors.fill: parent
                anchors.topMargin: (window.showHome ? home.height : 0) + window.headingHeight
                anchors.bottomMargin: window.detailSpace
                topMargin: 4
                bottomMargin: 4
                clip: true
                opacity: window.dimRows ? 0.45 : 1
                Behavior on opacity { NumberAnimation { duration: 150 } }
                model: window.launcher.results
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                reuseItems: true
                function stopScrolling() {
                    wheelScroll.cancel()
                    cancelFlick()
                }
                ListWheelScroll {
                    id: wheelScroll
                    view: boxList
                    animate: SystemTheme.animations
                    snapRows: true
                    headingRow: window.elsewhereRow
                    headingHeight: window.captionHeight
                }
                // Typed in a folder's window, its own rows first (GitHub #110):
                // the rows found elsewhere under a caption, the way to the
                // folder alone at its end (as Ctrl+D).
                section.property: "elsewhere"
                section.delegate: ElsewhereCaption {
                    id: caption
                    width: boxList.width
                    folder: window.launcher.typingFolder
                    alone: window.elsewhereRow === 0
                    transform: ScrollPixelAlignment { // on whole device pixels, as the rows
                        view: boxList
                        viewY: layout.y + listArea.y + boxList.y
                        itemY: caption.y
                    }
                    onOnlyRequested: window.launcher.switchTypingFolder()
                }
                onContentYChanged: nameTip.dismiss() // scrolled: another row under the pointer
                onCurrentIndexChanged: {
                    positionViewAtIndex(currentIndex, ListView.Contain)
                    // Rows can arrive while the opening list is still empty
                    // and zero-height; Contain then scrolls off the top margin.
                    if (currentIndex === 0)
                        contentY = originY - topMargin
                    if (currentIndex >= 0)
                        window.hintIndex = -1 // back on a row, however it got there
                }

                // After the last row: what the search passed over, a row each
                // with the offer to search it (window.hints). The keyboard
                // reaches them with ↓ past the last row (window.hintIndex).
                footer: Column {
                    id: hintRows
                    width: boxList.width
                    visible: window.showHints
                    transform: ScrollPixelAlignment {
                        view: boxList
                        viewY: layout.y + listArea.y + boxList.y
                        itemY: hintRows.y
                    }

                    Repeater {
                        model: window.showHints ? window.hints : []

                        delegate: Item {
                            id: hintRow

                            required property int index
                            required property var modelData
                            readonly property bool current: window.hintIndex === index

                            width: hintRows.width
                            height: window.hintHeight
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.text + " " + modelData.action

                            Rectangle { // as a row's: raised with the keyboard on it, or hovered
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                anchors.topMargin: 1
                                anchors.bottomMargin: 1
                                radius: Theme.largeControlRadius
                                color: hintRow.current ? Theme.selection : hintArea.containsMouse ? Theme.hover : "transparent"
                            }
                            Rectangle { // accent pill, as on the current row
                                visible: hintRow.current
                                x: 7
                                anchors.verticalCenter: parent.verticalCenter
                                width: 3
                                height: 18
                                radius: Theme.smallRadius
                                color: Theme.accent
                            }
                            Text {
                                x: 60 // under the rows' text
                                width: hintAction.x - 16 - x // the leftmost link (the first, with two showing)
                                anchors.verticalCenter: parent.verticalCenter
                                text: hintRow.modelData.text
                                color: Theme.subtext
                                font.pixelSize: Theme.uiBody
                                elide: Text.ElideRight
                            }
                            Text { // the action: accent words on the plain background, as a link; right, as the rows' labels
                                id: hintAction
                                anchors.right: hintAction2.visible ? hintAction2.left : parent.right
                                anchors.rightMargin: hintAction2.visible ? 14 : 18
                                anchors.verticalCenter: parent.verticalCenter
                                text: hintRow.modelData.action
                                color: Theme.accent
                                font.pixelSize: Theme.uiBody
                            }
                            Text { // a second link on the row (a hint's kind2), the mouse's
                                id: hintAction2
                                visible: !!hintRow.modelData.action2
                                z: 1 // over hintArea
                                anchors.right: parent.right
                                anchors.rightMargin: 18
                                anchors.verticalCenter: parent.verticalCenter
                                text: hintRow.modelData.action2
                                color: Theme.accent
                                font.pixelSize: Theme.uiBody
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: window.takeHint2(hintRow.index)
                                }
                            }
                            MouseArea {
                                id: hintArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: window.takeHint(hintRow.index)
                            }
                        }
                    }
                }

                // Scrolled, or moved with the keys, to the end: a page more if
                // there is one (Launcher.loadMore), added after the rows. Asked
                // again once a search ends, in case it brought nothing new.
                function loadMoreAtEnd() {
                    if (atYEnd && count > 0 && window.launcher.moreAvailable && !window.launcher.searching)
                        window.launcher.loadMore()
                }
                // Later, not from within the launcher's own signal (a search inside a search).
                onAtYEndChanged: Qt.callLater(loadMoreAtEnd)
                Connections {
                    target: window.launcher
                    function onStatusChanged() { Qt.callLater(boxList.loadMoreAtEnd) }
                }

                delegate: ResultRow {
                    id: resultRow
                    width: boxList.width
                    height: window.rowHeight
                    showModified: window.launcher.showModified
                    showSize: window.launcher.showFileSize || window.launcher.sort === Launcher.LargestFirst || window.launcher.sort === Launcher.SmallestFirst
                    hint: window.numberOf(index)
                    hintOpacity: numberHints.hintOpacity
                    hintModifier: window.numberModifier
                    hintTaken: resultRow.hint > 0 && !window.numberUsable(resultRow.hint)
                    nameTip: nameTip
                    // Drawn on whole device pixels, wherever the list has
                    // scrolled to (ResultRow puts its text on whole device
                    // pixels within). Between them (at 150 % most scroll
                    // positions are), text and icons round to the pixel grid
                    // each its own way, so while the list glides they take
                    // turns moving by a pixel and the rows shake. Align the
                    // picture without feeding rounding back into contentY;
                    // this also covers native dragging and touch gestures.
                    transform: ScrollPixelAlignment {
                        view: boxList
                        // Not mapToItem(): this also tracks layout changes.
                        viewY: layout.y + listArea.y + boxList.y
                        itemY: resultRow.y
                    }
                    onClicked: (index, modifiers) => window.rowClicked(index, modifiers)
                    onDoubleClicked: (index, modifiers) => window.rowDoubleClicked(index, modifiers)
                    onSwitchClicked: (index) => window.rowSwitchClicked(index)
                    onActionRequested: (index, action) => {
                        boxList.currentIndex = index
                        window.launcher.trigger(index, action)
                    }
                    onMenuRequested: (index, globalPos, modifiers) => window.rowMenuRequested(index, globalPos, modifiers)
                    onDragRequested: (index) => window.launcher.dragOut(index)
                }
            }

            ListScrollBar {
                id: boxScrollBar
                flickable: boxList
                anchors.right: parent.right
                anchors.top: boxList.top
                anchors.bottom: boxList.bottom
                // Its own at the window's right edge, not the edge that widens the window.
                Component.onCompleted: window.frame.addControl(boxScrollBar)
            }

            TextEdit { // measures the longest text at the area's width (detailBlock.wanted)
                id: detailMeasure
                visible: false
                width: listArea.width - 40
                text: window.detailLongest
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.PlainText
                font.pixelSize: Theme.uiTitle
            }

            Item { // detailText, all of it wrapped; scrolls beyond a few lines. Never takes the focus: the keys stay in the box.
                id: detailBlock
                readonly property int pad: 10
                readonly property int maxLines: 6
                readonly property real lineHeight: detailMeasure.lineCount > 0 ? detailMeasure.contentHeight / detailMeasure.lineCount : detailMeasure.font.pixelSize * 1.4
                // What the longest text wants, whole logical pixels: the divider, its padding and up to maxLines lines.
                readonly property int wanted: window.detailLongest.length === 0 ? 0
                                              : Math.ceil(Math.min(detailMeasure.contentHeight, maxLines * lineHeight)) + 2 * pad + 1
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: window.detailSpace
                visible: height > 0
                clip: true

                Rectangle {
                    width: parent.width
                    height: Theme.edge(Screen.devicePixelRatio)
                    color: Theme.divider
                }

                Flickable {
                    id: detailFlick
                    x: 20
                    y: detailBlock.pad + 1
                    width: parent.width - 40
                    height: Math.max(0, parent.height - 2 * detailBlock.pad - 1)
                    contentWidth: width
                    contentHeight: detailEdit.contentHeight
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true

                    TextEdit {
                        id: detailEdit
                        objectName: "detailText"
                        width: detailFlick.width
                        text: window.detailText
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        activeFocusOnPress: false // a click selects; the box keeps the keys
                        activeFocusOnTab: false
                        wrapMode: TextEdit.Wrap
                        textFormat: TextEdit.PlainText
                        color: Theme.text
                        selectionColor: Theme.accentFill
                        selectedTextColor: "white"
                        font.pixelSize: Theme.uiTitle
                        onTextChanged: deselect()
                    }
                }
            }

            Text { // held open while file contents are still being searched, or a plugin answers
                readonly property bool keywordMode: window.launcher.pluginKeyword.length > 0
                anchors.centerIn: parent
                // A plugin's wait shows only once it has lasted a moment
                // (pluginWaiting is empty before that), fading in.
                visible: boxList.count === 0 && window.launcher.searching
                         && (!keywordMode || window.launcher.pluginWaiting.length > 0)
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 100 } }
                text: keywordMode ? window.launcher.pluginWaiting : qsTr("正在搜索文件内容…")
                color: Theme.faint
                font.pixelSize: Theme.uiBody
            }

            EmptyState { // at full height, in the list area
                anchors.centerIn: parent
                width: parent.width - 80
                visible: window.fullHeight && window.showEmptyState
                title: window.emptyShownTitle
                hint: window.emptyShownHint
                opacity: window.dimEmpty ? 0.45 : 1
            }

            Text { // at full height, nothing typed and nothing opened yet (under the apps, if any)
                anchors.horizontalCenter: parent.horizontalCenter
                y: boxList.y + Math.round((boxList.height - height) / 2)
                width: parent.width - 80
                horizontalAlignment: Text.AlignHCenter
                visible: window.fullHeight && window.trimmedQuery.length === 0 && boxList.count === 0
                text: qsTr("输入名称或拼音，搜索文件、应用和设置")
                color: Theme.faint
                font.pixelSize: Theme.uiBody
                wrapMode: Text.Wrap
            }
        }

        Item {
            width: parent.width
            height: Math.max(80, emptyBelow.implicitHeight + 24) // taller with hints under it
            visible: window.showEmptyState && !window.clipboardBelow && !window.fullHeight

            EmptyState {
                id: emptyBelow
                anchors.centerIn: parent
                width: parent.width - 80
                title: window.emptyShownTitle
                hint: window.emptyShownHint
                opacity: window.dimEmpty ? 0.45 : 1
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
        enabled: window.menuOpen || panelPage.columnMenuOpen
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            // The columns menu is closed by any click, a right click on the header included
            const columns = panelPage.columnMenuOpen
            window.closeContextMenu()
            mouse.accepted = columns || mouse.button !== Qt.RightButton
        }
        onWheel: (wheel) => window.closeContextMenu()
    }

    Loader {
        id: menuLoader
        active: false
        sourceComponent: ContextMenu {
            host: window.launcher
            onTriggered: (row, action) => {
                if (action === Launcher.SystemMenu) // 更多选项: the system's menu where this one was
                    window.openSystemMenu(row, Qt.point(x, y))
                else
                    window.launcher.trigger(row, action)
            }
        }
    }

    WindowEdge {}
}
