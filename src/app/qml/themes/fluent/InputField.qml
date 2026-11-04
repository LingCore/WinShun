import QtQuick

// inputField: a text field's box (InputBox, forms). Windows 11's: a control
// fill and a 1 px neutral edge; focused, the field turns its brightest and
// gets the accent underline (a marker, not an outline).
Rectangle {
    id: part

    property QtObject look: null
    property bool focused: false
    property bool hovered: false
    property bool error: false
    property real dpr: 1

    readonly property real edge: Math.max(1, Math.round(dpr)) / dpr

    radius: look ? look.controlRadius : 0
    color: !look ? "transparent" : focused ? look.inputFocused : hovered ? look.controlHover : look.control
    border.width: edge
    border.color: look ? look.controlBorder : "transparent"

    Rectangle {
        visible: part.focused || part.error
        anchors.bottom: parent.bottom
        x: part.edge
        width: parent.width - 2 * x
        height: Math.round(2 * part.dpr) / part.dpr // 2.5 device pixels at 125 %
        radius: part.look ? part.look.lineRadius : 0
        color: !part.look ? "transparent" : part.error ? part.look.danger : part.look.accent
    }
}
