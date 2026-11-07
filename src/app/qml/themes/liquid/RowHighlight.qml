import QtQuick

// Selection uses a neutral fill and one accent marker, including navigation.
Rectangle {
    id: part

    property QtObject look: null
    property bool selected: false
    property bool hovered: false
    property bool current: false
    property bool pressed: false
    property string role
    property real dpr: 1
    readonly property bool menu: role === "menu"

    radius: !look ? 0 : menu ? look.menuItemRadius : look.rowRadius
    color: !look ? "transparent" : pressed ? look.pressed : selected || current ? look.selection : hovered ? look.hover : "transparent"

    Rectangle {
        x: 0
        y: Math.round((part.height - height) * part.dpr / 2) / part.dpr
        width: Math.round(3 * part.dpr) / part.dpr
        height: Math.min(18, part.height - 8)
        radius: part.look ? part.look.pill(width) : 0
        visible: !part.menu && (part.selected || part.current)
        color: part.look ? part.look.accentLine : "transparent"
    }
}
