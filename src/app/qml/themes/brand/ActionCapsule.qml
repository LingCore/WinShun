import QtQuick

// actionCapsule: the faint neutral capsule behind a result row's buttons
// (shown with the selected row). The host sizes it round its `count` keys:
// `padding` all round them, keys of `keyRadius` (concentric: 9 − 2 = 7).
// Flat — the keys on it are what is clickable, and they raise themselves.
Item {
    id: capsule

    property QtObject look
    property int count: 0
    property real dpr: 1

    readonly property real radius: look ? look.capsuleRadius : 0
    readonly property real padding: look ? look.capsulePadding : 0
    readonly property real keyRadius: look ? look.capsuleKeyRadius : 0

    Rectangle {
        anchors.fill: parent
        radius: capsule.radius
        color: capsule.look ? capsule.look.capsuleFill : "transparent"
    }
}
