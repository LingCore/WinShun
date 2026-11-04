import QtQuick
import QtQuick.Effects

// A menu or a tip: the card material (brighter face, 1 px line, 1 px top
// light) and one thin, close shadow. The host keeps menuPadding (6) inside;
// items are radius 4 (concentric with the 10 px frame).
Item {
    id: menu

    property real dpr: 1
    property real radius: look.menuRadius

    property QtObject look // injected by Look.qml
    readonly property real hair: look.hair(dpr)
    readonly property real padding: look.menuPadding
    readonly property color textColor: look.ink
    readonly property color iconColor: look.ink2

    RectangularShadow {
        anchors.fill: parent
        radius: menu.radius
        offset.y: 8
        spread: -6
        blur: 21.6 // CSS 18 x 1.2
        color: menu.look.menuShadow
    }
    Rectangle {
        y: menu.hair
        width: menu.width
        height: menu.height
        radius: menu.radius
        color: menu.look.menuShadowNear
    }
    Rectangle {
        anchors.fill: parent
        radius: menu.radius
        color: Qt.tint(menu.look.cardColor, menu.look.cardLine)
    }
    Rectangle {
        x: menu.hair
        y: menu.hair
        width: menu.width - 2 * menu.hair
        height: menu.height - 2 * menu.hair
        radius: Math.max(0, menu.radius - menu.hair)
        color: Qt.tint(menu.look.cardColor, menu.look.cardHighlight)
    }
    Rectangle {
        x: menu.hair
        y: 2 * menu.hair
        width: menu.width - 2 * menu.hair
        height: menu.height - 3 * menu.hair
        radius: Math.max(0, menu.radius - menu.hair)
        color: menu.look.cardColor
    }
}
