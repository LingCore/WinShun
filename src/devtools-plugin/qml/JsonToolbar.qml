pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

// 粘贴, 格式化, 压缩; the tree's 展开 and 折叠; the text beside it shown or
// not; a search through names and values (Enter / F3 the next match,
// Shift the one before); 复制 in three forms.
Item {
    id: bar

    required property JsonTool tool
    property real dpr: 1
    property bool textShown: true

    signal textToggled()
    signal found(int row) // a match stepped to: the tree shows it
    signal searchLeft() // Esc in an empty search box, or ↓

    implicitHeight: 52

    function focusSearch() {
        input.forceActiveFocus(Qt.ShortcutFocusReason)
        input.selectAll()
    }
    function find(words) {
        input.text = words
        searchNow()
    }
    function searchNow() {
        pause.stop()
        const row = bar.tool.tree.search(input.text)
        if (row >= 0)
            bar.found(row)
    }
    function step(direction) {
        if (pause.running)
            searchNow()
        else {
            const row = bar.tool.tree.step(direction)
            if (row >= 0)
                bar.found(row)
        }
    }

    // A new text is searched again by the tree itself: shown here too.
    Connections {
        target: bar.tool.tree
        function onDocumentChanged() {
            if (input.text.length > 0) {
                const row = bar.tool.tree.currentMatchRow()
                if (row >= 0)
                    bar.found(row)
            }
        }
    }
    Timer { id: pause; interval: 150; onTriggered: bar.searchNow() }

    component ToolButton: FlatButton {
        id: toolButton
        property string tip
        dpr: bar.dpr
        flat: true
        role: "toolbar"
        Layout.preferredHeight: 32
        HoverHandler { id: hover }
        HoverTip { target: hover.hovered && toolButton.tip.length > 0 ? toolButton : null; text: toolButton.tip; dpr: bar.dpr }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 2

        ToolButton {
            glyph: "ph:clipboard-text"
            text: qsTr("粘贴")
            tip: qsTr("换成剪贴板里的内容（Ctrl+V）")
            onClicked: bar.tool.paste()
        }
        ToolButton {
            glyph: "ph:text-indent"
            text: qsTr("格式化")
            enabled: bar.tool.valid
            tip: qsTr("原文换成缩进排好的 JSON")
            onClicked: bar.tool.reformat(true)
        }
        ToolButton {
            glyph: "ph:arrows-in-line-horizontal"
            text: qsTr("压缩")
            enabled: bar.tool.valid
            tip: qsTr("原文换成一行，去掉空白")
            onClicked: bar.tool.reformat(false)
        }
        Rectangle {
            Layout.preferredWidth: Theme.edge(bar.dpr)
            Layout.preferredHeight: 18
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            color: Theme.divider
        }
        ToolButton {
            glyph: "ph:arrows-out-line-vertical"
            enabled: bar.tool.valid
            tip: qsTr("全部展开")
            onClicked: bar.tool.tree.expandAll()
        }
        ToolButton {
            glyph: "ph:arrows-in-line-vertical"
            enabled: bar.tool.valid
            tip: qsTr("全部折叠")
            onClicked: bar.tool.tree.collapseAll()
        }
        ToolButton {
            glyph: "ph:sidebar-simple"
            highlighted: false
            tip: bar.textShown ? qsTr("隐藏原文") : qsTr("显示原文")
            onClicked: bar.textToggled()
        }
        Item { Layout.fillWidth: true }

        Item { // the search box: the style's field, a magnifier, the words, n/m, ↑ ↓
            id: box
            objectName: "jsonSearch"
            Layout.preferredWidth: Math.min(300, Math.max(180, bar.width * 0.3))
            Layout.preferredHeight: 32
            Hook {
                id: field
                anchors.fill: parent
                part: Theme.searchField
                flat: false
                focused: input.activeFocus
                hovered: boxHover.hovered
                corner: Theme.fieldRadius
                dpr: bar.dpr
            }
            HoverHandler { id: boxHover }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.IBeamCursor
                onPressed: input.forceActiveFocus(Qt.MouseFocusReason)
            }
            Glyph {
                id: magnifier
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                glyph: "ph:magnifying-glass"
                size: 14
                color: field.out("iconColor", Theme.subtext)
            }
            TextInput {
                id: input
                anchors.left: magnifier.right
                anchors.leftMargin: 8
                anchors.right: counter.left
                anchors.rightMargin: 6
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                color: field.out("textColor", Theme.text)
                font.pixelSize: Theme.uiBody
                selectByMouse: true
                selectionColor: Theme.textSelection
                selectedTextColor: Theme.text
                Accessible.name: qsTr("搜索键名和值")
                onTextEdited: pause.restart()
                Keys.onPressed: (event) => {
                    const shift = (event.modifiers & Qt.ShiftModifier) !== 0
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_F3) {
                        bar.step(shift ? -1 : 1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Escape && input.preeditText.length === 0) {
                        if (input.text.length > 0) {
                            input.text = ""
                            bar.searchNow()
                        } else {
                            bar.searchLeft()
                        }
                        event.accepted = true
                    } else if (event.key === Qt.Key_Down) {
                        bar.searchLeft()
                        event.accepted = true
                    }
                }
                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("搜索键名和值")
                    color: field.out("placeholderColor", Theme.faint)
                    font: input.font
                    elide: Text.ElideRight
                    visible: input.text.length === 0 && input.preeditText.length === 0
                }
            }
            Text {
                id: counter
                anchors.right: arrows.left
                anchors.rightMargin: 2
                anchors.verticalCenter: parent.verticalCenter
                visible: input.text.length > 0
                width: visible ? implicitWidth : 0
                text: bar.tool.tree.matchCount > 0 ? bar.tool.tree.matchNumber + "/" + bar.tool.tree.matchCount : qsTr("无结果")
                color: bar.tool.tree.matchCount > 0 ? Theme.subtext : Theme.danger
                font.pixelSize: Theme.uiCaption
            }
            Row {
                id: arrows
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                visible: input.text.length > 0
                width: visible ? implicitWidth : 0
                Repeater {
                    model: [{ glyph: "ph:caret-up", direction: -1, tip: qsTr("上一个（Shift+Enter）") },
                            { glyph: "ph:caret-down", direction: 1, tip: qsTr("下一个（Enter）") }]
                    delegate: FlatButton {
                        id: arrow
                        required property var modelData
                        dpr: bar.dpr
                        flat: true
                        keyboardActivation: false
                        implicitWidth: 24
                        implicitHeight: 24
                        glyph: modelData.glyph
                        enabled: bar.tool.tree.matchCount > 1
                        onClicked: bar.step(modelData.direction)
                        HoverHandler { id: arrowHover }
                        HoverTip { target: arrowHover.hovered ? arrow : null; text: arrow.modelData.tip; dpr: bar.dpr }
                    }
                }
            }
        }

        FlatButton {
            id: copyButton
            Layout.leftMargin: 8
            Layout.preferredHeight: 32
            dpr: bar.dpr
            glyph: "ph:copy"
            text: qsTr("复制")
            trailingGlyph: "ph:caret-down"
            enabled: bar.tool.valid
            onClicked: copyMenu.open = !copyMenu.open
        }
    }
    DropMenu {
        id: copyMenu
        anchor: copyButton
        menuWidth: 200
        entries: [{ text: qsTr("复制格式化的 JSON"), glyph: "ph:text-indent" },
                  { text: qsTr("复制压缩的 JSON"), glyph: "ph:arrows-in-line-horizontal" },
                  { text: qsTr("复制转义后的 JSON"), glyph: "ph:quotes" }]
        onActivated: (index) => bar.tool.copy(["pretty", "compact", "escaped"][index])
    }
}
