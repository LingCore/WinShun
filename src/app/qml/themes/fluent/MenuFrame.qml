import QtQuick

// menuFrame: a menu's or a tip's box (ContextMenu, DropMenu, HoverTip, a
// tray menu drawn in QML): solid like Acrylic's tint (nothing behind it is
// blurred), a 1 px edge and a light shadow close under it.
Item {
    id: part

    property QtObject look: null
    property real dpr: 1

    readonly property real radius: look ? look.menuRadius : 0

    Repeater { // the shadow: faint rings, each a pixel further out (no effects deployed)
        model: part.look ? part.look.menuShadowSpread : 0

        Rectangle {
            required property int index
            readonly property real grow: index + 1
            x: -grow
            y: -grow + 2
            width: part.width + 2 * grow
            height: part.height + 2 * grow
            radius: part.radius > 0 ? part.radius + grow : 0
            color: Qt.rgba(part.look.menuShadow.r, part.look.menuShadow.g, part.look.menuShadow.b,
                           part.look.menuShadow.a / part.look.menuShadowSpread)
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: part.radius
        color: part.look ? part.look.menuBackground : "transparent"
        border.width: Math.max(1, Math.round(part.dpr)) / part.dpr
        border.color: part.look ? part.look.menuEdge : "transparent"
    }
}
