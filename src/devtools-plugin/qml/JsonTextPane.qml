pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

// The JSON as text, to paste into and change: read again a moment after
// typing stops, the line with an error marked. A text too long to lay out
// quickly is not shown here (the tree still has it).
Item {
    id: pane

    required property JsonTool tool
    property real dpr: 1
    readonly property int codeSize: Theme.uiBody
    readonly property bool marked: tool.error.length > 0 && tool.errorOffset >= 0 && tool.editable
    // Where the error is, as laid out (the dependencies make it follow the layout).
    readonly property rect errorRect: marked && field.width > 0 && field.contentHeight > 0
                                      ? field.positionToRectangle(Math.min(tool.errorOffset, field.length))
                                      : Qt.rect(0, 0, 0, 0)

    function take() { field.text = pane.tool.editable ? pane.tool.input : "" }
    function showError() {
        if (!marked)
            return
        field.cursorPosition = Math.min(tool.errorOffset, field.length)
        field.forceActiveFocus()
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, errorRect.y - flick.height / 3))
    }

    Connections {
        target: pane.tool
        function onInputChanged() { pane.take() }
    }
    Component.onCompleted: take()

    FontMetrics { id: code; font: field.font }

    Flickable {
        id: flick
        anchors.fill: parent
        visible: pane.tool.editable
        clip: true
        contentWidth: width
        contentHeight: field.height + 20
        boundsBehavior: Flickable.StopAtBounds
        ListWheelScroll { view: flick; animate: SystemTheme.animations }

        Rectangle { // the line with the error
            visible: pane.marked
            x: 0
            y: field.y + pane.errorRect.y
            width: flick.width
            height: Math.max(pane.errorRect.height, code.height)
            color: Qt.alpha(Theme.danger, Theme.dark ? 0.22 : 0.12)
            Rectangle { width: 3; height: parent.height; color: Theme.danger }
        }
        TextEdit {
            id: field
            objectName: "jsonTextEdit"
            x: 14
            y: 10
            width: flick.width - 28
            height: Math.max(implicitHeight, flick.height - 20)
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            tabStopDistance: code.advanceWidth("    ")
            color: Theme.text
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            font.family: "Consolas"
            font.pixelSize: pane.codeSize
            Accessible.name: qsTr("JSON 原文")
            onTextChanged: pane.tool.edit(text)
            onCursorRectangleChanged: { // the caret in sight as it moves
                if (!activeFocus)
                    return
                if (cursorRectangle.y + y < flick.contentY)
                    flick.contentY = cursorRectangle.y + y
                else if (cursorRectangle.y + y + cursorRectangle.height > flick.contentY + flick.height)
                    flick.contentY = cursorRectangle.y + y + cursorRectangle.height - flick.height
            }

            Text {
                width: parent.width
                visible: field.length === 0 && field.preeditText.length === 0
                text: qsTr("在这里粘贴或输入 JSON")
                color: Theme.faint
                font: field.font
                elide: Text.ElideRight
            }
        }
    }
    ListScrollBar {
        dpr: pane.dpr
        flickable: flick
        anchors.top: flick.top
        anchors.bottom: flick.bottom
        anchors.right: flick.right
        visible: flick.visible
        z: 2
    }

    // Too long for the box.
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 320)
        visible: !pane.tool.editable
        spacing: 8
        Glyph {
            Layout.alignment: Qt.AlignHCenter
            glyph: "ph:file-text"
            size: 32
            color: Theme.faint
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("原文有 %1，太长了，这里不显示").arg(pane.tool.inputSize)
            color: Theme.text
            font.pixelSize: Theme.uiBody
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            lineHeight: 1.4
            text: qsTr("右边照样能查看、搜索；“复制”菜单能复制格式化或压缩后的全部内容")
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
    }
}
