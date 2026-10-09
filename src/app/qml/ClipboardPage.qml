pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The clipboard window's page (Win+V, ClipboardWindow.qml): a search field,
// the categories and groups, the history, hints below; the current entry in
// full on the right when asked for (a row's preview button, Alt+P). A click
// on a row pastes it into the program that was in front, as Enter does;
// Shift+Enter as plain text, Alt+1…9 the first nine rows, Ctrl/Shift pick
// several, Tab switches categories. Opened from a field of Win顺's own (a
// search box, which it drops down under), it pastes into that field, and Esc
// goes back to it (Clipboard).
Item {
    id: page

    required property Clipboard clipboard
    required property WindowFrame frame
    property int rows: 8 // the list shows this many (ClipboardWindow.qml: what fits below)
    property int listWidth: width // the list's part; the preview takes the rest
    property bool previewOpen: false

    signal previewOpening() // just before: the window sizes it (ClipboardWindow.qml)

    readonly property int rowHeight: 60
    // Opened from a field of Win顺's own: Enter pastes there, Esc goes back.
    readonly property bool intoField: clipboard.fieldName.length > 0
    readonly property string pasteLabel: intoField ? qsTr("粘贴到%1").arg(clipboard.fieldName) : qsTr("粘贴")
    // Everything but the rows.
    readonly property int chromeHeight: header.height + tabs.implicitHeight + 1 + 8 + footer.implicitHeight

    implicitHeight: chromeHeight + rows * rowHeight

    function focusSearch() {
        input.forceActiveFocus()
    }
    function selectFirst() {
        if (list.count > 0)
            list.currentIndex = 0
        list.positionViewAtBeginning()
    }
    function windowHidden() {
        altHeld = false
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
    // Alt held: the first nine rows show the number that pastes them (Alt+1...).
    property bool altHeld: false

    // What the keys act on: the picked rows (-1) if any, else the current one.
    function targetRow() {
        return selectedCount > 0 ? -1 : list.currentIndex
    }

    // The preview, of the row asked about; asked about the row it shows (or
    // with nothing to show), it closes.
    function togglePreview(index) {
        if (previewOpen && (index === list.currentIndex || index < 0 || index >= list.count)) {
            previewOpen = false
        } else if (index >= 0 && index < list.count) {
            list.currentIndex = index
            if (!previewOpen)
                previewOpening()
            previewOpen = true
        }
    }

    readonly property ContextMenu contextMenu: menuLoader.item as ContextMenu
    readonly property bool menuOpen: contextMenu !== null && contextMenu.visible
    property int menuCategory: -1 // >= 0: the menu is a category tab's

    // `globalPos` is where the mouse was; without it (Menu key, Shift+F10)
    // the menu opens under the current row.
    function openContextMenu(index, globalPos) {
        if (index < 0 || index >= list.count)
            return
        if (!clipboard.items.isSelected(index))
            clipboard.clearSelection() // as in Explorer: the menu is for this row alone
        list.currentIndex = index
        const fromKeyboard = globalPos === undefined
        if (fromKeyboard) {
            list.positionViewAtIndex(index, ListView.Contain)
            globalPos = list.currentItem.mapToGlobal(56, rowHeight - 6)
        }
        menuCategory = -1
        menuLoader.active = true
        const menu = menuLoader.item as ContextMenu
        menu.popup(index, clipboard.items.idAt(index), clipboard.menuItems(index), globalPos, fromKeyboard)
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
        if (menuOpen && contextMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        const alt = (event.modifiers & Qt.AltModifier) !== 0
        altHeld = alt && !ctrl
        if (event.key === Qt.Key_Alt) {
            event.accepted = true // alone, it would open the window menu
            return
        }
        if (alt && !ctrl && event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            clipboard.quickPaste(event.key - Qt.Key_0, shift)
            event.accepted = true
            return
        }
        if (alt && !ctrl && event.key === Qt.Key_P) {
            togglePreview(list.currentIndex)
            event.accepted = true
            return
        }
        switch (event.key) {
        case Qt.Key_Down: shift ? extendSelection(1) : moveSelection(1); break
        case Qt.Key_Up: shift ? extendSelection(-1) : moveSelection(-1); break
        case Qt.Key_PageDown: shift ? extendSelection(rows) : moveSelection(rows); break
        case Qt.Key_PageUp: shift ? extendSelection(-rows) : moveSelection(-rows); break
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
            else if (previewOpen)
                previewOpen = false
            else
                clipboard.dismiss()
            break
        case Qt.Key_Tab: clipboard.cycleCategory(1); break
        case Qt.Key_Backtab: clipboard.cycleCategory(-1); break
        case Qt.Key_C:
            // Ctrl+C copies text selected in the field; otherwise the entry.
            if (!ctrl || (input.selectedText.length > 0 && selectedCount === 0))
                return
            clipboard.copy(targetRow())
            break
        case Qt.Key_P:
            if (!ctrl)
                return
            clipboard.togglePin(targetRow())
            break
        case Qt.Key_Delete:
            // Deletes an entry only where it would delete no typed text.
            if (ctrl || shift || input.selectedText.length > 0 || input.cursorPosition < input.text.length)
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
            if (list.count > 0 && (list.currentIndex < 0 || list.currentIndex >= list.count))
                list.currentIndex = 0
            page.previewRevision++
        }
        function onCountChanged() { page.previewRevision++ }
    }

    Connections {
        target: page.clipboard
        function onShown() {
            input.text = ""
            page.selectionAnchor = -1
            page.previewOpen = false
            page.selectFirst()
            page.focusSearch()
        }
        function onListChanged() {
            page.selectionAnchor = -1
            page.closeContextMenu()
            page.selectFirst()
        }
        function onQueryChanged() {
            if (input.text !== page.clipboard.query)
                input.text = page.clipboard.query
        }
        function onContextMenuKeyPressed() { page.openContextMenu(list.currentIndex) }
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

    Column {
        width: page.width

        Item { // header: drags the window, except for the field
            id: header
            width: parent.width
            height: 64

            Component.onCompleted: {
                page.frame.addDragArea(header)
                page.frame.addControl(input)
            }

            Glyph {
                id: icon
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                glyph: "" // Paste
                size: 22
            }

            TextInput {
                id: input
                anchors.left: icon.right
                anchors.leftMargin: 14
                anchors.right: pausedTag.visible ? pausedTag.left : parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                focus: true
                clip: true
                color: Theme.text
                font.pixelSize: Theme.fontSearch
                selectByMouse: true
                selectionColor: Theme.textSelection
                selectedTextColor: Theme.text
                onTextChanged: page.clipboard.query = text
                Keys.onPressed: (event) => page.handleKey(event)
                Keys.onReleased: (event) => {
                    if (event.key === Qt.Key_Alt) {
                        page.altHeld = false
                        event.accepted = true
                    }
                }

                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("搜索剪贴板，支持拼音")
                    color: Theme.faint
                    font: input.font
                    elide: Text.ElideRight
                    visible: input.text.length === 0 && input.preeditText.length === 0
                }
            }

            Rectangle { // paused from the tray menu
                id: pausedTag
                visible: page.clipboard.paused
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                width: pausedText.implicitWidth + 20
                height: 26
                radius: 13
                color: Theme.badge

                Text {
                    id: pausedText
                    anchors.centerIn: parent
                    text: qsTr("已暂停记录")
                    color: Theme.accent
                    font.pixelSize: Theme.fontCaption
                }
            }
        }

        ClipTabs {
            id: tabs
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

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.divider
        }

        Item {
            id: body
            width: parent.width
            height: page.rows * page.rowHeight + 8

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
                    onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
                    onCountChanged: {
                        if (count > 0 && (currentIndex < 0 || currentIndex >= count))
                            currentIndex = Math.min(Math.max(0, currentIndex), count - 1)
                    }

                    delegate: ClipRow {
                        id: clipRow
                        width: list.width
                        height: page.rowHeight
                        hint: page.altHeld && index < 9 ? index + 1 : 0
                        previewing: page.previewOpen && clipRow.current
                        // On whole device pixels wherever the list has scrolled
                        // to (see Main.qml).
                        transform: Translate {
                            y: {
                                const dpr = Screen.devicePixelRatio
                                const listTop = page.y + body.y + list.y
                                const deviceY = (listTop + list.contentItem.y + clipRow.y) * dpr
                                return (Math.round(deviceY) - deviceY) / dpr
                            }
                        }
                        onClicked: (index, modifiers) => {
                            const ctrl = (modifiers & Qt.ControlModifier) !== 0
                            if (modifiers & Qt.ShiftModifier) {
                                page.selectTo(index, ctrl)
                            } else if (ctrl) {
                                page.toggleRow(index)
                            } else {
                                list.currentIndex = index
                                page.clipboard.paste(index, false) // a picked row pastes all the picked ones
                            }
                        }
                        onPreviewRequested: (index) => page.togglePreview(index)
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
                        font.pixelSize: Theme.fontTitle
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
                        font.pixelSize: Theme.fontBody
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
                width: 1
                height: parent.height
                color: Theme.divider
            }

            ClipPreview {
                visible: page.previewOpen
                onCopyRequested: (text, remember) => page.clipboard.copyText(text, remember)
                x: listArea.width + 1
                width: parent.width - x
                height: parent.height
                parentTop: page.y + body.y
                // Re-read when the rows change. The revision is part of the
                // condition: a bare read of it would be dropped by the QML
                // compiler, and the binding would not depend on it.
                info: page.previewRevision >= 0 && list.count > 0 && list.currentIndex >= 0
                      ? page.clipboard.preview(list.currentIndex) : ({})
            }
        }

        Item { // footer: status or how picked rows are joined, and key hints; drags the window
            id: footer
            width: parent.width
            implicitHeight: 42
            height: implicitHeight

            Component.onCompleted: {
                page.frame.addDragArea(footer)
                page.frame.addControl(separatorChip)
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.divider
            }

            Text {
                id: statusLabel
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.right: hints.shown ? hints.left : parent.right
                anchors.rightMargin: hints.shown ? 12 : 16
                anchors.verticalCenter: parent.verticalCenter
                visible: page.selectedCount === 0
                text: page.clipboard.statusText
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.fontBody
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
                    font.pixelSize: Theme.fontBody
                }
                Rectangle {
                    id: separatorChip
                    visible: page.selectedCount >= 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: separatorText.implicitWidth + 20
                    height: 26
                    radius: 13
                    color: separatorArea.containsMouse ? Theme.chipHover : Theme.track
                    border.width: 1
                    border.color: Theme.chipBorder

                    Text {
                        id: separatorText
                        anchors.centerIn: parent
                        text: page.clipboard.separatorName
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
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
                    font.pixelSize: Theme.fontBody
                }
            }

            Row {
                id: hints
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14

                // Room for them all, unless several picked (and what they are
                // joined with) take it.
                readonly property bool roomy: page.selectedCount < 2 || page.previewOpen
                // Out of the way of a message too long to share the line with
                // them ("已删除 1 条，按 Ctrl+Z 撤销"), while it shows.
                readonly property bool shown: page.selectedCount > 0
                                              || statusLabel.implicitWidth <= footer.width - 18 - 12 - 16 - implicitWidth
                opacity: shown ? 1 : 0 // still laid out: its width tells when it fits again

                KeyHint { keys: "Enter"; label: page.pasteLabel }
                KeyHint { keys: "Shift+Enter"; label: qsTr("纯文本"); visible: !page.intoField && hints.roomy } // text there either way
                KeyHint { keys: "Esc"; label: qsTr("返回"); visible: page.intoField && hints.roomy }
                KeyHint { keys: "Tab"; label: qsTr("分类"); visible: page.selectedCount < 2 }
            }
        }
    }

    HoverTip {
        target: separatorArea.containsMouse ? separatorChip : null
        text: qsTr("点一下换一种：换行、空格、逗号、Tab、不分隔")
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
