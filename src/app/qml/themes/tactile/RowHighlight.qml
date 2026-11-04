pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects

// Under a row. Selected (or current): a card lifted out of the tray: a
// brighter face, a 1 px line, a 1 px top light and one thin close shadow;
// pressed (Enter) it settles, the shadow goes. Hovered: a faint warm tint.
// In a menu (role "menu") the hovered/current item rises as a key face instead.
// The host keeps a single highlight and slides it between rows (animSlide).
// A plain row builds only the tint; the card and the key face are loaded
// when shown (lists have hundreds of rows).
Item {
    id: row

    property bool selected: false
    property bool hovered: false
    property bool current: false
    property bool pressed: false
    property string role           // "" | list | menu | panel
    property real corner: -1
    property real dpr: 1
    readonly property bool menuMode: role === "menu"
    readonly property real radius: corner >= 0 ? corner : !look ? 0 : menuMode ? look.menuItemRadius : look.liftRadius

    property QtObject look // injected by Look.qml
    readonly property real hair: look ? look.hair(dpr) : 1
    readonly property bool lifted: !menuMode && (selected || current)
    readonly property bool risen: menuMode && (selected || current || hovered)

    // hover tint
    Rectangle {
        visible: !row.lifted && !row.menuMode && row.hovered && row.look !== null
        anchors.fill: parent
        radius: row.radius
        color: row.look ? (row.look?.rowHover ?? Qt.rgba(0, 0, 0, 0)) : "transparent"
    }

    // ---- the lifted card ----
    Loader {
        anchors.fill: parent
        active: row.lifted && row.look !== null
        sourceComponent: Item {
            RectangularShadow {
                visible: !row.pressed
                anchors.fill: parent
                radius: row.radius
                offset.y: 3
                spread: -2
                blur: 9.6 // CSS 8 x 1.2
                color: (row.look?.cardShadow ?? Qt.rgba(0, 0, 0, 0))
            }
            Rectangle { // the close one: a device pixel under the edge
                visible: !row.pressed
                y: row.hair
                width: parent.width
                height: parent.height
                radius: row.radius
                color: (row.look?.cardShadowNear ?? Qt.rgba(0, 0, 0, 0))
            }
            Rectangle {
                anchors.fill: parent
                radius: row.radius
                color: Qt.tint((row.look?.cardColor ?? Qt.rgba(0, 0, 0, 0)), (row.look?.cardLine ?? Qt.rgba(0, 0, 0, 0))) // CSS borders lie over the element's own face
            }
            Rectangle { // top light: the face below leaves a one-pixel crescent
                x: row.hair
                y: row.hair
                width: parent.width - 2 * row.hair
                height: parent.height - 2 * row.hair
                radius: Math.max(0, row.radius - row.hair)
                color: Qt.tint((row.look?.cardColor ?? Qt.rgba(0, 0, 0, 0)), (row.look?.cardHighlight ?? Qt.rgba(0, 0, 0, 0)))
            }
            Rectangle {
                x: row.hair
                y: 2 * row.hair
                width: parent.width - 2 * row.hair
                height: parent.height - 3 * row.hair
                radius: Math.max(0, row.radius - row.hair)
                color: (row.look?.cardColor ?? Qt.rgba(0, 0, 0, 0))
            }
        }
    }

    // ---- a menu item under the pointer: a key face, no contact shadow ----
    Loader {
        anchors.fill: parent
        active: row.risen && row.look !== null
        sourceComponent: KeySurface {
            corner: row.radius
            drop: false
            dpr: row.dpr
            look: row.look
        }
    }
}
