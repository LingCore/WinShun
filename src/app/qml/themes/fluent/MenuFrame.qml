import QtQuick
import QtQuick.Effects

// menuFrame: a menu's or a tip's box (ContextMenu, DropMenu, HoverTip, a
// tray menu drawn in QML): solid like Acrylic's tint (nothing behind it is
// blurred), a 1 px edge and a light shadow close under it.
Item {
    id: part

    property QtObject look: null
    property real dpr: 1

    readonly property real radius: look ? look.menuRadius : 0

    RectangularShadow {
        anchors.fill: parent
        radius: part.radius
        offset.y: 2
        blur: part.look ? part.look.menuShadowSpread * 2 : 0
        spread: -2
        color: part.look ? part.look.menuShadow : "transparent"
    }

    Rectangle {
        anchors.fill: parent
        radius: part.radius
        color: part.look ? part.look.menuBackground : "transparent"
        border.width: Math.max(1, Math.round(part.dpr)) / part.dpr
        border.color: part.look ? part.look.menuEdge : "transparent"
    }
}
