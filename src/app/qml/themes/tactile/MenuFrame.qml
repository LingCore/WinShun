pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects

// A menu or a tip: the card material (brighter face, 1 px line, 1 px top
// light) and one thin, close shadow. The host keeps menuPadding (6) inside;
// items are radius 4 (concentric with the 8 px frame, a menu window's).
Item {
    id: menu

    property real dpr: 1
    property real radius: look ? look.menuRadius : 0

    property QtObject look // injected by Look.qml
    readonly property real hair: look ? look.hair(dpr) : 1
    readonly property real padding: look ? look.menuPadding : 0
    readonly property color textColor: look ? look.ink : "transparent"
    readonly property color iconColor: look ? look.ink2 : "transparent"

    // Built once `look` is set: a part's own bindings can run before Look's
    // `look: root` (Hook makes it with createObject).
    Loader {
        anchors.fill: parent
        active: menu.look !== null
        sourceComponent: Item {
            RectangularShadow {
                anchors.fill: parent
                radius: menu.radius
                offset.y: 3
                spread: -2
                blur: 12
                color: (menu.look?.menuShadow ?? Qt.rgba(0, 0, 0, 0))
            }
            Rectangle {
                y: menu.hair
                width: menu.width
                height: menu.height
                radius: menu.radius
                color: (menu.look?.menuShadowNear ?? Qt.rgba(0, 0, 0, 0))
            }
            Rectangle {
                anchors.fill: parent
                radius: menu.radius
                color: Qt.tint((menu.look?.cardColor ?? Qt.rgba(0, 0, 0, 0)), (menu.look?.cardLine ?? Qt.rgba(0, 0, 0, 0)))
            }
            Rectangle {
                x: menu.hair
                y: menu.hair
                width: menu.width - 2 * menu.hair
                height: menu.height - 2 * menu.hair
                radius: Math.max(0, menu.radius - menu.hair)
                color: Qt.tint((menu.look?.cardColor ?? Qt.rgba(0, 0, 0, 0)), (menu.look?.cardHighlight ?? Qt.rgba(0, 0, 0, 0)))
            }
            Rectangle {
                x: menu.hair
                y: 2 * menu.hair
                width: menu.width - 2 * menu.hair
                height: menu.height - 3 * menu.hair
                radius: Math.max(0, menu.radius - menu.hair)
                color: (menu.look?.cardColor ?? Qt.rgba(0, 0, 0, 0))
            }
        }
    }
}
