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

    readonly property real radius: look ? look.controlRadius : 0
    readonly property color caretColor: look ? look.caret : "transparent"
    readonly property color ink: look ? (enabled ? look.ink : look.ink3) : "transparent"
    readonly property color placeholderInk: look ? look.ink3 : "transparent"
    // Hook.out names.
    readonly property color textColor: ink
    readonly property color placeholderColor: placeholderInk
    readonly property color iconColor: look ? look.ink2 : "transparent"
    readonly property real disabledOpacity: 1 // the box dims itself (below)

    opacity: enabled ? 1 : 0.55

    Rectangle {
        anchors.fill: parent
        radius: box.radius
        color: box.look ? (box.focused ? box.look.inputFocused : box.hovered && box.enabled ? box.look.controlHover : box.look.control) : "transparent"
        border.width: box.look ? box.look.edge(box.dpr) : 0
        border.color: !box.look ? "transparent"
                    : box.error ? box.look.danger
                    : box.focused ? (box.look.dark ? Qt.rgba(1, 1, 1, 0.28) : Qt.rgba(0, 0, 0, 0.32))
                    : box.hovered && box.enabled ? (box.look.dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.20))
                    : box.look.controlBorder
        Behavior on color { ColorAnimation { duration: box.look ? box.look.animFast : 0 } }
        Behavior on border.color { ColorAnimation { duration: box.look ? box.look.animFast : 0 } }
    }
}
