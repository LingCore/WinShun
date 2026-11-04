import QtQuick
import QtQuick.Effects

// menuFrame: ContextMenu, DropMenu, HoverTip. A plain white (dark: #2F2F35)
// card, radius 10, a faint 1 px neutral edge and two thin, close shadows
// (1 px + 4 px). Items sit `padding` (4) inside it with `itemRadius` 6:
// concentric (the prototype's 7 corrected to 10 − 4).
Item {
    id: menu

    property QtObject look
    property real dpr: 1

    readonly property real radius: look.menuRadius
    readonly property real padding: look.menuPadding
    readonly property real itemRadius: look.menuItemRadius
    readonly property real itemHeight: look.menuItemHeight
    readonly property color ink: look.ink
    readonly property color iconInk: look.ink2
    readonly property color dangerInk: look.danger
    // Hook.out names.
    readonly property color foreground: ink
    readonly property color iconColor: iconInk

    RectangularShadow {
        anchors.fill: parent
        radius: menu.radius
        offset.y: 4
        blur: 14
        spread: -1
        color: menu.look.menuShadowFar
    }
    RectangularShadow {
        anchors.fill: parent
        radius: menu.radius
        offset.y: 1
        blur: 2.4
        color: menu.look.menuShadowNear
    }
    Rectangle {
        anchors.fill: parent
        radius: menu.radius
        color: menu.look.menuFill
        border.width: menu.look.edge(menu.dpr)
        border.color: menu.look.menuEdge
    }
}
