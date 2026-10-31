import QtQuick
import WinShun

// Single-line text field with a placeholder and the accent underline when focused.
Rectangle {
    id: box

    property alias text: input.text
    property alias validator: input.validator
    property alias horizontalAlignment: input.horizontalAlignment
    property alias echoMode: input.echoMode // TextInput.Password for a plugin's password setting
    property alias readOnly: input.readOnly // shown to be copied (selectable still)
    property alias maximumLength: input.maximumLength // 32767 characters unless raised
    property alias tabFocus: input.activeFocusOnTab
    property string placeholder
    readonly property bool inputFocused: input.activeFocus

    signal accepted()
    signal editingFinished()

    function clear() { input.text = "" }
    function takeFocus() { input.forceActiveFocus() }

    implicitWidth: 220
    implicitHeight: 36
    radius: Theme.controlRadius
    color: input.activeFocus ? Theme.inputFocused : area.containsMouse ? Theme.controlHover : Theme.control
    border.width: 1
    border.color: Theme.controlBorder

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.IBeamCursor
        // Only the margins beside the text reach here: the press is kept
        // (let through, the page under it took the focus back), and the
        // caret goes to the nearer end.
        onPressed: (mouse) => {
            input.forceActiveFocus(Qt.MouseFocusReason)
            input.cursorPosition = mouse.x < width / 2 ? 0 : input.length
        }
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
        // A value wider than the box shows its start while nobody is
        // typing in it: set, the text leaves the cursor at the end, and
        // scrolling to the cursor would show the end ("d5 sha1 sha256").
        autoScroll: activeFocus
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
            // Centred, the empty field's cursor stands in the middle of the
            // placeholder: it goes once the field is being typed in.
            visible: input.text.length === 0 && input.preeditText.length === 0
                     && !(input.activeFocus && input.horizontalAlignment !== TextInput.AlignLeft)
        }
    }

    Rectangle {
        visible: input.activeFocus
        anchors.bottom: parent.bottom
        x: Theme.edge(Screen.devicePixelRatio) // inside the field's border
        width: parent.width - 2 * x
        height: Theme.snap(2, Screen.devicePixelRatio) // 2.5 device pixels at 125 %
        radius: Theme.lineRadius
        color: Theme.accent
    }
}
