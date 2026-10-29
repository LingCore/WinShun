import QtQuick
import WinShun

// Search icon, the input field and the scope switcher. The rest of it drags
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

    // On whole device pixels, as the rows' text (ResultRow.qml): centred, at
    // 150 % the field's text sits on a half pixel, and while the window slides
    // in (Placement.contentShift) its glyphs come out a pixel taller on some
    // frames than when it rests.
    function onPixel(y) { return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio }

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
        frame.addControl(pin)
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

    // The search icon is a handle (linux.do #157, as Lertaro has it): held
    // and dragged it moves the window, as the rest of the header does;
    // clicked, the settings (WindowFrame.addHandle, handleClicked in
    // Main.qml). Pointed at, a square shows under it and a tip says so.
    // In the drag area, QML hears of the pointer there from the frame alone.
    Rectangle {
        id: handle

        readonly property bool pointed: visible && contains(mapFromItem(null, bar.frame.pointer))

        visible: !bar.inBox
        anchors.horizontalCenter: icon.horizontalCenter
        y: bar.onPixel((bar.height - height) / 2)
        width: Theme.searchIconSize + 12
        height: width
        radius: Theme.controlRadius
        color: pointed && !bar.placement.moving ? Theme.chipHover : "transparent"
        Behavior on color { ColorAnimation { duration: 100 } }
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("设置")
    }

    HoverTip {
        target: handle.pointed && !bar.placement.moving ? handle : null
        text: qsTr("拖动可移动窗口 · 点击打开设置")
    }

    Glyph {
        id: icon
        visible: !bar.inBox
        color: handle.pointed ? Theme.text : Theme.subtext
        anchors.left: parent.left
        anchors.leftMargin: 20
        y: bar.onPixel((bar.height - height) / 2)
        glyph: bar.launcher.scope === Launcher.Content ? "" // Document
             : "" // Search
        size: Theme.searchIconSize
    }

    TextInput {
        id: input
        objectName: bar.panelMode ? "panelSearchField" : "searchField" // App answers the input method for it while it is in the box
        anchors.left: icon.right
        anchors.leftMargin: 14
        anchors.right: tabs.left
        anchors.rightMargin: 12
        y: bar.onPixel((bar.height - height) / 2)
        focus: true
        clip: true
        color: Theme.text
        font.pixelSize: Theme.uiSearch
        selectByMouse: true
        selectionColor: Theme.textSelection
        selectedTextColor: Theme.text
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
            color: Theme.faint
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
            color: Theme.faint
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

    // Pins the launcher (Launcher.pinned, Ctrl+T; linux.do #157): it stays
    // up through clicks elsewhere and opened results, until Esc. Beside the
    // panel's button; on, it shows so: the filled pin in
    // the accent colour on a raised square with an edge (as the footer's
    // chosen chips; a mark in the accent colour, never a fill of it).
    Rectangle {
        id: pin

        readonly property bool checked: bar.launcher.pinned
        readonly property bool tipWanted: pinArea.containsMouse
        readonly property string tip: checked ? qsTr("取消钉住") : qsTr("钉住：点别处、打开文件后都不收起，按 Esc 关闭")

        anchors.right: expand.left
        anchors.rightMargin: 4
        y: bar.onPixel((bar.height - height) / 2)
        width: height
        height: tabs.height
        radius: Theme.controlRadius
        color: pinArea.pressed ? Theme.controlPressed : checked ? Theme.chipActive
             : pinArea.containsMouse ? Theme.chipHover : "transparent"
        border.width: checked ? 1 : 0
        border.color: Theme.chipBorder
        Accessible.role: Accessible.CheckBox
        Accessible.name: qsTr("钉住")
        Accessible.checked: checked

        Glyph {
            anchors.centerIn: parent
            glyph: pin.checked ? "" : "" // PinnedFill, Pin
            overlay: pin.checked ? "" : "" // the needle (lines up with PinnedFill in Fluent and MDL2)
            size: 24 / Screen.devicePixelRatio // fixed 24 device pixels, including on another screen
            color: pin.checked ? Theme.accent : pinArea.containsMouse ? Theme.text : Theme.subtext
        }
        MouseArea {
            id: pinArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: bar.launcher.togglePin()
        }
        HoverTip {
            target: pin.tipWanted ? pin : null
            text: pin.tip
            shortcut: "Ctrl+T"
        }
    }

    // Expands the box into the search panel (Launcher.panel), or collapses
    // it: at the right corner (Ctrl+E too).
    Rectangle {
        id: expand

        readonly property bool tipWanted: expandArea.containsMouse
        readonly property string tip: bar.panelMode ? qsTr("收回搜索框") : qsTr("展开为搜索面板")

        anchors.right: parent.right
        anchors.rightMargin: 10
        y: bar.onPixel((bar.height - height) / 2)
        width: height
        height: tabs.height
        radius: Theme.controlRadius
        color: expandArea.pressed ? Theme.controlPressed : expandArea.containsMouse ? Theme.chipHover : "transparent"
        Accessible.role: Accessible.Button
        Accessible.name: tip

        Glyph {
            anchors.centerIn: parent
            glyph: bar.panelMode ? "\uE73F" : "\uE740" // BackToWindow, FullScreen
            size: 24 / Screen.devicePixelRatio // fixed 24 device pixels
            color: expandArea.containsMouse ? Theme.text : Theme.subtext
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
        anchors.right: pin.left
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
