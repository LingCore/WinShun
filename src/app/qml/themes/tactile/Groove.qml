import QtQuick

// Something cut into the panel: the slot colour with an inner shadow along
// the top only (one device pixel, then a few pixels of fade), which tapers
// round the top corners. Used by the search groove, inputs, switch grooves.
Item {
    id: groove

    property real radius: look.searchRadius
    property real dpr: 1
    property color tone: look.slot // not "fill": Hook passes a window body colour by that name
    property color shade: look.slotShade
    property color fade: look.slotFade

    property QtObject look // injected by Look.qml
    readonly property real hair: look.hair(dpr)

    Rectangle {
        anchors.fill: parent
        radius: groove.radius
        color: Qt.tint(groove.tone, groove.shade)
    }
    Rectangle {
        y: groove.hair
        width: groove.width
        height: groove.height - groove.hair
        radius: groove.radius
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.tint(groove.tone, groove.fade) }
            GradientStop { position: groove.height > 8 ? 4 / (groove.height - groove.hair) : 0.5; color: groove.tone }
            GradientStop { position: 1; color: groove.tone }
        }
    }
}
