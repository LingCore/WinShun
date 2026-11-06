import QtQuick
import QtQuick.Effects

// menuFrame: ContextMenu, DropMenu, HoverTip. A plain white (dark: #2F2F35)
// card, radius 8 (a menu window's, DWM's), a faint 1 px neutral edge and two
// thin, close shadows (1 px + 4 px). Items sit `padding` (4) inside it with
// `itemRadius` 4: concentric (8 − 4).
Item {
    id: menu

    property QtObject look
    property real dpr: 1
    // extra from a window's own floating bar (the capture toolbar, the presenter's bar): edged as the style's windows are, not as a menu.
    property bool floating: false

    readonly property real radius: look ? look.menuRadius : 0
    readonly property real padding: look ? look.menuPadding : 0
    readonly property real itemRadius: look ? look.menuItemRadius : 0
    readonly property real itemHeight: look ? look.menuItemHeight : 0
    readonly property color ink: look ? look.ink : "transparent"
    readonly property color iconInk: look ? look.ink2 : "transparent"
    readonly property color dangerInk: look ? look.danger : "transparent"
    // Hook.out names.
    readonly property color foreground: ink
    readonly property color iconColor: iconInk

    RectangularShadow {
        anchors.fill: parent
        radius: menu.radius
        offset.y: 4
        blur: 14
        spread: -1
        color: menu.look ? menu.look.menuShadowFar : "transparent"
    }
    RectangularShadow {
        anchors.fill: parent
        radius: menu.radius
        offset.y: 1
        blur: 2.4
        color: menu.look ? menu.look.menuShadowNear : "transparent"
    }
    Rectangle {
        anchors.fill: parent
        radius: menu.radius
        color: menu.look ? menu.look.menuFill : "transparent"
        border.width: menu.look ? menu.look.edge(menu.dpr) : 0
        border.color: menu.look ? (menu.floating ? menu.look.windowEdge : menu.look.menuEdge) : "transparent"
    }
}
