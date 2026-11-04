import QtQuick

// inputField: an ordinary text box (InputBox, forms). Neutral throughout: the
// focus shows in the fill (it turns plain) and a firmer 1 px edge; the brand
// colour is only the caret. Error: the edge in the danger colour.
Item {
    id: box

    property QtObject look
    property bool focused: false
    property bool hovered: false
    // enabled: Item's own (the host binds it; a part takes no input)
    property bool error: false
    property real dpr: 1

    readonly property real radius: look.controlRadius
    readonly property color caretColor: look.caret
    readonly property color ink: enabled ? look.ink : look.ink3
    readonly property color placeholderInk: look.ink3
    // Hook.out names.
    readonly property color textColor: ink
    readonly property color placeholderColor: placeholderInk
    readonly property color iconColor: look.ink2
    readonly property real disabledOpacity: 1 // the box dims itself (below)

    opacity: enabled ? 1 : 0.55

    Rectangle {
        anchors.fill: parent
        radius: box.radius
        color: box.focused ? box.look.inputFocused : box.hovered && box.enabled ? box.look.controlHover : box.look.control
        border.width: box.look.edge(box.dpr)
        border.color: box.error ? box.look.danger
                    : box.focused ? (box.look.dark ? Qt.rgba(1, 1, 1, 0.28) : Qt.rgba(0, 0, 0, 0.32))
                    : box.hovered && box.enabled ? (box.look.dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.20))
                    : box.look.controlBorder
        Behavior on color { ColorAnimation { duration: box.look.animFast } }
        Behavior on border.color { ColorAnimation { duration: box.look.animFast } }
    }
}
