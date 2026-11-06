pragma ComponentBehavior: Bound
import QtQuick

// A settings plate (SettingRow / SettingsSection), plugin card, share card.
// It cannot be pressed, so it is flat: a step lighter than the panel, a
// 1 px line round it and a 1 px top light, no shadow. Hover (cards that
// open something) only warms the face a step.
Item {
    id: card

    property bool hovered: false
    property real dpr: 1
    property real corner: -1
    readonly property real radius: corner >= 0 ? corner : look ? look.cardRadius : 0

    property QtObject look // injected by Look.qml
    readonly property real hair: look ? look.hair(dpr) : 1
    readonly property color face: !look ? "transparent" : hovered ? look.plateHover : look.plate

    // Built once `look` is set: a part's own bindings can run before Look's
    // `look: root` (Hook makes it with createObject).
    Loader {
        anchors.fill: parent
        active: card.look !== null
        sourceComponent: Item {
            Rectangle {
                anchors.fill: parent
                radius: card.radius
                color: Qt.tint(card.face, (card.look?.cardLine ?? Qt.rgba(0, 0, 0, 0)))
            }
            Rectangle { // top light: the face below leaves a one-pixel crescent
                x: card.hair
                y: card.hair
                width: card.width - 2 * card.hair
                height: card.height - 2 * card.hair
                radius: Math.max(0, card.radius - card.hair)
                color: Qt.tint(card.face, (card.look?.cardHighlight ?? Qt.rgba(0, 0, 0, 0)))
            }
            Rectangle {
                x: card.hair
                y: 2 * card.hair
                width: card.width - 2 * card.hair
                height: card.height - 3 * card.hair
                radius: Math.max(0, card.radius - card.hair)
                color: card.face
                Behavior on color { ColorAnimation { duration: (card.look?.animFast ?? 0) } }
            }
        }
    }
}
