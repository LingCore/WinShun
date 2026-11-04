import QtQuick

// An indicator lamp: a 6 px dot set into a key face. Off: a dimple in the
// side colour with a 1 px rim; lit: signal orange with a small soft glow.
// Made by a Loader in KeySurface, its own bindings can run before `look`
// is set: nothing is drawn until it is.
Item {
    id: lamp

    property bool lit: false
    property real dpr: 1
    property QtObject look // injected by Look.qml
    readonly property bool ready: look !== null
    readonly property color glow: ready ? look.ledGlow : "transparent"

    width: ready ? look.ledSize : 0
    height: width
    visible: ready

    // glow: two faint rings instead of a blur
    Rectangle {
        visible: lamp.lit
        anchors.centerIn: parent
        width: lamp.width + 4
        height: width
        radius: width / 2
        color: Qt.rgba(lamp.glow.r, lamp.glow.g, lamp.glow.b, lamp.glow.a * 0.35)
    }
    Rectangle {
        visible: lamp.lit
        anchors.centerIn: parent
        width: lamp.width + 2
        height: width
        radius: width / 2
        color: Qt.rgba(lamp.glow.r, lamp.glow.g, lamp.glow.b, lamp.glow.a * 0.6)
    }
    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: !lamp.ready ? "transparent" : lamp.lit ? (lamp.look?.led ?? Qt.rgba(0, 0, 0, 0)) : (lamp.look?.keySideColor ?? Qt.rgba(0, 0, 0, 0))
        border.width: lamp.lit || !lamp.ready ? 0 : (lamp.look?.hair(lamp.dpr) ?? 1)
        border.color: lamp.ready ? (lamp.look?.keyRim ?? Qt.rgba(0, 0, 0, 0)) : "transparent"
        Behavior on color { ColorAnimation { duration: 120 } }
    }
}
