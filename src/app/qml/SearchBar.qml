import QtQuick
import WinShun

// Only the visible logo and grip move the window; the field owns its whitespace.
Item {
    id: bar

    required property Launcher launcher
    required property Placement placement
    required property WindowFrame frame
    property alias text: input.text
    readonly property bool hasSelection: input.selectedText.length > 0
    readonly property string composition: input.preeditText // an input method's, not typed yet
    readonly property Item field: input // what the clipboard pastes into
    // Opened over Win顺's box on the taskbar: the field is drawn there (its
    // state reported, see Launcher.inTaskbarBox), here only the tabs show.
    // The field stays, out of sight, with the keyboard.
    readonly property bool inBox: launcher.inTaskbarBox
    // The search panel's own bar (Panel.qml): the button at the right
    // corner collapses the panel instead of expanding the box into it.
    property bool panelMode: false
    property bool minimal: false
    // 纯净搜索模式: the box keeps its scope tabs and loses the two keys right
    // of them, the search history and the panel, with words typed too; their
    // shortcuts stay (GitHub #185, #244). Not the panel's own bar (the way
    // back from it), a folder's or the taskbar box's, which the mode leaves alone.
    // One key for the scopes in place of the tabs: a folder's box always (it
    // is narrow), the launcher's where the settings ask for it (GitHub
    // #185, #244). The panel has the room and keeps its tabs; the taskbar
    // box's bar too.
    readonly property bool scopeKey: bar.folderMode || (bar.launcher.scopeButton && !bar.panelMode && !bar.inBox)
    readonly property bool pureKeys: bar.launcher.pureSearch && !bar.panelMode && !bar.folderMode && !bar.inBox
    readonly property bool menuOpen: headerMenu.item !== null && headerMenu.item.visible
    readonly property string positionAction: panelMode && launcher.logoRightClick === "top" ? "center" : launcher.logoRightClick
    readonly property string positionActionLabel: positionAction === "top" ? qsTr("回到靠上位置")
        : positionAction === "center" ? qsTr("居中")
        : positionAction === "restore" ? qsTr("恢复上次拖动位置") : qsTr("位置菜单")
    function closeMenu() { if (headerMenu.item) headerMenu.item.dismiss() }
    function menuKey(event) { return menuOpen && headerMenu.item.handleKey(event) }
    function activateLogoPosition() {
        if (positionAction === "menu") { showPositionMenu(); return }
        closeMenu()
        if (positionAction === "top") placement.moveHome()
        else if (positionAction === "center") placement.centerOnScreen()
        else if (positionAction === "restore") placement.restoreDraggedPosition()
        focusAtEnd()
    }
    function showPositionMenu() {
        if (menuOpen) { closeMenu(); return }
        headerMenu.active = true
        const items = [
            { text: qsTr("靠上"), action: 0, enabled: !bar.panelMode },
            { text: qsTr("居中"), action: 1 },
            { text: qsTr("恢复上次拖动位置"), action: 2 }
        ]
        headerMenu.item.popup(-1, "", items, handle.mapToGlobal(0, handle.height + 4), false)
    }
    Loader {
        id: headerMenu
        active: false
        sourceComponent: ContextMenu {
            objectName: bar.panelMode ? "panelHeaderMenu" : "searchHeaderMenu"
            host: bar.launcher
            onTriggered: (row, action) => {
                if (action === 0) bar.placement.moveHome()
                else if (action === 1) bar.placement.centerOnScreen()
                else bar.placement.restoreDraggedPosition()
                bar.focusAtEnd()
            }
        }
    }
    Connections {
        target: bar.placement
        function onMovingChanged() { if (bar.placement.moving) bar.closeMenu() }
    }
    Connections {
        target: bar.launcher
        function onLogoRightClickChanged() { bar.closeMenu() }
    }
    Connections {
        target: bar.Window.window
        function onVisibleChanged() { if (!bar.Window.window.visible) bar.closeMenu() }
    }
    onVisibleChanged: if (!visible) { historyPicker.close(); closeMenu() }
    readonly property bool folderMode: !panelMode && launcher.folderSearch
    readonly property string hiddenPrefix: folderMode ? launcher.typingPrefix : ""
    property bool syncingQuery: false
    function syncQuery() {
        const query = launcher.query
        const shown = hiddenPrefix.length > 0 && query.toLowerCase().startsWith(hiddenPrefix.toLowerCase())
                      ? query.slice(hiddenPrefix.length) : query
        if (input.text === shown)
            return
        syncingQuery = true
        input.text = shown
        syncingQuery = false
    }
    onHiddenPrefixChanged: syncQuery()
    // The pointer over the header: Windows takes its drag area as a title bar,
    // so QML hears of the pointer there only from the frame.
    readonly property bool hovered: hover.hovered || contains(mapFromItem(null, frame.pointer))

    signal keyPressed(var event)
    signal keyReleased(var event)
    signal editingRequested()
    signal typed() // the text was edited by the user (not set by the launcher)

    // On whole device pixels, as the rows' text (ResultRow.qml): centred, at
    // 150 % the field's text sits on a half pixel, and while the window slides
    // in (Placement.contentShift) its glyphs come out a pixel taller on some
    // frames than when it rests.
    function onPixel(y) { return Math.round(y * dpr) / dpr }
    // The window's, read once by the host (GitHub #111).
    property real dpr: Screen.devicePixelRatio

    // The style's field (Theme.searchField, flat: the launcher's row): inset
    // from the row by fieldInset (0 in fluent: the whole row, nothing drawn),
    // and the keys in it (pin, expand, .*, the scopes) inset from its top,
    // bottom and right; never taller than they are, nor nearer the right
    // edge than 10.
    readonly property real keyInset: Theme.fieldKeyInsetAt(dpr)
    readonly property real keyHeight: Math.min(tabs.implicitHeight, fieldPart.height - 2 * keyInset)
    readonly property real keysRight: Math.max(10, Theme.fieldInset + keyInset)

    Hook {
        id: fieldPart
        anchors.fill: parent
        anchors.margins: Theme.fieldInset
        part: Theme.searchField
        flat: true
        focused: input.activeFocus
        hovered: bar.hovered
        corner: Theme.fieldRadius
        dpr: bar.dpr
    }

    function focusAndSelect() {
        input.forceActiveFocus()
        input.selectAll()
    }
    // The keyboard back in the field, the caret after the words (from the
    // panel's filters; the other bar on expanding or collapsing).
    function focusAtEnd() {
        input.forceActiveFocus()
        input.cursorPosition = input.length
    }
    function handleKey(event) {
        if (input.preeditText.length === 0 && historyPicker.handleKey(event)) { event.accepted = true; return }
        const modifier = launcher.historyKeys === "alt" ? Qt.AltModifier : Qt.ControlModifier
        if (!folderMode && launcher.queryHistoryEnabled && launcher.historyKeys !== "off" && input.preeditText.length === 0
                && event.modifiers === modifier && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
            launcher.navigateQueryHistory(event.key === Qt.Key_Up ? 1 : -1)
            event.accepted = true
            return
        }
        keyPressed(event)
    }

    function reportField() {
        if (inBox)
            launcher.reportField(input.text, input.cursorPosition, input.selectionStart, input.selectionEnd,
                                 input.preeditText)
    }
    onInBoxChanged: reportField()

    Connections {
        target: bar.launcher
        function onCaretRequested(position, word) {
            input.cursorPosition = Math.max(0, position - bar.hiddenPrefix.length)
            if (word)
                input.selectWord()
        }
        function onSelectionRequested(anchor, position) {
            input.select(Math.max(0, anchor - bar.hiddenPrefix.length), Math.max(0, position - bar.hiddenPrefix.length))
        }
    }

    implicitHeight: folderMode ? 44 : Theme.headerHeight

    Component.onCompleted: {
        frame.addDragArea(handle)
        frame.addDragHandle(grip)
        frame.addControl(input)
        frame.addControl(tabs)
        frame.addControl(compactScope)
        frame.addControl(expand)
        frame.addControl(sidebarToggle)
        frame.addControl(historyButton)
        frame.addControl(regexChip)
        frame.addControl(boxPin)
        frame.addHandle(handle)
    }

    HoverHandler { id: hover } // over the field and the tabs

    states: State {
        when: bar.inBox
        AnchorChanges {
            target: input
            anchors.right: undefined
        }
        PropertyChanges {
            input.width: 1 // still takes the keys; too thin to be clicked
            input.opacity: 0
        }
        AnchorChanges {
            target: tabs
            anchors.right: undefined
            anchors.left: bar.left
        }
    }

    // Grip: shows that the header moves the window. Faint while the pointer is
    // over the header; pointed at, it grows and brightens; while the window
    // moves, it takes the accent colour.
    Item {
        id: grip
        objectName: bar.panelMode ? "panelSearchGrip" : "searchGrip"

        readonly property bool pointed: contains(mapFromItem(null, bar.frame.pointer))
        readonly property bool held: bar.placement.moving

        visible: !bar.inBox
        anchors.top: parent.top
        anchors.topMargin: Theme.fieldInset > 0 ? 0 : 4
        anchors.horizontalCenter: parent.horizontalCenter
        width: 48
        height: 10

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 4
            width: grip.held || grip.pointed ? 48 : 36
            height: 4
            radius: Theme.progressRadius
            color: grip.held ? Theme.accent : grip.pointed ? Theme.subtext : Theme.faint
            opacity: grip.held || grip.pointed ? 1 : 0.5
            Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
            Behavior on color { ColorAnimation { duration: 150 } }
            Behavior on opacity { NumberAnimation { duration: 150 } }
        }
    }

    // A native drag handle with three click actions (Main.qml). The pin
    // badge and selected background make its persistent state visible.
    Rectangle {
        id: handle
        objectName: bar.panelMode ? "panelSearchLogo" : "searchLogo"

        readonly property bool pointed: visible && contains(mapFromItem(null, bar.frame.pointer))
        readonly property bool checked: bar.launcher.pinned
        readonly property string help: qsTr("左键：设置 · 中键：%1\n右键：%2 · 拖动：移动窗口\n双击横条：展开／收回搜索面板")
            .arg(checked ? qsTr("取消保持显示") : qsTr("保持显示"))
            .arg(bar.positionActionLabel)

        visible: !bar.inBox
        anchors.left: parent.left
        // Its icon where the style puts the field's icon (iconInset, from the
        // field's left edge); 14 in a style that says nothing (fluent).
        readonly property real styleInset: fieldPart.out("iconInset", -1)
        anchors.leftMargin: styleInset >= 0 ? Theme.snap(Theme.fieldInset + styleInset - (width - Theme.searchIconSize) / 2, bar.dpr) : 14
        y: bar.onPixel((bar.height - height) / 2)
        width: Theme.searchIconSize + 12
        height: width
        radius: Theme.controlRadius
        color: checked ? Theme.chipActive : pointed && !bar.placement.moving ? Theme.chipHover : "transparent"
        border.width: checked || activeFocus ? Theme.edge(bar.dpr) : 0
        border.color: activeFocus ? Theme.accent : Theme.chipBorder
        Behavior on color { ColorAnimation { duration: 100 } }
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Win顺：设置")
        Accessible.description: (checked ? qsTr("已钉住") + "\n" : "") + help
        Accessible.onPressAction: bar.launcher.openSettings()
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                bar.launcher.openSettings()
                event.accepted = true
            }
        }

        Image {
            objectName: "searchLogoImage"
            anchors.centerIn: parent
            width: Theme.searchIconSize
            height: width
            source: "qrc:/branding/logo.svg"
            sourceSize.width: width
            sourceSize.height: height
            smooth: false
            Accessible.ignored: true
        }

        Rectangle {
            objectName: "searchLogoPinBadge"
            visible: handle.checked
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: 14
            height: width
            radius: Theme.controlRadius
            color: Theme.chipActive
            Accessible.ignored: true
            Glyph {
                anchors.centerIn: parent
                glyph: "\uE841" // PinnedFill
                overlay: "\uE718"
                size: 12
                color: Theme.accent
            }
        }
    }

    HoverTip {
        objectName: bar.panelMode ? "panelSearchLogoHint" : "searchLogoHint"
        target: handle.pointed && !bar.placement.moving && !bar.menuOpen ? handle : null
        text: handle.help
        shortcut: "Ctrl+, · Ctrl+T"
        host: bar.launcher
    }

    TextInput {
        id: input
        objectName: bar.panelMode ? "panelSearchField" : "searchField" // App answers the input method for it while it is in the box
        // Where the style's words start in its field (leadingInset, from the
        // field's left edge), never nearer the logo than 6; 8 after the logo
        // in a style that says nothing (fluent).
        readonly property real styleStart: fieldPart.out("leadingInset", 0)
        anchors.left: handle.right
        anchors.leftMargin: styleStart > 0 ? Theme.snap(Math.max(6, Theme.fieldInset + styleStart - handle.x - handle.width), bar.dpr) : 8
        anchors.right: bar.minimal ? bar.right : regexChip.visible ? regexChip.left : bar.scopeKey ? compactScope.left : tabs.left
        anchors.rightMargin: regexChip.visible ? 8 : 12
        height: bar.height - grip.y - 2 * grip.height
        y: grip.y + grip.height
        verticalAlignment: TextInput.AlignVCenter
        focus: true
        // Typed into without the focus (keysRouted, GitHub #203): the caret
        // shows where, steady (it blinks only with the focus).
        cursorVisible: activeFocus || bar.launcher.keysRouted
        clip: true
        color: fieldPart.out("textColor", Theme.text)
        font.pixelSize: bar.folderMode ? 16 : Theme.uiSearch + Theme.tok("searchFontDelta", 0)
        selectByMouse: true
        // The selection is drawn below (selectionMark), as tall as the caret:
        // Qt's own is a different height from its caret in a style's field.
        readonly property bool ownMarks: fieldPart.out("caretColor", "transparent").a > 0
        selectionColor: ownMarks ? "transparent" : Theme.textSelection
        selectedTextColor: Theme.text
        FontMetrics { id: markMetrics; font: input.font }
        // Caret and selection: one line of the font tall, centred on the field's middle (not on the words' optical offset).
        readonly property real markHeight: Math.round(markMetrics.height * bar.dpr) / bar.dpr
        readonly property real markY: bar.onPixel((height - markHeight) / 2 - fieldPart.out("contentOffsetY", 0))
        Rectangle {
            id: selectionMark
            z: -1 // under the words
            readonly property string watched: input.text + input.preeditText // the rectangles move with the words
            visible: input.ownMarks && input.selectionStart !== input.selectionEnd
            x: { watched; return input.positionToRectangle(input.selectionStart).x }
            width: { watched; return input.positionToRectangle(input.selectionEnd).x - x }
            y: input.markY
            height: input.markHeight
            color: Theme.textSelection
        }
        // The style's caret colour, where it has one (Qt's own caret otherwise).
        cursorDelegate: ownMarks ? caretPart : null
        Component {
            id: caretPart
            Caret { color: fieldPart.out("caretColor", Theme.text); dpr: bar.dpr; markHeight: input.markHeight; markY: input.markY }
        }
        // Still shown while the clipboard under it has the keyboard: what a
        // paste there goes over.
        persistentSelection: true
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onPressedChanged: if (pressed) bar.editingRequested()
        }
        onTextChanged: {
            if (!bar.syncingQuery)
                bar.launcher.query = bar.hiddenPrefix + text
            bar.reportField()
        }
        onTextEdited: { historyPicker.close(); bar.typed() }
        onPreeditTextChanged: bar.reportField()
        onCursorPositionChanged: bar.reportField()
        onSelectionStartChanged: bar.reportField()
        onSelectionEndChanged: bar.reportField()
        Keys.onPressed: (event) => bar.handleKey(event)
        Keys.onReleased: (event) => bar.keyReleased(event)

        Text {
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            text: bar.folderMode && bar.launcher.typingFolder.length > 0
                  ? qsTr("搜索 %1").arg(bar.launcher.typingFolder) : bar.launcher.placeholder
            color: fieldPart.out("placeholderColor", Theme.faint)
            font: input.font
            elide: Text.ElideRight
            visible: input.text.length === 0 && input.preeditText.length === 0
        }

        Text { // a plugin command's hint, after its keyword, until the words are typed ("kill |进程名或 PID")
            id: keywordHint

            // Where the typed text ends: asked again whenever it changes
            // (positionToRectangle is a call, not a property, so a binding
            // would not follow the text). On displayTextChanged, not
            // textChanged: TextInput emits textChanged before it lays the
            // new text out, and asked then for a text that grew, the old
            // layout has no such position and answers x = 0 (the hint came
            // out over the keyword; pitfalls "TextInput 的 textChanged 早于排版").
            property real textEnd: 0
            function place() { textEnd = input.positionToRectangle(input.length).x }

            // Placed by x, so not anchored on the right (an anchor there would
            // set x itself, see pitfalls "只锚了右边又写了 x"): the width to the edge instead.
            x: textEnd + 2
            width: Math.max(0, parent.width - x)
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            verticalAlignment: Text.AlignVCenter
            text: bar.launcher.pluginHint
            color: fieldPart.out("placeholderColor", Theme.faint)
            font: input.font
            elide: Text.ElideRight
            visible: text.length > 0 && input.preeditText.length === 0 && !bar.inBox

            Connections {
                target: input
                function onDisplayTextChanged() { keywordHint.place() }
                function onFontChanged() { keywordHint.place() }
            }
        }
    }

    // Regex mode is on (Launcher.regexMode, Ctrl+R): ".*" before the tabs,
    // as the pin shows it is on; a click turns it off. Not in 内容, where
    // it changes nothing.
    // The keys in the field: the style's (Theme.keySurface, role "fieldKey");
    // a mode that is on (.*, the pin) is a checked one.
    component FieldKey: Hook {
        anchors.fill: parent
        part: Theme.keySurface
        role: "fieldKey"
    }

    // An icon key at the box's right end: the search history, the panel's
    // filters, expand. One box, one icon size and one colour for them all at
    // any scale (GitHub #279: the history's icon had a size of its own, the
    // filters' key stood filled between two flat ones). `checked`: what it
    // opens is open, told by the icon alone (`checkedGlyph`, in the accent
    // colour); `marked`: at work though closed.
    component IconKey: Item {
        id: key

        property real dpr: 1 // (an inline component cannot see bar)
        property string glyph
        property string checkedGlyph
        property string glyphName // the icon's objectName
        property bool checked: false
        property bool marked: false
        property string tip
        property string shortcut
        readonly property bool tipWanted: keyArea.containsMouse

        signal clicked()
        function activate() { clicked() }

        width: height
        Accessible.role: Accessible.Button
        Accessible.name: tip
        Accessible.onPressAction: activate()
        Keys.onSpacePressed: activate()
        Keys.onReturnPressed: activate()

        FieldKey {
            id: keySurface
            dpr: key.dpr
            flat: true
            hovered: keyArea.containsMouse
            pressed: keyArea.pressed
            focused: key.activeFocus
        }
        Glyph {
            objectName: key.glyphName
            anchors.centerIn: parent
            anchors.verticalCenterOffset: keySurface.out("contentOffsetY", 0)
            inkScale: keySurface.out("contentScale", 1)
            glyph: key.checked && key.checkedGlyph.length > 0 ? key.checkedGlyph : key.glyph
            size: Theme.searchIconSize * 0.8
            color: key.checked || key.marked ? Theme.accent
                 : keySurface.out("foreground", keyArea.containsMouse ? Theme.text : Theme.subtext)
        }
        MouseArea {
            id: keyArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: key.activate()
        }
        HoverTip {
            target: key.tipWanted ? key : null
            text: key.tip
            shortcut: key.shortcut
        }
    }

    Item {
        id: regexChip

        visible: !bar.minimal && bar.launcher.regexMode && bar.launcher.scope !== Launcher.Content && !bar.inBox
        anchors.right: bar.scopeKey ? compactScope.left : tabs.left
        anchors.rightMargin: 6
        y: bar.onPixel((bar.height - height) / 2)
        width: Math.max(height, regexText.implicitWidth + 16)
        height: bar.keyHeight
        Accessible.role: Accessible.CheckBox
        Accessible.name: qsTr("正则模式")
        Accessible.checked: true

        FieldKey {
            id: regexKey
            dpr: bar.dpr // (an inline component cannot see bar)
            checked: true
            flat: false
            hovered: regexArea.containsMouse
            pressed: regexArea.pressed
        }

        Text {
            id: regexText
            anchors.centerIn: parent
            anchors.verticalCenterOffset: regexKey.out("contentOffsetY", 0)
            text: ".*"
            color: regexKey.out("foreground", Theme.accentText)
            font.pixelSize: Theme.uiBody
            font.bold: true
        }
        MouseArea {
            id: regexArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: bar.launcher.toggleRegexMode()
        }
        HoverTip {
            target: regexArea.containsMouse ? regexChip : null
            text: qsTr("正则模式：每个词都按正则表达式匹配名字 · 点击关闭")
            shortcut: "Ctrl+R"
        }
    }

    // The logo, and the pin badge on it, are not there over the taskbar box:
    // a pinned window shows this chip left of the expand button, a click
    // unpins. Nothing while it is not pinned.
    Item {
        id: boxPin

        visible: bar.inBox && bar.launcher.pinned
        anchors.right: expand.left
        anchors.rightMargin: 6
        y: bar.onPixel((bar.height - height) / 2)
        width: height
        height: bar.keyHeight
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("取消钉住")
        Accessible.description: qsTr("已钉住")

        FieldKey {
            id: pinKey
            dpr: bar.dpr // (an inline component cannot see bar)
            checked: true
            flat: false
            hovered: pinArea.containsMouse
            pressed: pinArea.pressed
        }

        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: pinKey.out("contentOffsetY", 0)
            inkScale: pinKey.out("contentScale", 1)
            glyph: "" // PinnedFill
            overlay: ""
            size: 12
            color: pinKey.out("foreground", Theme.accentText)
        }
        MouseArea {
            id: pinArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: bar.launcher.togglePin()
        }
        HoverTip {
            target: pinArea.containsMouse ? boxPin : null
            text: qsTr("已钉住 · 点击取消")
            shortcut: "Ctrl+T"
        }
    }

    // Expands the box into the search panel (Launcher.panel), or collapses
    // it: at the right corner (Ctrl+E too).
    IconKey {
        id: expand
        objectName: bar.panelMode ? "panelCollapseButton" : "searchExpandButton"
        visible: !bar.minimal && !bar.pureKeys
        // Gone with pureKeys, it takes no room either: what is anchored to
        // its left (the scope tabs) moves up to the box's edge.
        width: bar.pureKeys ? 0 : height
        anchors.right: parent.right
        anchors.rightMargin: bar.keysRight
        y: bar.onPixel((bar.height - height) / 2)
        height: bar.keyHeight
        dpr: bar.dpr
        glyph: bar.panelMode ? "\uE73F" : "\uE740" // BackToWindow, FullScreen
        glyphName: "searchExpandGlyph"
        tip: bar.panelMode ? qsTr("收回搜索框") : qsTr("展开为搜索面板")
        shortcut: "Ctrl+E"
        onClicked: bar.launcher.togglePanel()
    }

    QueryHistoryPicker {
        id: historyPicker
        popupName: bar.panelMode ? "panelQueryHistoryPopup" : "queryHistoryPopup"
        launcher: bar.launcher
        frame: bar.frame
        onChosen: bar.focusAtEnd()
    }
    IconKey {
        id: historyButton
        objectName: bar.panelMode ? "panelHistoryButton" : "queryHistoryButton"
        visible: !bar.minimal && !bar.pureKeys && bar.launcher.queryHistoryEnabled && !bar.folderMode && !bar.inBox
        anchors.right: sidebarToggle.visible ? sidebarToggle.left : expand.left
        anchors.rightMargin: 6
        y: bar.onPixel((bar.height - height) / 2)
        height: bar.keyHeight
        dpr: bar.dpr
        glyph: "\uE81C" // History
        tip: qsTr("搜索历史")
        shortcut: bar.launcher.historyKeys === "off" ? "" : (bar.launcher.historyKeys === "alt" ? "Alt" : "Ctrl") + "+↑/↓"
        onClicked: historyPicker.toggle(bar)
    }

    // The panel's filters, shown or put away. Away with some of them set,
    // it says so: the accent colour, and a dot.
    IconKey {
        id: sidebarToggle
        objectName: "panelSidebarToggle"
        visible: bar.panelMode
        anchors.right: expand.left
        anchors.rightMargin: 6
        y: bar.onPixel((bar.height - height) / 2)
        height: bar.keyHeight
        dpr: bar.dpr
        activeFocusOnTab: visible
        glyph: "\uE71C" // Filter
        checkedGlyph: "\uE16E" // the same, filled
        checked: bar.launcher.panelSidebarVisible
        marked: bar.launcher.filtersActive
        tip: bar.launcher.panelSidebarVisible ? qsTr("隐藏筛选")
            : bar.launcher.filtersActive ? qsTr("显示筛选（筛选仍生效）") : qsTr("显示筛选")
        Accessible.checkable: true
        Accessible.checked: bar.launcher.panelSidebarVisible
        onClicked: {
            bar.launcher.panelSidebarVisible = !bar.launcher.panelSidebarVisible
            bar.focusAtEnd()
        }

        Rectangle {
            objectName: "panelActiveFiltersIndicator"
            visible: bar.launcher.filtersActive && !bar.launcher.panelSidebarVisible
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 3
            width: 5
            height: 5
            radius: 2.5
            color: Theme.accent
        }
    }

    Item {
        id: compactScope
        objectName: bar.panelMode ? "panelScopeKey" : "searchScopeKey"
        visible: bar.scopeKey && !bar.minimal
        // A plugin's keyword has the list: its name, and a click leaves it (as the tabs' chip does).
        readonly property bool keyword: bar.launcher.scope === Launcher.All && bar.launcher.pluginKeyword.length > 0
                                        && bar.launcher.pluginLabel.length > 0
        function next() {
            if (keyword)
                bar.launcher.leaveKeyword()
            else
                bar.launcher.cycleScope(1)
        }
        anchors.right: historyButton.visible ? historyButton.left : expand.left
        anchors.rightMargin: 6
        y: bar.onPixel((bar.height - height) / 2)
        width: compactScopeLabel.implicitWidth + 20
        height: bar.keyHeight
        Accessible.role: Accessible.Button
        Accessible.name: compactScopeLabel.text
        Accessible.onPressAction: compactScope.next()

        FieldKey {
            id: compactScopeKey
            dpr: bar.dpr
            flat: true
            hovered: compactScopeArea.containsMouse
            pressed: compactScopeArea.pressed
        }
        Text {
            id: compactScopeLabel
            anchors.centerIn: parent
            text: compactScope.keyword ? bar.launcher.pluginLabel : tabs.labels[tabs.current] ?? ""
            color: compactScopeKey.out("foreground", Theme.subtext)
            font.pixelSize: bar.folderMode ? 12 : Theme.uiBody
        }
        MouseArea {
            id: compactScopeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: compactScope.next()
        }
        HoverTip {
            target: compactScopeArea.containsMouse ? compactScope : null
            text: bar.folderMode ? compactScopeLabel.text : qsTr("切换搜索范围")
            shortcut: bar.folderMode ? "Tab" : "Tab / Shift+Tab"
        }
    }

    ScopeTabs {
        id: tabs
        HoverHandler { id: scopeHover }
        HoverTip { target: scopeHover.hovered ? tabs : null; text: qsTr("切换搜索范围"); shortcut: "Tab / Shift+Tab"; host: bar.launcher }
        objectName: bar.panelMode ? "panelScopeTabs" : "searchScopeTabs"
        visible: !bar.minimal && !bar.scopeKey
        scaled: true
        role: "scope"
        dpr: bar.dpr
        height: Math.min(tabs.implicitHeight, fieldPart.height - 2 * Math.min(bar.keyInset, Theme.tok("scopeKeyInset", bar.keyInset)))
        anchors.right: historyButton.visible ? historyButton.left : sidebarToggle.visible ? sidebarToggle.left : expand.left
        anchors.rightMargin: 6
        anchors.leftMargin: 16
        y: bar.onPixel((bar.height - height) / 2)
        // 文件夹 is a chip of the strip under the box (TypeStrip): 文件 stays current.
        // The tabs' scopes, in their order: 全部, 文件 (unless the two are one), 内容, and last 应用 where
        // the settings ask for it (Launcher.appsTab): 全部 stays first, as its plugin chip needs it.
        readonly property var scopes: {
            const list = [Launcher.All]
            if (!bar.launcher.mergeFileScope)
                list.push(Launcher.Files)
            if (contentTab)
                list.push(Launcher.Content)
            if (bar.launcher.appsTab)
                list.push(Launcher.Apps)
            return list
        }
        current: Math.max(0, scopes.indexOf(bar.launcher.scope === Launcher.Folders ? Launcher.Files : bar.launcher.scope))
        contentTab: bar.launcher.contentEnabled
        labels: scopes.map(scope => scope === Launcher.Files ? qsTranslate("ScopeTabs", "文件")
                                  : scope === Launcher.Content ? qsTranslate("ScopeTabs", "内容")
                                  : scope === Launcher.Apps ? qsTranslate("ScopeTabs", "应用") : qsTranslate("ScopeTabs", "全部"))
        // 全部 › [icon] 结束进程 while a plugin's keyword has the list; the
        // chip (or 全部 itself) empties the box, which leaves the mode.
        pluginLabel: bar.launcher.pluginLabel
        pluginIcon: bar.launcher.pluginIcon
        onActivated: (index) => {
            const scope = tabs.scopes[index]
            if (scope === Launcher.All && bar.launcher.scope === Launcher.All && bar.launcher.pluginKeyword.length > 0)
                bar.launcher.leaveKeyword()
            else if (scope !== Launcher.Files || bar.launcher.scope !== Launcher.Folders) // 文件 in 文件夹: stays
                bar.launcher.scope = scope
        }
        onPluginClicked: bar.launcher.leaveKeyword()
    }
}
