import QtQuick
import WinShun

// Single-line text field with a placeholder and the accent underline when focused.
Rectangle {
    id: box

    property alias text: input.text
    property alias validator: input.validator
    property alias horizontalAlignment: input.horizontalAlignment
    property string placeholder

    signal accepted()
    signal editingFinished()

    function clear() { input.text = "" }
    function takeFocus() { input.forceActiveFocus() }

    implicitWidth: 220
    implicitHeight: 36
    radius: 4
    color: input.activeFocus ? Theme.inputFocused : area.containsMouse ? Theme.controlHover : Theme.control
    border.width: 1
    border.color: Theme.controlBorder

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.IBeamCursor
        acceptedButtons: Qt.NoButton
    }

    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        color: Theme.text
        font.pixelSize: Theme.fontBody
        selectByMouse: true
        selectionColor: Theme.textSelection
        selectedTextColor: Theme.text
        onAccepted: box.accepted()
        onEditingFinished: box.editingFinished()

        Text {
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: input.horizontalAlignment
            text: box.placeholder
            color: Theme.faint
            font: input.font
            elide: Text.ElideRight
            visible: input.text.length === 0 && input.preeditText.length === 0
        }
    }

    Rectangle {
        visible: input.activeFocus
        anchors.bottom: parent.bottom
        x: 1
        width: parent.width - 2
        height: 2
        radius: 1
        color: Theme.accent
    }
}
