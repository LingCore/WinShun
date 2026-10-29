pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The clipboard window's page (Win+V, ClipboardWindow.qml): a search field,
// the categories and groups, the history, hints below; the current entry in
// full on the right when asked for (a click on the row, Alt+P, → at the end
// of the search), and again the next time if left open. A row's paste
// button or a double click pastes it into the program that was in front,
// as Enter does; Shift+Enter as plain text, Alt+1…9 the first nine rows, Ctrl/Shift pick
// several, Tab switches categories. Opened from a field of Win顺's own (a
// search box, which it drops down under), it pastes into that field, and Esc
// goes back to it (Clipboard). Over another program that keeps the focus,
// the keys come from App (KeyRouter) to the item with the focus here, as the
// window's own would.
// From the top the sections go: the search field, the categories, the
// history, the hints; with clipboard.searchFar the history first, then the
// categories and the search field, the rows by the caret it opens under.
// Opened over the caret (or box: above), with clipboard.flipAbove they go
// from the bottom up, from the caret too, the history's newest entry by it
// (GitHub #38); the arrow keys go the way they point.
Item {
    id: page

    required property Clipboard clipboard
    required property WindowFrame frame
    property int rows: 8 // the list shows this many (ClipboardWindow.qml: what fits below)
    property int listWidth: width // the list's part; the preview takes the rest
    property bool previewOpen: false
    property bool above: false // over the caret or box it opens by (ClipboardWindow: Placement.above)

    property int previewWidth: 400 // as the window has it (ClipboardWindow.qml)
    property bool previewSizing: false // its edge is being dragged

    signal previewOpening() // just before: the window sizes it (ClipboardWindow.qml)
    signal previewResizing(int wanted) // the edge dragged: the width asked for
    signal previewResized() // ... let go
    signal previewWidthReset() // ... double clicked: the default

    readonly property int rowHeight: Theme.rowHeight // what the window's height is counted in
    // A picture's row: its thumbnail larger (ClipRow), larger still with largePictures.
    readonly property int pictureRowHeight: clipboard.largePictures ? 142 : 80
    // Opened from a field of Win顺's own: Enter pastes there, Esc goes back.
    readonly property bool intoField: clipboard.fieldName.length > 0
    readonly property string pasteLabel: intoField ? qsTr("粘贴到%1").arg(clipboard.fieldName) : qsTr("粘贴")
    // Everything but the rows.
    readonly property int chromeHeight: header.height + tabs.implicitHeight + 1 + 8 + footer.implicitHeight

    implicitHeight: chromeHeight + rows * rowHeight

    // Upside down: the end by the caret is the bottom, the list's first row
    // there too (BottomToTop).
    readonly property bool flipped: above && clipboard.flipAbove
    // How far each section starts from the end they go out from (the top;
    // flipped, the bottom), in their order (see above). The line between the
    // categories and the history keeps between them; the footer has its own
    // line on its inner edge. The sizes are the same in every order: the
    // window's height, which Placement refits it by, never follows the order.
    readonly property real headerFrom: clipboard.searchFar ? body.height + 1 + tabs.height : 0
    readonly property real tabsFrom: clipboard.searchFar ? body.height + 1 : header.height
    readonly property real lineFrom: clipboard.searchFar ? body.height : header.height + tabs.height
    readonly property real bodyFrom: clipboard.searchFar ? 0 : header.height + tabs.height + 1
    readonly property real footerFrom: header.height + tabs.height + 1 + body.height
    // ... as a y, for a section that tall.
    function placed(from: real, size: real): real {
        return flipped ? implicitHeight - from - size : from
    }
    // ↑ in rows: toward the first row; flipped, away from it.
    readonly property int rowUp: flipped ? 1 : -1

    function focusSearch() {
        input.forceActiveFocus()
    }
    function selectFirst() {
        list.stopScrolling()
        if (list.count > 0)
            list.currentIndex = 0
        // After rows went and came in place (ClipModel::setRows) the view
        // stops with the first row at the very top, its top margin scrolled
        // off; the true start is the margin above the origin. Flipped, it
        // stops with the bottom margin off just as well (list.restY).
        list.positionViewAtBeginning()
        list.contentY = list.restY()
    }
    // The clipboard's hotkey held (GitHub #107): on the entry its steps are
    // on (Clipboard.heldRow), wherever the list moved it.
    function selectHeld(row: int) {
        selectionAnchor = -1
        list.currentIndex = row
        list.positionViewAtIndex(row, ListView.Contain)
    }
    function windowHidden() {
        previewClickTimer.stop()
        clearHover()
        list.stopScrolling()
        numberHints.reset()
        closeContextMenu()
        menuLoader.active = false
        tabs.stopNaming()
    }

    // Several rows: Ctrl+click toggles one, Shift+click or Shift+arrows take
    // the rows from the anchor (where the range started) to here.
    property int selectionAnchor: -1
    readonly property int selectedCount: clipboard.items.selectedCount
    function moveSelection(delta) {
        if (list.count > 0)
            list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + delta))
        selectionAnchor = -1
    }
    function toggleRow(index) {
        clipboard.toggleSelected(index)
        list.currentIndex = index
        selectionAnchor = index
    }
    function selectTo(index, add) {
        if (list.count === 0 || index < 0)
            return
        if (selectionAnchor < 0 || selectionAnchor >= list.count)
            selectionAnchor = Math.max(0, list.currentIndex)
        clipboard.selectRange(selectionAnchor, index, add)
        list.currentIndex = index
    }
    function extendSelection(delta) {
        if (list.count > 0)
            selectTo(Math.max(0, Math.min(list.count - 1, list.currentIndex + delta)), false)
    }
    // The number keys (Clipboard.numberKeys): Alt+1... pastes the first nine
    // rows; showing their hints never changes which keys paste them.
    readonly property int numberKey: clipboard.numberKeys === "alt" ? Qt.Key_Alt
                                   : clipboard.numberKeys === "ctrl" ? Qt.Key_Control : -1
    NumberHints {
        id: numberHints
        mode: page.clipboard.numberHints
        key: page.numberKey
        // Over another app the existing KeyRouter delivers modifier keys too.
        listening: page.visible && (page.Window.active || page.clipboard.keysRouted) && !page.menuOpen
    }
    // A digit another program holds as a global hotkey is its (Clipboard.numberKeysLeft):
    // the row still shows the chord, struck through (ClipRow.hintTaken).
    function numberUsable(number) {
        return (clipboard.numberKeysLeft & (1 << number)) === 0
    }
    readonly property int numberModifier: numberKey === Qt.Key_Alt ? Qt.AltModifier
                                        : numberKey === Qt.Key_Control ? Qt.ControlModifier : -1

    // What the keys act on: the picked rows (-1) if any, else the current one.
    function targetRow() {
        return selectedCount > 0 ? -1 : list.currentIndex
    }

    // The preview, of the row asked about; asked about the row it shows (or
    // with nothing to show), it closes. Opened or closed so, it is so the
    // next time the window opens too (clipboard.previewOpen).
    function togglePreview(index) {
        previewClickTimer.stop()
        if (previewOpen && (index === list.currentIndex || index < 0 || index >= list.count)) {
            closePreview()
        } else if (index >= 0 && index < list.count) {
            list.currentIndex = index
            showPreview()
            clipboard.previewOpen = true
        }
    }
    function showPreview() {
        if (!previewOpen)
            previewOpening()
        previewOpen = true
    }
    function closePreview() {
        previewClickTimer.stop()
        clearHover()
        previewOpen = false
        clipboard.previewOpen = false
    }

    function requestRowPreview(index) {
        previewClickTimer.close = previewOpen && index === list.currentIndex
        list.currentIndex = index
        previewClickTimer.restart()
    }
    Timer {
        id: previewClickTimer
        property bool close: false
        interval: Application.styleHints.mouseDoubleClickInterval
        onTriggered: {
            if (close) {
                page.closePreview()
            } else {
                page.showPreview()
                page.clipboard.previewOpen = true
            }
        }
    }

    // An open preview shows the row the mouse rests on for a moment (GitHub
    // #47), the current row stays where it is. It never opens or closes the
    // preview: the window's width must not jump under the mouse.
    property int hoverIndex: -1 // -1: the current row
    property int hoverCandidate: -1
    property point hoverPos: Qt.point(-1, -1) // the last real mouse move, global
    // The row the preview shows; out of range (rows went) counts as none.
    readonly property int previewIndex: hoverIndex >= 0 && hoverIndex < list.count ? hoverIndex : list.currentIndex
    function clearHover() {
        hoverTimer.stop()
        hoverIndex = -1
        hoverCandidate = -1
    }
    function hoverRow(index: int, globalPos: point) {
        // A wheel moving rows under a still mouse sends the same position.
        if (globalPos.x === hoverPos.x && globalPos.y === hoverPos.y)
            return
        hoverPos = globalPos
        if (!previewOpen || previewSizing || list.rowDragging || (Qt.application.keyboardModifiers & (Qt.ControlModifier | Qt.ShiftModifier)))
            return
        hoverCandidate = index
        if (index === previewIndex)
            hoverTimer.stop()
        else
            hoverTimer.restart()
    }
    Timer {
        id: hoverTimer
        interval: 100
        onTriggered: {
            if (page.previewOpen && !page.previewSizing && !list.rowDragging && !(Qt.application.keyboardModifiers & (Qt.ControlModifier | Qt.ShiftModifier))
                    && page.hoverCandidate >= 0 && page.hoverCandidate < list.count)
                page.hoverIndex = page.hoverCandidate === list.currentIndex ? -1 : page.hoverCandidate
        }
    }

    readonly property ContextMenu contextMenu: menuLoader.item as ContextMenu
    readonly property bool menuOpen: contextMenu !== null && contextMenu.visible
    property int menuCategory: -1 // >= 0: the menu is a category tab's

    // `globalPos` is where the mouse was; without it (Menu key, Shift+F10)
    // the menu opens under the current row (flipped: over it, away from the
    // caret, as the window is).
    function openContextMenu(index, globalPos) {
        previewClickTimer.stop()
        if (index < 0 || index >= list.count)
            return
        if (!clipboard.items.isSelected(index))
            clipboard.clearSelection() // as in Explorer: the menu is for this row alone
        list.currentIndex = index
        const fromKeyboard = globalPos === undefined
        if (fromKeyboard) {
            list.positionViewAtIndex(index, ListView.Contain)
            globalPos = list.currentItem.mapToGlobal(56, flipped ? 6 : list.currentItem.height - 6)
        }
        menuCategory = -1
        menuLoader.active = true
        const menu = menuLoader.item as ContextMenu
        menu.popup(index, clipboard.items.idAt(index), clipboard.menuItems(index), globalPos, fromKeyboard,
                   fromKeyboard && flipped)
    }
    function openCategoryMenu(category, globalPos) {
        const entries = clipboard.categoryMenuItems(category)
        if (entries.length === 0)
            return
        menuCategory = category
        menuLoader.active = true
        const menu = menuLoader.item as ContextMenu
        menu.popup(category, -1, entries, globalPos, false)
    }
    function closeContextMenu() {
        if (contextMenu)
            contextMenu.dismiss()
    }

    // A name for a group: row >= 0 a new group for that row (or the picked
    // ones), -2 a new empty one, -1 a new name for `group`.
    property int namingRow: -2
    property real namingGroup: 0
    function commitName(name) {
        tabs.stopNaming()
        if (name.length > 0) {
            if (namingGroup > 0) {
                clipboard.renameGroup(namingGroup, name)
            } else {
                const id = clipboard.addGroup(name)
                if (id > 0 && namingRow >= 0) {
                    clipboard.moveToGroup(namingRow, id)
                } else if (id > 0) {
                    const list = clipboard.categories
                    for (let i = 0; i < list.length; ++i) {
                        if (list[i].group === id)
                            clipboard.category = i
                    }
                }
            }
        }
        focusSearch()
    }

    function handleKey(event) {
        previewClickTimer.stop()
        // A key typed (not a modifier held for one) puts the key hints back.
        if (![Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta].includes(event.key))
            pinyinHintTimer.stop()
        // Composing with the input method (the window active: opened from a
        // box of Win顺's, over the Start menu or an elevated program, naming
        // a group): the keys are its own. Alt+1 pastes no half-typed word,
        // Esc closes nothing. Not while the keys are routed: none composes
        // those, and a preedit left over must not deaden them.
        if (page.Window.active && input.preeditText.length > 0) {
            // Alt alone would open the window menu. A key with Ctrl or Alt
            // the input method leaves to the field, which would type it
            // (Alt+5: a 5 past the word, left in the search when Esc drops
            // the word) or act on its text (Ctrl+V, Ctrl+Z): neither.
            if (event.key === Qt.Key_Alt || (event.modifiers & (Qt.ControlModifier | Qt.AltModifier)))
                event.accepted = true
            return
        }
        numberHints.press(event)
        if (menuOpen && contextMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        const alt = (event.modifiers & Qt.AltModifier) !== 0
        if (event.key === Qt.Key_Alt) {
            event.accepted = true // alone, it would open the window menu
            return
        }
        const others = (event.modifiers & ~(Qt.ShiftModifier | Qt.KeypadModifier)) !== numberModifier
        if (numberModifier >= 0 && !others && event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            if (numberUsable(event.key - Qt.Key_0))
                clipboard.quickPaste(event.key - Qt.Key_0, shift)
            event.accepted = true
            return
        }
        if (alt && !ctrl && event.key === Qt.Key_P) {
            togglePreview(list.currentIndex)
            event.accepted = true
            return
        }
        // Ctrl+↑/↓: the current row up or down in its group's own order
        // ("固定" listed first, a group's tab), where it can move. At the end
        // of its group it stays put; rows that never move just go up or down.
        // Up and down on the screen: flipped, ↑ is later in the order.
        if (ctrl && !shift && !alt && selectedCount === 0 && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                && list.currentItem?.movable) {
            const to = list.currentIndex + (event.key === Qt.Key_Up ? rowUp : -rowUp)
            if (clipboard.canMove(list.currentIndex, to)) {
                clipboard.moveRow(list.currentIndex, to)
                list.positionViewAtIndex(list.currentIndex, ListView.Contain)
            }
            event.accepted = true
            return
        }
        switch (event.key) {
        case Qt.Key_Down: shift ? extendSelection(-rowUp) : moveSelection(-rowUp); break
        case Qt.Key_Up: shift ? extendSelection(rowUp) : moveSelection(rowUp); break
        case Qt.Key_PageDown: shift ? extendSelection(-rowUp * rows) : moveSelection(-rowUp * rows); break
        case Qt.Key_PageUp: shift ? extendSelection(rowUp * rows) : moveSelection(rowUp * rows); break
        case Qt.Key_Right:
            // The preview, with the caret at the end of the search (the
            // arrow would move it nowhere); else the caret moves.
            if (ctrl || shift || alt || input.selectedText.length > 0 || input.cursorPosition < input.text.length)
                return
            togglePreview(list.currentIndex)
            break
        case Qt.Key_Left:
            if (ctrl || shift || alt || !previewOpen || input.text.length > 0)
                return
            closePreview()
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (!clipboard.recording)
                clipboard.turnOn()
            else if (list.count > 0)
                clipboard.paste(targetRow(), shift)
            break
        case Qt.Key_Escape:
            if (selectedCount > 0)
                clipboard.clearSelection()
            else // an open preview stays (clipboard.previewOpen): it is there next time
                clipboard.dismiss()
            break
        case Qt.Key_Tab: clipboard.cycleCategory(1); break
        case Qt.Key_Backtab: clipboard.cycleCategory(-1); break
        case Qt.Key_C:
            // Ctrl+C copies text selected in the field, or a picture's words
            // selected in the preview; otherwise the entry.
            if (!ctrl || (input.selectedText.length > 0 && selectedCount === 0))
                return
            if (previewOpen && selectedCount === 0 && preview.selectedWords.length > 0)
                preview.copyWords()
            else
                clipboard.copy(targetRow())
            break
        case Qt.Key_P:
            if (!ctrl)
                return
            clipboard.togglePin(targetRow())
            break
        case Qt.Key_A:
            // The field's own first: Ctrl+A selects its text. With no text, or
            // all of it selected already, it picks the listed rows (not what
            // is in a group, unless this is a group's tab).
            if (!ctrl || shift || alt || (input.text.length > 0 && input.selectedText.length < input.text.length))
                return
            if (clipboard.selectAll() === 0)
                return
            selectionAnchor = -1
            break
        case Qt.Key_Delete:
            // Deletes an entry only where it would delete no typed text; picked
            // rows are deleted whatever the field holds (Ctrl+A in a search
            // leaves its text selected).
            if (ctrl || shift || (selectedCount === 0 && (input.selectedText.length > 0 || input.cursorPosition < input.text.length)))
                return
            clipboard.remove(targetRow())
            break
        case Qt.Key_Z:
            if (!ctrl || shift || !clipboard.canUndo)
                return
            clipboard.undo()
            break
        default:
            return
        }
        event.accepted = true
    }

    // The preview follows the current row and any change to it.
    property int previewRevision: 0
    Connections {
        target: page.clipboard.items
        function onDataChanged() { page.previewRevision++ }
        function onModelReset() { // an entry moved up (copied again): stay on a row
            page.clearHover()
            const held = page.clipboard.heldRow()
            if (held >= 0)
                page.selectHeld(held)
            else if (list.count > 0 && (list.currentIndex < 0 || list.currentIndex >= list.count))
                list.currentIndex = 0
            page.previewRevision++
        }
        function onCountChanged() {
            page.clearHover() // the rows' indexes moved
            page.previewRevision++
        }
    }

    // A Shift pressed alone under a Chinese layout while the keys come from
    // the program in front (Clipboard.pinyinHintRequested): meant to switch
    // to Chinese, which no input method does without the keyboard focus, and
    // the window keeps off it (GitHub #42). Pinyin finds Chinese anyway, and
    // that is said: by the empty field until the window hides, else in the
    // key hints' place until a key is typed, 5 s at most.
    property bool pinyinHint: false // the empty field's placeholder says it
    readonly property string pinyinHintText: qsTr("直接打拼音：tupian → 图片")
    Timer { id: pinyinHintTimer; interval: 5000 } // the footer says it while running

    Connections {
        target: page.clipboard
        function onShown() {
            input.text = ""
            page.pinyinHint = false
            pinyinHintTimer.stop()
            if (holdFade.running) // a hold's paste hid it mid-fade, no frame drawn to carry it on
                holdFade.complete()
            page.selectionAnchor = -1
            page.previewOpen = false // the list alone as it is put in place ...
            page.selectFirst()
            page.focusSearch()
        }
        function onPlaced() { // ... then the preview, if it was left open
            // Placed only now (over the caret, maybe upside down, its rows):
            // at the first row as it ends up.
            page.selectFirst()
            if (page.clipboard.previewOpen && list.count > 0)
                page.showPreview()
        }
        function onListChanged() {
            page.selectionAnchor = -1
            page.closeContextMenu()
            page.selectFirst()
            const held = page.clipboard.heldRow()
            if (held >= 0)
                page.selectHeld(held)
        }
        function onStepped(row) { page.selectHeld(row) }
        function onQueryChanged() {
            pinyinHintTimer.stop() // also for a paste, which goes past handleKey
            if (input.text !== page.clipboard.query)
                input.text = page.clipboard.query
        }
        // The keys back from the program in front (pinned, or unpinned):
        // what is typed now replaces the last search, not adds to it.
        function onKeysRoutedChanged() {
            if (page.clipboard.keysRouted)
                input.selectAll()
        }
        function onContextMenuKeyPressed() { page.openContextMenu(list.currentIndex) }
        function onPinyinHintRequested() {
            if (input.text.length === 0)
                page.pinyinHint = true
            else
                pinyinHintTimer.restart()
        }
        function onGroupNameRequested(row, group) {
            page.namingRow = row
            page.namingGroup = group
            let name = ""
            if (group > 0) {
                for (const c of page.clipboard.categories) {
                    if (c.group === group)
                        name = c.title
                }
            }
            tabs.startNaming(name)
        }
    }

    Item { // the sections, each where page.placed() puts it
        width: page.width
        height: page.implicitHeight

        Item { // header: drags the window, except for the field
            id: header
            y: page.placed(page.headerFrom, height)
            width: parent.width
            height: Theme.headerHeight

            Component.onCompleted: {
                page.frame.addDragArea(header)
                page.frame.addControl(input)
                page.frame.addControl(pinButton)
                page.frame.addControl(eyeButton)
            }

            Glyph {
                id: icon
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                glyph: "" // Paste
                size: Theme.searchIconSize
            }

            TextInput {
                id: input
                anchors.left: icon.right
                anchors.leftMargin: 14
                anchors.right: pausedTag.visible ? pausedTag.left : eyeButton.left
                anchors.rightMargin: pausedTag.visible ? 16 : 8
                anchors.verticalCenter: parent.verticalCenter
                focus: true
                // Typed into without the focus (keysRouted): the caret shows
                // where, steady (it blinks only with the focus).
                cursorVisible: activeFocus || page.clipboard.keysRouted
                clip: true
                color: Theme.text
                font.pixelSize: Theme.uiSearch
                selectByMouse: true
                selectionColor: Theme.textSelection
                selectedTextColor: Theme.text
                onTextChanged: page.clipboard.query = text
                Keys.onPressed: (event) => page.handleKey(event)
                Keys.onReleased: (event) => {
                    numberHints.release(event)
                    if (event.key === Qt.Key_Alt)
                        event.accepted = true
                }
                onActiveFocusChanged: numberHints.reset()

                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    text: page.pinyinHint ? page.pinyinHintText : qsTr("搜索剪贴板，支持拼音")
                    color: Theme.faint
                    font: input.font
                    elide: Text.ElideRight
                    visible: input.text.length === 0 && input.preeditText.length === 0
                }

                // A passive tap leaves TextInput's caret and selection alone.
                // Recover routing on an explicit click without taking the
                // foreground from a field that closes when it loses focus.
                TapHandler {
                    enabled: page.clipboard.keysRouted
                    onTapped: page.clipboard.wantKeys()
                }

                // Pinned, the keys with the program in front: a click asks
                // for them (App routes them here, as the hotkey does).
                MouseArea {
                    anchors.fill: parent
                    enabled: page.clipboard.pinned && !page.clipboard.keysRouted && !page.Window.active
                    cursorShape: Qt.IBeamCursor
                    onClicked: page.clipboard.wantKeys()
                }
            }

            // Pinned: stays up, on top, through pastes and clicks elsewhere
            // (Clipboard.pinned). Not while pasting into a field of Win顺's.
            RowAction {
                id: pinButton
                visible: !page.intoField
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                glyph: page.clipboard.pinned ? "" : "" // PinnedFill, Pin
                glyphOverlay: page.clipboard.pinned ? "" : "" // Pin: the needle (lines up with PinnedFill in Fluent and MDL2)
                checked: page.clipboard.pinned
                tip: page.clipboard.pinned ? qsTr("取消钉住") : qsTr("钉住：一直显示在最上面，粘贴后也不收起")
                onClicked: page.clipboard.pinned = !page.clipboard.pinned
            }

            // The preview bar on or off (GitHub #47: hovering a row previews it,
            // which some would rather not have in the way of the keys). The same
            // switch as Alt+P and →, so one state and one memory
            // (clipboard.previewOpen); it only makes it visible and clickable.
            RowAction {
                id: eyeButton
                anchors.right: pinButton.visible ? pinButton.left : parent.right
                anchors.rightMargin: pinButton.visible ? 4 : 14
                anchors.verticalCenter: parent.verticalCenter
                glyph: page.previewOpen ? "" : "" // View, Hide
                checked: page.previewOpen
                tip: page.previewOpen ? qsTr("关闭预览（Alt+P）") : qsTr("预览（Alt+P）")
                onClicked: page.togglePreview(list.currentIndex)
            }

            Rectangle { // paused from the tray menu
                id: pausedTag
                visible: page.clipboard.paused
                anchors.right: eyeButton.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: pausedText.implicitWidth + 20
                height: 26
                radius: Theme.pillRadius(height)
                color: Theme.badge

                Text {
                    id: pausedText
                    anchors.centerIn: parent
                    text: qsTr("已暂停记录")
                    color: Theme.accent
                    font.pixelSize: Theme.uiCaption
                }
            }
        }

        ClipTabs {
            id: tabs
            y: page.placed(page.tabsFrom, height)
            width: parent.width
            parentLeft: page.x
            clipboard: page.clipboard
            onNameCommitted: (name) => page.commitName(name)
            onNameCancelled: {
                tabs.stopNaming()
                page.focusSearch()
            }
            onMenuRequested: (category, globalPos) => page.openCategoryMenu(category, globalPos)
        }

        Rectangle { // between the categories and the history
            y: page.placed(page.lineFrom, Theme.edge(Screen.devicePixelRatio))
            width: parent.width
            height: Theme.edge(Screen.devicePixelRatio)
            color: Theme.divider
        }

        Item {
            id: body
            y: page.placed(page.bodyFrom, height)
            width: parent.width
            height: page.rows * page.rowHeight + 8

            // The mouse left the list and the preview: the preview is the
            // current row's again. Into the preview it stays (to scroll it,
            // pick words in it).
            HoverHandler {
                onHoveredChanged: if (!hovered) page.clearHover()
            }

            Item {
                id: listArea
                width: page.previewOpen ? page.listWidth : parent.width
                height: parent.height

                ListView {
                    id: list
                    anchors.fill: parent
                    topMargin: 4
                    bottomMargin: 4
                    clip: true
                    model: page.clipboard.items
                    boundsBehavior: Flickable.StopAtBounds
                    highlightMoveDuration: 0
                    reuseItems: true
                    // Upside down (page.flipped): the first row at the bottom,
                    // by the caret, the later ones up from it.
                    verticalLayoutDirection: page.flipped ? ListView.BottomToTop : ListView.TopToBottom
                    // A row moved in its group's order (Ctrl+↑/↓, dragged)
                    // slides there, and the ones it passes make way.
                    // Not under a drag: the rows are where they go at once,
                    // for the next one to be found under the mouse.
                    // Any transition declared here, enabled or not, makes Qt
                    // leave some pooled rows drawn after removals (ClipRow hides them).
                    property bool rowDragging: false
                    move: Transition {
                        enabled: SystemTheme.animations && !list.rowDragging
                        NumberAnimation { property: "y"; duration: 150; easing.type: Easing.OutCubic }
                    }
                    moveDisplaced: Transition {
                        enabled: SystemTheme.animations && !list.rowDragging
                        NumberAnimation { property: "y"; duration: 150; easing.type: Easing.OutCubic }
                    }
                    onCurrentIndexChanged: {
                        page.clearHover() // the keys and clicks pick the row
                        positionViewAtIndex(currentIndex, ListView.Contain)
                    }
                    onCountChanged: {
                        if (count > 0 && (currentIndex < 0 || currentIndex >= count))
                            currentIndex = Math.min(Math.max(0, currentIndex), count - 1)
                    }
                    // Where it rests at its first row, that row's margin in
                    // view too: the top, or upside down the bottom (as
                    // ListWheelScroll.restY).
                    function restY(): real {
                        return verticalLayoutDirection === ListView.BottomToTop
                            ? originY + contentHeight + bottomMargin - height
                            : originY - topMargin
                    }
                    // Turned over, or back (by the setting, while open): the
                    // current row in view again, the first one as it rests.
                    onVerticalLayoutDirectionChanged: {
                        stopScrolling()
                        if (currentIndex > 0) {
                            positionViewAtIndex(currentIndex, ListView.Contain)
                        } else {
                            positionViewAtBeginning()
                            contentY = restY()
                        }
                    }

                    // Mixed row heights re-estimate the range while scrolling.
                    // The shared wheel controller follows those ends without
                    // native wheel fixup or an old goal surviving a reversal.
                    function stopScrolling() {
                        wheelScroll.cancel()
                        cancelFlick()
                    }
                    ListWheelScroll {
                        id: wheelScroll
                        view: list
                        animate: SystemTheme.animations
                        logicalPixelSteps: true
                        snapRows: true
                    }

                    delegate: ClipRow {
                        id: clipRow
                        objectName: "clipboardRow-" + index
                        width: list.width
                        height: clipRow.picture ? page.pictureRowHeight : page.rowHeight
                        hint: page.numberKey >= 0 && index < 9 ? index + 1 : 0
                        hintOpacity: numberHints.hintOpacity
                        hintModifier: page.numberKey === Qt.Key_Control ? "Ctrl" : "Alt"
                        hintTaken: clipRow.hint > 0 && !page.numberUsable(clipRow.hint)
                        pasteTip: page.pasteLabel
                        clickToPaste: page.clipboard.clickToPaste
                        largePictures: page.clipboard.largePictures
                        onDragRequested: (index) => page.clipboard.dragOut(index)
                        onMoveHeld: (held) => {
                            list.rowDragging = held
                            if (held)
                                list.stopScrolling()
                        }
                        onMoveRequested: (from, to) => {
                            list.currentIndex = from
                            page.clipboard.moveRow(from, to)
                        }
                        // On whole device pixels wherever the list has scrolled
                        // to (see Main.qml).
                        transform: ScrollPixelAlignment {
                            view: list
                            viewY: page.y + body.y + list.y
                            itemY: clipRow.y
                        }
                        onHoverMoved: (index, globalPos) => page.hoverRow(index, globalPos)
                        onClicked: (index, modifiers) => {
                            previewClickTimer.stop()
                            page.clearHover()
                            const ctrl = (modifiers & Qt.ControlModifier) !== 0
                            if (modifiers & Qt.ShiftModifier) {
                                page.selectTo(index, ctrl)
                            } else if (ctrl) {
                                page.toggleRow(index)
                            } else if (page.clipboard.clickToPaste) {
                                // As Enter: a picked row pastes all the picked ones; another, just itself.
                                if (page.selectedCount > 0 && !page.clipboard.items.isSelected(index))
                                    page.clipboard.clearSelection()
                                list.currentIndex = index
                                page.clipboard.paste(index, false)
                            } else {
                                page.requestRowPreview(index)
                            }
                        }
                        onPreviewRequested: (index) => page.togglePreview(index)
                        onPasteRequested: (index) => {
                            previewClickTimer.stop()
                            list.currentIndex = index
                            page.clipboard.paste(index, false) // a picked row pastes all the picked ones
                        }
                        onActionRequested: (index, action) => {
                            list.currentIndex = index
                            page.clipboard.trigger(index, action)
                        }
                        onMenuRequested: (index, globalPos) => page.openContextMenu(index, globalPos)
                    }
                }

                ListScrollBar {
                    flickable: list
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                }

                Column { // nothing to list: why, and what to do
                    anchors.centerIn: parent
                    width: parent.width - 80
                    spacing: 6
                    visible: list.count === 0

                    readonly property bool off: !page.clipboard.recording
                    readonly property bool none: page.clipboard.total === 0
                    readonly property bool searched: page.clipboard.query.trim().length > 0
                    readonly property var category: page.clipboard.categories[page.clipboard.category] ?? ({})

                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: parent.off ? qsTr("剪贴板历史没有打开")
                            // On one line: with a line break, it would spread over the page.
                            : parent.searched ? qsTr("没有找到“%1”").arg(page.clipboard.query.trim().replace(/\s+/g, " "))
                            : parent.none ? qsTr("还没有记录")
                            : (parent.category.group ?? 0) > 0 ? qsTr("“%1”里还没有内容").arg(parent.category.title)
                            : qsTr("这一类还没有内容")
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle // a long query keeps its end and the quotes
                        color: Theme.subtext
                        font.pixelSize: Theme.uiTitle
                    }
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: parent.off ? qsTr("打开后，复制过的文字、图片和文件都会记在这里，按 %1 随时找回来。")
                                           .arg(page.clipboard.shortcut.length > 0 ? page.clipboard.shortcut : "Win+V")
                            : parent.searched ? qsTr("按 Tab 换个分类试试")
                            : parent.none ? qsTr("复制一点什么，就会出现在这里")
                            : (parent.category.group ?? 0) > 0 ? qsTr("在条目上点右键，选“移到…”放进来；放进分组的内容不会过期")
                            : qsTr("按 Tab 看看别的分类")
                        color: Theme.faint
                        font.pixelSize: Theme.uiBody
                    }
                    Item {
                        width: 1
                        height: 8
                        visible: parent.off
                    }
                    FlatButton {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: parent.off
                        highlighted: true
                        text: qsTr("打开剪贴板历史")
                        onClicked: page.clipboard.turnOn()
                    }
                }
            }

            Rectangle {
                visible: page.previewOpen
                x: listArea.width
                width: Theme.edge(Screen.devicePixelRatio)
                height: parent.height
                color: Theme.divider
            }

            ClipPreview {
                id: preview
                visible: page.previewOpen
                onCopyRequested: (text, remember) => page.clipboard.copyText(text, remember)
                x: listArea.width + 1
                width: parent.width - x
                height: parent.height
                parentTop: page.y + body.y
                // Re-read when the rows change. The revision is part of the
                // condition: a bare read of it would be dropped by the QML
                // compiler, and the binding would not depend on it.
                info: page.previewRevision >= 0 && list.count > 0 && page.previewIndex >= 0
                      ? page.clipboard.preview(page.previewIndex) : ({})
            }

            // The edge between the list and the preview: drag to size the
            // preview (the window's left edge stays), double click for the
            // default.
            MouseArea {
                id: edge
                visible: page.previewOpen
                x: listArea.width // the line and 7 px of the preview; the scroll bar ends before it
                width: 8
                height: parent.height
                hoverEnabled: true
                cursorShape: Qt.SplitHCursor
                preventStealing: true
                property real pressX: 0
                property int pressWidth: 0
                property bool moved: false
                onPressed: (mouse) => {
                    page.clearHover()
                    page.previewSizing = true
                    pressX = mouse.x
                    pressWidth = page.previewWidth
                    moved = false
                }
                onPositionChanged: (mouse) => {
                    if (!pressed)
                        return
                    const wanted = pressWidth + Math.round(mouse.x - pressX)
                    if (wanted !== page.previewWidth) {
                        moved = true
                        page.previewResizing(wanted)
                    }
                }
                onReleased: {
                    page.previewSizing = false
                    if (moved)
                        page.previewResized()
                }
                onCanceled: {
                    page.previewSizing = false
                    if (moved)
                        page.previewResized()
                }
                onDoubleClicked: {
                    moved = false
                    page.previewWidthReset()
                }
            }
        }

        Item { // footer: status or how picked rows are joined, and key hints; drags the window
            id: footer
            y: page.placed(page.footerFrom, height)
            width: parent.width
            implicitHeight: Theme.footerHeight
            height: implicitHeight

            Component.onCompleted: {
                page.frame.addDragArea(footer)
                page.frame.addControl(separatorChip)
            }

            Rectangle { // on its inner edge
                y: page.flipped ? parent.height - height : 0
                width: parent.width
                height: Theme.edge(Screen.devicePixelRatio)
                color: Theme.divider
            }

            Text {
                id: statusLabel
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.right: holdNote.visible ? holdNote.left
                             : pinyinNote.visible ? pinyinNote.left
                             : hints.shown ? hints.left : infoButton.left
                anchors.rightMargin: holdNote.visible || pinyinNote.visible || hints.shown ? 12 : 8
                anchors.verticalCenter: parent.verticalCenter
                // Beside the hold's note only if it fits whole: the note says what letting go does.
                visible: page.selectedCount === 0
                         && (!holdNote.visible || statusLabel.implicitWidth <= footer.width - hints.reserved - holdNote.width)
                text: page.clipboard.statusText
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.uiBody
                elide: Text.ElideRight
            }

            Row { // picked: how many (some may be out of the list), and joined by what
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                visible: page.selectedCount > 0
                spacing: 8

                Text { // the order they go in: on the rows
                    anchors.verticalCenter: parent.verticalCenter
                    text: page.selectedCount === 1 ? qsTr("已选 1 条，Esc 取消选择")
                                                   : qsTr("已选 %Ln 条，合在一起，用", "", page.selectedCount)
                    color: Theme.subtext
                    font.pixelSize: Theme.uiBody
                }
                Rectangle {
                    id: separatorChip
                    visible: page.selectedCount >= 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: separatorText.implicitWidth + 20
                    height: 26
                    radius: Theme.pillRadius(height)
                    color: separatorArea.containsMouse ? Theme.chipHover : Theme.track
                    border.width: 1
                    border.color: Theme.chipBorder

                    Text {
                        id: separatorText
                        anchors.centerIn: parent
                        text: page.clipboard.separatorName
                        color: Theme.text
                        font.pixelSize: Theme.uiCaption
                    }
                    MouseArea {
                        id: separatorArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.clipboard.cycleSeparator()
                    }
                }
                Text {
                    visible: page.selectedCount >= 2
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("隔开")
                    color: Theme.subtext
                    font.pixelSize: Theme.uiBody
                }
            }

            Row {
                id: hints
                anchors.right: infoButton.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14

                // Room for them all, unless rows are picked: what says so (and
                // what several are joined with) takes it.
                readonly property bool roomy: page.selectedCount === 0 || page.previewOpen
                // Beside the hints: the status's margin and gap, the info button.
                readonly property real reserved: 18 + 12 + 8 + infoButton.width + infoButton.anchors.rightMargin
                // Out of the way of a message too long to share the line with
                // them ("已删除 1 条，按 Ctrl+Z 撤销"), while it shows.
                // Pinned without the keys: no key does anything here; the
                // status says what a click does.
                readonly property bool shown: !(page.clipboard.pinned && !page.clipboard.keysRouted && !page.Window.active)
                                              && (page.selectedCount > 0
                                                  || statusLabel.implicitWidth <= footer.width - reserved - implicitWidth)
                opacity: shown && !pinyinNote.visible && !holdNote.visible ? 1 : 0 // still laid out: its width tells when it fits again

                // The ones always there, and their gaps; then Tab's and →'s
                // where there is room beside them and the status (beside the
                // list alone there often is not). The rest: the info button.
                readonly property real alwaysWidth: {
                    const widths = [enterHint, backHint].filter(h => h.visible).map(h => h.fullWidth)
                    return widths.reduce((sum, w) => sum + w, 0) + Math.max(0, widths.length - 1) * spacing
                }
                readonly property real free: footer.width - reserved - statusLabel.implicitWidth - alwaysWidth
                readonly property bool tabFits: page.selectedCount < 2 && spacing + tabHint.fullWidth <= free
                readonly property bool previewFits: page.selectedCount === 0
                    && (tabFits ? spacing + tabHint.fullWidth : 0) + spacing + previewHint.fullWidth <= free

                KeyHint { id: enterHint; keys: "Enter"; label: page.pasteLabel }
                KeyHint { id: backHint; keys: "Esc"; label: qsTr("返回"); visible: page.intoField && hints.roomy }
                KeyHint { id: tabHint; keys: "Tab"; label: qsTr("分类"); visible: hints.tabFits }
                KeyHint {
                    id: previewHint
                    keys: "→"
                    label: page.previewOpen ? qsTr("关闭预览") : qsTr("预览")
                    visible: hints.previewFits
                }
            }

            // In the key hints' place for a while, the field not empty (see
            // page.pinyinHint): pinyin finds Chinese. Only while the keys come
            // from the program in front, which is when no input method has them.
            Text {
                id: pinyinNote
                readonly property bool shown: pinyinHintTimer.running && page.selectedCount === 0
                                              && page.clipboard.keysRouted && !holdNote.shown
                anchors.right: infoButton.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, footer.width - hints.reserved)
                opacity: shown ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { enabled: SystemTheme.animations; NumberAnimation { duration: 150 } }
                text: page.pinyinHintText
                textFormat: Text.PlainText
                color: Theme.faint
                font.pixelSize: Theme.uiBody
                elide: Text.ElideRight
            }

            // In the key hints' place while letting go of the hotkey pastes the
            // current row (GitHub #107, Clipboard.holdHint): only once it does,
            // a step made or held a second, so a quick press shows nothing.
            Row {
                id: holdNote
                readonly property bool shown: page.clipboard.holdHint && list.count > 0
                // Where the line is short (a narrow list, longer words), Esc's goes first.
                readonly property bool escFits: holdReleaseHint.fullWidth + spacing + holdEscHint.fullWidth
                                                <= footer.width - hints.reserved
                anchors.right: infoButton.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14
                opacity: shown ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { enabled: SystemTheme.animations; NumberAnimation { id: holdFade; duration: 150 } }

                KeyHint { id: holdReleaseHint; keys: page.clipboard.holdKeys; label: qsTr("松开粘贴") }
                KeyHint { id: holdEscHint; keys: "Esc"; label: qsTr("取消"); visible: holdNote.escFits }
            }

            // The other keys, while the mouse rests on it.
            ShortcutsButton {
                id: infoButton

                readonly property string modifier: page.numberKey === Qt.Key_Alt ? "Alt" : "Ctrl"

                host: page.clipboard
                frame: page.frame
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                shortcuts: (page.numberKey < 0 ? [] : [[modifier + "+1…9", qsTr("粘贴第几条")]]) // the numbers on the rows' icons
                    .concat(page.intoField ? [] : [["Shift+Enter", qsTr("粘贴为纯文本")]])
                    .concat(page.numberKey < 0 ? []
                            : [[modifier + "+Shift+1…9", qsTr("第几条粘贴为纯文本")]])
                    // Only where it works: a hotkey with something to hold (a lone F9 has not), over another program.
                    .concat(!page.clipboard.holdToStep || page.intoField || !page.clipboard.shortcut.includes("+") ? []
                            : [[page.clipboard.shortcut, qsTr("按住连按往下选，松开粘贴")]])
                    .concat([
                        ["→  Alt+P", qsTr("打开或关闭预览")],
                        ["Shift+Tab", qsTr("上一个分类")],
                        ["↑ ↓", qsTr("上下移动，PgUp PgDn 翻页")],
                        ["Shift+↑ ↓", qsTr("选中多条，也可 Ctrl+单击、Shift+单击；按选的顺序一起粘贴")],
                        ["Ctrl+C", qsTr("复制；预览里选中了图中的字时，复制那一段")],
                        ["Ctrl+P", qsTr("固定或取消固定")],
                        ["Delete", qsTr("删除，Ctrl+Z 撤销")],
                        ["Shift+F10", qsTr("右键菜单")],
                        ["Esc", qsTr("关闭；选中了多条时先取消选择")]
                    ])
            }
        }
    }

    HoverTip {
        target: separatorArea.containsMouse ? separatorChip : null
        text: qsTr("点一下换一种：换行、空格、逗号、Tab、不分隔")
    }

    HoverTip {
        target: pinButton.tipWanted ? pinButton : null
        text: pinButton.tip
    }

    HoverTip {
        target: eyeButton.tipWanted ? eyeButton : null
        text: eyeButton.tip
    }

    // While the menu is open, a click anywhere on the page closes it (a right
    // click on a row then opens it again there).
    MouseArea {
        anchors.fill: parent
        enabled: page.menuOpen
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            page.closeContextMenu()
            mouse.accepted = mouse.button !== Qt.RightButton
        }
        onWheel: (wheel) => page.closeContextMenu()
    }

    Loader {
        id: menuLoader
        active: false
        sourceComponent: ContextMenu {
            host: page.clipboard
            onTriggered: (row, action) => {
                if (page.menuCategory >= 0)
                    page.clipboard.triggerCategory(row, action)
                else
                    page.clipboard.trigger(row, action)
            }
        }
    }
}
