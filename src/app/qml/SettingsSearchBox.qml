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
    function setText(text) { input.text = text } // App::openAiSettings: looks for a row by its title
    function focusAll() {
        input.forceActiveFocus(Qt.ShortcutFocusReason)
        input.selectAll()
    }

    // The window's, for the style's field and the key hint on whole device pixels.
    readonly property real dpr: Screen.devicePixelRatio

    implicitHeight: 36
    radius: Theme.controlRadius
    color: warm ? WarmPalette.chip : "transparent" // else the style's field draws it (below)

    Hook { // the style's search field (Theme.searchField, boxed): fill, edge, the focus mark
        id: fieldPart
        anchors.fill: parent
        visible: !box.warm
        part: box.warm ? null : Theme.searchField
        flat: false
        focused: input.activeFocus
        hovered: area.containsMouse
        dpr: box.dpr
    }

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
        color: box.warm ? WarmPalette.inkSoft : fieldPart.out("iconColor", Theme.subtext)
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
        color: box.warm ? WarmPalette.ink : fieldPart.out("textColor", Theme.text)
        font.pixelSize: Theme.fontBody
        selectByMouse: true
        selectionColor: Theme.textSelection
        selectedTextColor: box.warm ? WarmPalette.ink : Theme.text
        // The style's caret colour, where it has one (Qt's own caret otherwise).
        cursorDelegate: !box.warm && fieldPart.out("caretColor", "transparent").a > 0 ? caretPart : null
        Component {
            id: caretPart
            Caret { color: fieldPart.out("caretColor", Theme.text); dpr: box.dpr }
        }
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
            color: box.warm ? WarmPalette.inkSoft : fieldPart.out("placeholderColor", Theme.faint)
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
        radius: Theme.controlRadius
        color: box.warm && (clearArea.pressed || clearArea.containsMouse) ? WarmPalette.chip : "transparent"

        Hook { // the style's key, flat until hovered (Theme.keySurface)
            id: clearPart
            anchors.fill: parent
            visible: !box.warm
            part: box.warm ? null : Theme.keySurface
            hovered: clearArea.containsMouse
            pressed: clearArea.pressed
            flat: true
            role: "toolbar"
            dpr: box.dpr
        }

        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: clearPart.out("contentOffsetY", 0)
            glyph: "" // Cancel
            size: 12
            color: box.warm ? WarmPalette.ink : clearPart.out("foreground", clearArea.containsMouse ? Theme.text : Theme.subtext)
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

    Item { // the key that gets here, while nothing is typed
        id: keyHint
        visible: box.empty && !input.activeFocus && !box.warm
        // On whole device pixels: a keycap on a half one is soft on its sides.
        x: Theme.snap(parent.width - 7 - width, box.dpr)
        y: Theme.snap((parent.height - height) / 2, box.dpr)
        width: Theme.snapUp(keyText.implicitWidth + 10, box.dpr)
        height: 22

        Hook { // the style's key cap (Theme.keyHint): it only tells, never pressed
            id: keyCap
            anchors.fill: parent
            part: keyHint.visible ? Theme.keyHint : null
            dpr: box.dpr
        }

        Text {
            id: keyText
            x: Theme.snap((keyHint.width - width) / 2, box.dpr)
            y: Theme.snap((keyHint.height - height) / 2 + keyCap.out("contentOffsetY", 0), box.dpr)
            text: "Ctrl+F"
            color: keyCap.out("foreground", Theme.faint)
            font.pixelSize: Theme.fontCaption
        }
    }

    Rectangle { // the accent line of the field being typed into, on the warm scene (else the field's own mark)
        visible: input.activeFocus && box.warm
        anchors.bottom: parent.bottom
        x: Theme.edge(box.dpr)
        width: parent.width - 2 * x
        height: Theme.snap(2, box.dpr) // 2.5 device pixels at 125 %
        radius: Theme.lineRadius
        color: WarmPalette.accent
    }
}
