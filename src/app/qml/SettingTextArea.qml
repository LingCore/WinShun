pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A setting of several lines (a plugin's "multiline" text): the style's
// field, as tall as its lines between `minLines` and `maxLines` and
// scrolled past them. What is typed is kept a moment after the typing
// stops and on leaving the box; Tab goes on to the next control, Esc
// puts back what was kept (and, with nothing to put back, is the window's).
Item {
    id: area

    property string value // what is kept; shown again while the box is not being typed in
    property string placeholder
    property string label // for screen readers: the setting's title
    property real dpr: 0
    property int minLines: 4
    property int maxLines: 12
    readonly property bool typing: field.activeFocus
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio

    signal committed(string text)

    function commit() {
        quiet.stop()
        if (field.text !== value)
            committed(field.text)
    }

    readonly property real padding: 8
    implicitWidth: 320
    implicitHeight: Math.round(Math.min(Math.max(field.implicitHeight, lineMetrics.height * minLines),
                                        lineMetrics.height * maxLines)) + 2 * padding

    onValueChanged: if (!field.activeFocus) field.text = value
    Component.onCompleted: field.text = value

    FontMetrics { id: lineMetrics; font: field.font }
    Timer { id: quiet; interval: 1000; onTriggered: area.commit() }

    Hook { // the style's field, with its focus mark
        id: part
        anchors.fill: parent
        part: Theme.inputField
        focused: field.activeFocus
        hovered: mouse.containsMouse
        dpr: area.pixelRatio
    }

    MouseArea { // beside and below the lines: the caret to the end
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.IBeamCursor
        onPressed: {
            field.forceActiveFocus(Qt.MouseFocusReason)
            field.cursorPosition = field.length
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: area.padding
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        clip: true
        contentWidth: width
        contentHeight: field.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        ListWheelScroll {
            view: flick
            animate: SystemTheme.animations
        }

        TextEdit {
            id: field
            objectName: "settingTextArea"
            // As tall as its lines: below them a press reaches the box's
            // MouseArea (the caret to the end), and the box can follow them.
            width: flick.width
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            activeFocusOnTab: true
            color: part.out("textColor", Theme.text)
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            font.pixelSize: Theme.fontBody
            Accessible.name: area.label
            // The caret in sight as it moves.
            onCursorRectangleChanged: {
                if (cursorRectangle.y < flick.contentY)
                    flick.contentY = cursorRectangle.y
                else if (cursorRectangle.y + cursorRectangle.height > flick.contentY + flick.height)
                    flick.contentY = cursorRectangle.y + cursorRectangle.height - flick.height
            }
            onTextChanged: if (activeFocus) quiet.restart()
            onActiveFocusChanged: if (!activeFocus) area.commit()
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                    const next = field.nextItemInFocusChain(event.key === Qt.Key_Tab)
                    if (next && next !== field)
                        next.forceActiveFocus(Qt.TabFocusReason)
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape && field.preeditText.length === 0 // composing: the input method's
                           && field.text !== area.value) { // nothing to put back: the window's Esc
                    quiet.stop()
                    const at = field.cursorPosition // set, the text puts the caret at its start
                    field.text = area.value
                    field.cursorPosition = Math.min(at, field.length)
                    event.accepted = true
                }
            }

            Text {
                anchors.fill: parent
                visible: field.length === 0 && field.preeditText.length === 0
                text: area.placeholder
                color: part.out("placeholderColor", Theme.faint)
                font: field.font
                wrapMode: Text.Wrap
            }
        }
    }

    ListScrollBar {
        dpr: area.pixelRatio
        flickable: flick
        anchors.right: parent.right
        anchors.rightMargin: 2
        anchors.top: flick.top
        anchors.bottom: flick.bottom
    }
}
