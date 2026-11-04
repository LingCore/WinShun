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

    property bool error: false // the value is not taken: the style's error mark
    property real dpr: 0 // the window's, passed by a host that has it (GitHub #111)
    // Screen.* only when no host passed it: a default binding to it would make a
    // Screen object even where the host's value replaces it.
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio

    implicitWidth: 220
    implicitHeight: 36
    radius: Theme.controlRadius
    color: "transparent"

    Hook { // the style's field (Theme.inputField), with its focus mark
        id: field
        anchors.fill: parent
        part: Theme.inputField
        focused: input.activeFocus
        hovered: area.containsMouse
        error: box.error
        dpr: box.pixelRatio
    }

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
        color: field.out("textColor", Theme.text)
        font.pixelSize: Theme.fontBody
        // A value wider than the box shows its start while nobody is
        // typing in it: set, the text leaves the cursor at the end, and
        // scrolling to the cursor would show the end ("d5 sha1 sha256").
        autoScroll: activeFocus
        selectByMouse: true
        selectionColor: Theme.textSelection
        selectedTextColor: Theme.text
        // The style's caret colour, where it has one (Qt's own caret otherwise).
        cursorDelegate: field.out("caretColor", "transparent").a > 0 ? caretPart : null
        Component {
            id: caretPart
            Caret { color: field.out("caretColor", Theme.text); dpr: box.pixelRatio }
        }
        onAccepted: box.accepted()
        onEditingFinished: box.editingFinished()

        Text {
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: input.horizontalAlignment
            text: box.placeholder
            color: field.out("placeholderColor", Theme.faint)
            font: input.font
            elide: Text.ElideRight
            // Centred, the empty field's cursor stands in the middle of the
            // placeholder: it goes once the field is being typed in.
            visible: input.text.length === 0 && input.preeditText.length === 0
                     && !(input.activeFocus && input.horizontalAlignment !== TextInput.AlignLeft)
        }
    }

}
