pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// By an Open or Save dialog of another program (see DialogBar): a box to
// search folders and files, and the folder shown in File Explorer to go to
// (or, after the dialog went there by itself, the one it came from). Picking
// one takes the dialog there. A path typed lists what is in that folder: Tab
// goes into the folder of a row, Shift+Tab up one. The list opens while the
// box has the focus; it sits below the box, or above it where there is more
// room. Right-click or "more": the menus (pin a folder, hide the bar).
Window {
    id: window

    required property DialogBar bar

    readonly property int listHeight: bar.rows > 0 ? bar.rows * bar.rowHeight + 9 : 0
    readonly property bool typed: bar.query.trim().length > 0
    readonly property ContextMenu contextMenu: menuLoader.item as ContextMenu
    readonly property bool menuOpen: contextMenu !== null && contextMenu.visible

    // On top: it shows only while the dialog (or the bar) is in front, and
    // Win顺 has no right to bring a window of its own above others then.
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: Theme.background
    title: qsTr("Win顺")

    onActiveChanged: {
        if (active) {
            input.forceActiveFocus() // clicked, or double Ctrl
        } else {
            closeMenu()
            hideNumbers()
        }
    }
    // Hidden: also free the menu window until it is needed again.
    onVisibleChanged: {
        if (!visible) {
            closeMenu()
            menuLoader.active = false
        }
    }

    Binding {
        target: window.bar.results
        property: "highlightColor"
        value: Theme.accent
    }

    function move(delta) {
        if (list.count > 0)
            list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + delta))
    }

    // A row's menu, or the bar's own (index -1). `globalPos` is where the
    // mouse was; without it (Menu key, Shift+F10) under the row or the "⋯".
    function openMenu(index, globalPos) {
        const fromKeyboard = globalPos === undefined
        if (index >= 0) {
            if (index >= list.count)
                return
            list.currentIndex = index
            if (fromKeyboard) {
                list.positionViewAtIndex(index, ListView.Contain)
                globalPos = list.currentItem.mapToGlobal(56, window.bar.rowHeight - 6)
            }
        } else if (fromKeyboard) {
            globalPos = moreButton.mapToGlobal(moreButton.width - 240, moreButton.height + 2)
        }
        menuLoader.active = true
        // Not a line starting with "(": it would continue the line above.
        const menu = menuLoader.item as ContextMenu
        menu.popup(index, index >= 0 ? window.bar.results.pathAt(index) : "", window.bar.menuItems(index), globalPos,
                   fromKeyboard)
    }

    function closeMenu() {
        if (contextMenu)
            contextMenu.dismiss()
    }

    // Ctrl+1… (or Alt+1…) as in the launcher (see Main.qml): the nth row
    // shown, its number up while the modifier is held.
    readonly property int numberKey: bar.numberKeys === "ctrl" ? Qt.Key_Control
                                   : bar.numberKeys === "alt" ? Qt.Key_Alt : -1
    readonly property int firstShownRow: Math.max(0, Math.floor((list.contentY + bar.rowHeight / 2) / bar.rowHeight))
    readonly property int lastShownRow: Math.min(list.count - 1, firstShownRow + 8,
                                                 Math.floor((list.contentY + list.height - bar.rowHeight / 2) / bar.rowHeight))
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
        return showNumbers && list.visible && index >= firstShownRow && index <= lastShownRow
            ? index - firstShownRow + 1 : 0
    }
    function openNumber(number) {
        const row = firstShownRow + number - 1
        if (number < 1 || row > lastShownRow || !list.visible)
            return
        hideNumbers()
        list.currentIndex = row
        window.bar.choose(row)
    }
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
        hideNumbers()
        if (isNumberChord(event) && input.preeditText.length === 0) {
            window.bar.pressNumber(event.key - Qt.Key_0)
            event.accepted = true
            return
        }
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        const row = list.count > 0 ? list.currentIndex : -1
        switch (event.key) {
        case Qt.Key_Down: move(1); break
        case Qt.Key_Up: move(-1); break
        case Qt.Key_PageDown: move(Math.max(1, window.bar.rows)); break
        case Qt.Key_PageUp: move(-Math.max(1, window.bar.rows)); break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (window.bar.holdUntilShown(0, ctrl))
                break // typed a moment ago: on the rows for it
            if (row >= 0)
                window.bar.choose(row, ctrl) // Ctrl: and open the file
            break
        case Qt.Key_Tab: // into the row's folder; the focus stays in the box
            if (row >= 0)
                window.bar.enter(row)
            break
        case Qt.Key_Backtab:
            window.bar.up()
            break
        case Qt.Key_C:
            if (!ctrl || !shift)
                return // Ctrl+C copies the text in the box
            if (row >= 0)
                window.bar.trigger(row, DialogBar.CopyPath)
            break
        case Qt.Key_Escape:
            if (input.text.length > 0)
                input.text = ""
            else
                window.bar.back()
            break
        default:
            return
        }
        event.accepted = true
    }

    Connections {
        target: window.bar
        function onNumberPressed(number) { window.openNumber(number) }
        function onHeldKey(number, open) {
            if (number > 0)
                window.openNumber(number)
            else if (list.count > 0 && list.currentIndex >= 0)
                window.bar.choose(list.currentIndex, open)
        }
        function onResultsReplaced() {
            // A row's menu goes when its row now holds another; the bar's own stays.
            if (window.menuOpen && window.contextMenu.row >= 0
                    && window.bar.results.pathAt(window.contextMenu.row) !== window.contextMenu.key)
                window.closeMenu()
            if (window.menuOpen)
                return // its row stays the current one
            list.currentIndex = list.count > 0 ? 0 : -1
            list.positionViewAtBeginning()
        }
        function onQueryChanged() {
            if (input.text !== window.bar.query)
                input.text = window.bar.query
        }
        function onContextMenuKeyPressed() {
            window.openMenu(window.bar.rows > 0 && list.currentIndex >= 0 ? list.currentIndex : -1)
        }
    }

    Item { // the box
        id: box
        y: window.bar.listAbove ? window.listHeight : 0
        width: window.width
        height: window.bar.barHeight

        Glyph {
            id: searchIcon
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            glyph: "" // Search
            size: 16
        }

        TextInput {
            id: input
            anchors.left: searchIcon.right
            anchors.leftMargin: 10
            anchors.right: trailing.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            focus: true
            clip: true
            color: Theme.text
            font.pixelSize: Theme.fontBody
            selectByMouse: true
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            onTextChanged: window.bar.query = text
            onPreeditTextChanged: window.bar.composing = preeditText.length > 0
            Keys.onPressed: (event) => window.handleKey(event)
            Keys.onReleased: (event) => window.handleKeyRelease(event)
            Accessible.role: Accessible.EditableText
            Accessible.name: placeholder.text

            Text {
                id: placeholder
                anchors.fill: parent
                verticalAlignment: Text.AlignVCenter
                text: window.bar.foldersOnly ? qsTr("搜索文件夹，或输入路径") : qsTr("搜索文件或文件夹，或输入路径")
                color: Theme.faint
                font: input.font
                elide: Text.ElideRight
                visible: input.text.length === 0 && input.preeditText.length === 0
            }
        }

        MouseArea { // right-click: the bar's menu; the left button reaches the box
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: (mouse) => window.openMenu(-1, mapToGlobal(mouse.x, mouse.y))
        }

        Row { // that nothing was found, or a folder to go to; the menu
            id: trailing
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            Text {
                id: notFound
                anchors.verticalCenter: parent.verticalCenter
                visible: window.bar.nothingFound && window.typed
                rightPadding: 8
                text: qsTr("没有找到")
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
            }

            Rectangle {
                id: chip

                // After the dialog went to the file manager's folder by
                // itself: back to where it was; else to that folder.
                readonly property bool toOrigin: window.bar.originName.length > 0

                visible: !notFound.visible && (toOrigin || window.bar.explorerName.length > 0)
                anchors.verticalCenter: parent.verticalCenter
                width: chipRow.implicitWidth + 16
                height: 32
                radius: 6
                color: chipArea.pressed ? Theme.controlPressed : chipArea.containsMouse ? Theme.hover : "transparent"

                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: 8

                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: chip.toOrigin ? "" : "" // Back, Folder
                        size: 16
                        color: Theme.accent
                    }
                    Text {
                        id: chipText
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, Math.max(80, window.width * 0.3))
                        text: chip.toOrigin ? qsTr("回到“%1”").arg(window.bar.originName) : window.bar.explorerName
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                    Rectangle { // the key that does the same; not where the box would be too short (beside the dialog)
                        visible: !chip.toOrigin && window.width >= 560
                        anchors.verticalCenter: parent.verticalCenter
                        width: keyText.implicitWidth + 10
                        height: 20
                        radius: 4
                        color: Theme.keycap
                        border.width: 1
                        border.color: Theme.keycapBorder

                        Text {
                            id: keyText
                            anchors.centerIn: parent
                            text: "Ctrl+G"
                            color: Theme.subtext
                            font.pixelSize: 12
                        }
                    }
                }

                function press() {
                    if (toOrigin)
                        window.bar.goBack()
                    else
                        window.bar.chooseExplorer()
                }

                Accessible.role: Accessible.Button
                Accessible.name: toOrigin ? chipText.text
                                          : qsTr("转到%1中的“%2”").arg(window.bar.explorerSource).arg(window.bar.explorerName)
                Accessible.onPressAction: press()

                MouseArea {
                    id: chipArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: chip.press()
                }
            }

            Rectangle {
                id: moreButton
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                height: 32
                radius: 6
                color: moreArea.pressed ? Theme.controlPressed
                     : moreArea.containsMouse || (window.menuOpen && window.contextMenu.row < 0) ? Theme.hover
                     : "transparent"

                Glyph {
                    anchors.centerIn: parent
                    glyph: "" // More
                    size: 16
                }

                function press() {
                    window.openMenu(-1, mapToGlobal(width - 240, height + 2))
                }

                Accessible.role: Accessible.ButtonMenu
                Accessible.name: qsTr("更多")
                Accessible.onPressAction: press()

                MouseArea {
                    id: moreArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: moreButton.press()
                }
            }
        }
    }

    Rectangle {
        visible: window.bar.rows > 0
        y: window.bar.listAbove ? window.listHeight - 1 : window.bar.barHeight
        width: window.width
        height: 1
        color: Theme.divider
    }

    ListView {
        id: list
        visible: window.bar.rows > 0
        y: window.bar.listAbove ? 0 : window.bar.barHeight + 1
        width: window.width
        height: Math.max(0, window.listHeight - 1)
        topMargin: 4
        bottomMargin: 4
        clip: true
        model: window.bar.results
        boundsBehavior: Flickable.StopAtBounds
        highlightMoveDuration: 0
        onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
        Accessible.role: Accessible.List
        Accessible.name: placeholder.text

        delegate: DialogBarRow {
            id: rowItem
            width: list.width
            height: window.bar.rowHeight
            showModified: window.bar.showModified
            hint: window.numberOf(index)
            // On whole device pixels wherever the list has scrolled to (see
            // Main.qml). Below the bar the rows start at 49 logical pixels:
            // at 150 % on a half pixel even before any scrolling.
            transform: Translate {
                y: {
                    const dpr = Screen.devicePixelRatio
                    const deviceY = (list.y + list.contentItem.y + rowItem.y) * dpr
                    return (Math.round(deviceY) - deviceY) / dpr
                }
            }
            // On the current row while typing: a key that does more with it.
            tag: {
                if (window.typed && rowItem.current) {
                    if (!rowItem.isDir && window.bar.canOpen)
                        return qsTr("Ctrl+Enter 直接打开")
                    if (rowItem.isDir && rowItem.path !== window.bar.browsedFolder)
                        return qsTr("Tab 展开")
                }
                return window.bar.tags[rowItem.index] ?? ""
            }
            onClicked: (index) => window.bar.choose(index)
            onMenuRequested: (index, globalPos) => window.openMenu(index, globalPos)
        }
    }

    ListScrollBar {
        visible: window.bar.rows > 0
        flickable: list
        x: window.width - width
        y: list.y
        height: list.height
    }

    // While the menu is open, a press anywhere else closes it (a right click
    // on a row then opens that row's).
    MouseArea {
        anchors.fill: parent
        enabled: window.menuOpen
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            window.closeMenu()
            mouse.accepted = mouse.button !== Qt.RightButton
        }
        onWheel: (wheel) => window.closeMenu()
    }

    Loader {
        id: menuLoader
        active: false
        sourceComponent: ContextMenu {
            host: window.bar
            onTriggered: (row, action) => window.bar.trigger(row, action)
        }
    }

    WindowEdge {}
}
