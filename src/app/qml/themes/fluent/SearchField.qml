import QtQuick

// searchField: under a search box's text. `flat`: the launcher's search
// row, which in fluent is no box at all (it sits on the chrome layer, the
// caret and the scope's pill say where you are); otherwise a boxed field
// (the settings', the clipboard's, a plugin's) as an InputBox.
Item {
    id: part

    property QtObject look: null
    property bool flat: false
    property bool focused: false
    property bool hovered: false
    property real dpr: 1

    readonly property real edge: Math.max(1, Math.round(dpr)) / dpr

    Rectangle {
        anchors.fill: parent
        visible: !part.flat && part.look !== null
        radius: part.look ? part.look.controlRadius : 0
        color: !part.look ? "transparent" : part.focused ? part.look.inputFocused : part.hovered ? part.look.controlHover : part.look.control
        border.width: part.edge
        border.color: part.look ? part.look.controlBorder : "transparent"

        Rectangle { // focused: the accent underline, inside the border
            visible: part.focused
            anchors.bottom: parent.bottom
            x: part.edge
            width: parent.width - 2 * x
            height: Math.round(2 * part.dpr) / part.dpr
            radius: part.look ? part.look.lineRadius : 0
            color: part.look ? part.look.accent : "transparent"
        }
    }
}
