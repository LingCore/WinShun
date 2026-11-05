import QtQuick
import WinShun

// Logo controls, the input field and the scope switcher. The rest of it drags
// the window (see WindowFrame).
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

    function reportField() {
        if (inBox)
            launcher.reportField(input.text, input.cursorPosition, input.selectionStart, input.selectionEnd,
                                 input.preeditText)
    }
    onInBoxChanged: reportField()

    Connections {
        target: bar.launcher
        function onCaretRequested(position, word) {
            input.cursorPosition = position
            if (word)
                input.selectWord()
        }
        function onSelectionRequested(anchor, position) { input.select(anchor, position) }
    }

    implicitHeight: Theme.headerHeight // 界面大小

    Component.onCompleted: {
        frame.addDragArea(bar)
        frame.addControl(input)
        frame.addControl(tabs)
        frame.addControl(expand)
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

        readonly property bool pointed: contains(mapFromItem(null, bar.frame.pointer))
        readonly property bool held: bar.placement.moving

        visible: !bar.inBox
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        width: 72 // easier to find than the pill itself
        height: 14

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 6 // even: on whole device pixels at 150 %
            width: grip.held || grip.pointed ? 48 : 36
            height: 4
            radius: Theme.progressRadius
            color: grip.held ? Theme.accent : grip.pointed ? Theme.subtext : Theme.faint
            opacity: grip.held || grip.pointed ? 1 : bar.hovered ? 0.7 : 0
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
        readonly property string help: qsTr("左键：设置 · 中键：%1\n右键：窗口居中 · 拖动：移动窗口")
            .arg(checked ? qsTr("取消钉住") : qsTr("钉住"))

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
        target: handle.pointed && !bar.placement.moving ? handle : null
        text: handle.help
        shortcut: "Ctrl+, · Ctrl+T"
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
        anchors.right: regexChip.visible ? regexChip.left : tabs.left
        anchors.rightMargin: regexChip.visible ? 8 : 12
        y: bar.onPixel((bar.height - height) / 2 + fieldPart.out("contentOffsetY", 0))
        focus: true
        clip: true
        color: fieldPart.out("textColor", Theme.text)
        font.pixelSize: Theme.uiSearch + Theme.tok("searchFontDelta", 0)
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
            bar.launcher.query = text
            bar.reportField()
        }
        onTextEdited: bar.typed()
        onPreeditTextChanged: bar.reportField()
        onCursorPositionChanged: bar.reportField()
        onSelectionStartChanged: bar.reportField()
        onSelectionEndChanged: bar.reportField()
        Keys.onPressed: (event) => bar.keyPressed(event)
        Keys.onReleased: (event) => bar.keyReleased(event)

        Text {
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            text: bar.launcher.placeholder
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

    Item {
        id: regexChip

        visible: bar.launcher.regexMode && bar.launcher.scope !== Launcher.Content && !bar.inBox
        anchors.right: tabs.left
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
    Item {
        id: expand

        readonly property bool tipWanted: expandArea.containsMouse
        readonly property string tip: bar.panelMode ? qsTr("收回搜索框") : qsTr("展开为搜索面板")

        anchors.right: parent.right
        anchors.rightMargin: bar.keysRight
        y: bar.onPixel((bar.height - height) / 2)
        width: height
        height: bar.keyHeight
        Accessible.role: Accessible.Button
        Accessible.name: tip

        FieldKey {
            id: expandKey
            dpr: bar.dpr // (an inline component cannot see bar)
            flat: true
            hovered: expandArea.containsMouse
            pressed: expandArea.pressed
        }

        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: expandKey.out("contentOffsetY", 0)
            glyph: bar.panelMode ? "\uE73F" : "\uE740" // BackToWindow, FullScreen
            size: 24 / bar.dpr // fixed 24 device pixels
            color: expandKey.out("foreground", expandArea.containsMouse ? Theme.text : Theme.subtext)
        }
        MouseArea {
            id: expandArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: bar.launcher.togglePanel()
        }
        HoverTip {
            target: expand.tipWanted ? expand : null
            text: expand.tip
            shortcut: "Ctrl+E"
        }
    }

    ScopeTabs {
        id: tabs
        scaled: true
        role: "scope"
        dpr: bar.dpr
        height: Math.min(tabs.implicitHeight, fieldPart.height - 2 * Math.min(bar.keyInset, Theme.tok("scopeKeyInset", bar.keyInset)))
        anchors.right: expand.left
        anchors.rightMargin: 6
        anchors.leftMargin: 16
        y: bar.onPixel((bar.height - height) / 2)
        // 文件夹 is a chip of the strip under the box (TypeStrip): 文件 stays current.
        current: bar.launcher.scope === Launcher.Folders ? Launcher.Files : bar.launcher.scope
        contentTab: bar.launcher.contentEnabled
        // 全部 › [icon] 结束进程 while a plugin's keyword has the list; the
        // chip (or 全部 itself) empties the box, which leaves the mode.
        pluginLabel: bar.launcher.pluginLabel
        pluginIcon: bar.launcher.pluginIcon
        onActivated: (scope) => {
            if (scope === Launcher.All && bar.launcher.scope === Launcher.All && bar.launcher.pluginKeyword.length > 0)
                bar.launcher.leaveKeyword()
            else if (scope !== Launcher.Files || bar.launcher.scope !== Launcher.Folders) // 文件 in 文件夹: stays
                bar.launcher.scope = scope
        }
        onPluginClicked: bar.launcher.leaveKeyword()
    }
}
