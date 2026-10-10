import QtQuick
import WinShun

// The box above the settings' categories: the options on the right narrow
// down to those that fit as the words are typed (see SettingsSearch). ↑ and
// ↓ pick one of them, Enter goes to it, Esc clears the box.
Rectangle {
    id: box

    property bool warm: false // on the 拾穗计划 scene
    // What to look for: the text, and the letters an input method is still
    // composing ("jian'tie"), so that pinyin finds options before a
    // character is chosen.
    readonly property string query: input.text + composing
    readonly property string composing: input.preeditText.replace(/['\s]/g, "")
    readonly property bool empty: input.text.length === 0 && input.preeditText.length === 0
    readonly property bool editing: input.activeFocus

    signal moved(int delta) // ↑: -1, ↓: 1
    signal activated() // Enter
    signal escaped() // Esc with nothing typed

    function clear() { input.text = "" }
    function focusAll() {
        input.forceActiveFocus(Qt.ShortcutFocusReason)
        input.selectAll()
    }

    implicitHeight: 36
    radius: 4
    color: warm ? WarmPalette.chip
         : input.activeFocus ? Theme.inputFocused : area.containsMouse ? Theme.controlHover : Theme.control
    border.width: warm ? 0 : 1
    border.color: Theme.controlBorder

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.IBeamCursor
        onPressed: (mouse) => {
            input.forceActiveFocus(Qt.MouseFocusReason)
            mouse.accepted = false // the field places the caret
        }
    }

    Glyph {
        x: 11
        anchors.verticalCenter: parent.verticalCenter
        glyph: "" // Search
        size: 16
        color: box.warm ? WarmPalette.inkSoft : Theme.subtext
    }

    TextInput {
        id: input
        anchors.left: parent.left
        anchors.leftMargin: 36
        anchors.right: clearButton.visible ? clearButton.left : keyHint.visible ? keyHint.left : parent.right
        anchors.rightMargin: 6
        height: parent.height
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        activeFocusOnTab: true
        color: box.warm ? WarmPalette.ink : Theme.text
        font.pixelSize: Theme.fontBody
        selectByMouse: true
        selectionColor: Theme.textSelection
        selectedTextColor: box.warm ? WarmPalette.ink : Theme.text
        maximumLength: 100

        Accessible.role: Accessible.EditableText
        Accessible.name: placeholder.text

        Keys.onUpPressed: box.moved(-1)
        Keys.onDownPressed: box.moved(1)
        Keys.onReturnPressed: box.activated()
        Keys.onEnterPressed: box.activated()
        Keys.onEscapePressed: {
            if (input.text.length > 0)
                box.clear()
            else
                box.escaped()
        }

        Text {
            id: placeholder
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            text: qsTr("查找设置")
            color: box.warm ? WarmPalette.inkSoft : Theme.faint
            font: input.font
            elide: Text.ElideRight
            visible: box.empty
        }
    }

    Rectangle { // clears the box
        id: clearButton
        visible: !box.empty
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 28
        height: 28
        radius: 4
        color: clearArea.pressed ? (box.warm ? WarmPalette.chip : Theme.controlPressed)
             : clearArea.containsMouse ? (box.warm ? WarmPalette.chip : Theme.chipHover) : "transparent"

        Glyph {
            anchors.centerIn: parent
            glyph: "" // Cancel
            size: 12
            color: box.warm ? WarmPalette.ink : clearArea.containsMouse ? Theme.text : Theme.subtext
        }

        MouseArea {
            id: clearArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                box.clear()
                input.forceActiveFocus(Qt.MouseFocusReason)
            }
        }
    }

    Rectangle { // the key that gets here, while nothing is typed
        id: keyHint
        visible: box.empty && !input.activeFocus && !box.warm
        anchors.right: parent.right
        anchors.rightMargin: 7
        anchors.verticalCenter: parent.verticalCenter
        width: keyText.implicitWidth + 10
        height: 22
        radius: 4
        color: Theme.keycap
        border.width: 1
        border.color: Theme.keycapBorder

        Text {
            id: keyText
            anchors.centerIn: parent
            text: "Ctrl+F"
            color: Theme.faint
            font.pixelSize: Theme.fontCaption
        }
    }

    Rectangle { // the accent line of the field being typed into
        visible: input.activeFocus
        anchors.bottom: parent.bottom
        x: 1
        width: parent.width - 2
        height: 2
        radius: 1
        color: box.warm ? WarmPalette.accent : Theme.accent
    }
}
