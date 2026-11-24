pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A clipboard entry's own words, changed in the preview's place (GitHub
// #267): Ctrl+Enter saves, Esc leaves them as they were.
Item {
    id: editor

    property UiScale metrics: Theme.metrics
    property real dpr: Screen.devicePixelRatio
    property real resizeMargin: 0
    readonly property alias text: field.text
    readonly property bool typing: field.activeFocus

    signal saveRequested(string text)
    signal cancelled()
    signal focusLost()
    signal uiSizeRequested(int delta)

    function begin(text) {
        field.text = text
        field.cursorPosition = field.length
        field.forceActiveFocus()
    }

    Hook { // the style's field
        id: part
        anchors.fill: flick
        anchors.margins: -8
        part: Theme.inputField
        focused: field.activeFocus
        dpr: editor.dpr
    }

    Flickable {
        id: flick
        objectName: "clipboardTextEditor"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: keys.top
        anchors.leftMargin: 20
        anchors.rightMargin: 20 + editor.resizeMargin
        anchors.topMargin: 20
        anchors.bottomMargin: 18
        clip: true
        contentWidth: width
        contentHeight: field.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ListWheelScroll {
            view: flick
            animate: SystemTheme.animations
        }

        TextEdit {
            id: field
            width: flick.width - 8
            height: Math.max(implicitHeight, flick.height)
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            color: part.out("textColor", Theme.text)
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            font.pixelSize: editor.metrics.uiBody
            Accessible.name: qsTr("修改文字")
            // The caret in sight as it moves.
            onCursorRectangleChanged: {
                if (cursorRectangle.y < flick.contentY)
                    flick.contentY = cursorRectangle.y
                else if (cursorRectangle.y + cursorRectangle.height > flick.contentY + flick.height)
                    flick.contentY = cursorRectangle.y + cursorRectangle.height - flick.height
            }
            onActiveFocusChanged: if (!activeFocus) editor.focusLost()
            Keys.onPressed: (event) => {
                const zoom = field.preeditText.length === 0 ? SearchKeys.uiZoomAction(event.key, event.modifiers) : SearchKeys.zoomNone
                if (zoom !== SearchKeys.zoomNone) {
                    if (!event.isAutoRepeat)
                        editor.uiSizeRequested(zoom === SearchKeys.zoomReset ? 0 : zoom)
                    event.accepted = true
                    return
                }
                const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
                if (ctrl && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_S)) {
                    editor.saveRequested(field.text)
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape && field.preeditText.length === 0) { // composing: the input method's
                    editor.cancelled()
                    event.accepted = true
                }
            }
        }
    }

    ListScrollBar {
        dpr: editor.dpr
        flickable: flick
        anchors.right: flick.right
        anchors.rightMargin: -8
        anchors.top: flick.top
        anchors.bottom: flick.bottom
    }

    Item {
        id: keys
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 12
        anchors.rightMargin: 12 + editor.resizeMargin
        anchors.bottomMargin: 10
        height: save.height

        Text {
            anchors.left: parent.left
            anchors.right: buttons.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Ctrl+Enter 保存")
            elide: Text.ElideRight
            color: Theme.faint
            font.pixelSize: editor.metrics.uiCaption
        }
        Row {
            id: buttons
            anchors.right: parent.right
            spacing: 8

            FlatButton {
                dpr: editor.dpr
                implicitHeight: 30
                keyboardActivation: false
                text: qsTr("取消")
                onClicked: editor.cancelled()
            }
            FlatButton {
                id: save
                dpr: editor.dpr
                implicitHeight: 30
                keyboardActivation: false
                highlighted: true
                enabled: field.text.trim().length > 0
                text: qsTr("保存")
                onClicked: editor.saveRequested(field.text)
            }
        }
    }
}
