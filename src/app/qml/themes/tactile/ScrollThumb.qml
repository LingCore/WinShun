import QtQuick

// A list's scroll bar: the track fills the box the host gives it (thin, wider
// while the pointer is near: `active`); the thumb is at `position`, `extent`
// tall. It only tells where you are, so it stays flat; only under the pointer,
// where it can be dragged, does the thumb become a slim key (held: down).
Item {
    id: bar

    property bool active: false    // the pointer is near: the host widens the bar
    property bool hovered: false   // over the thumb
    property bool pressed: false
    property real position: 0
    property real extent: 0
    property real dpr: 1

    property QtObject look // injected by Look.qml
    readonly property bool lifted: hovered || pressed

    // track: a faint groove while near
    Rectangle {
        visible: bar.active
        anchors.fill: parent
        radius: bar.look.pill(width)
        color: Qt.rgba(bar.look.slot.r, bar.look.slot.g, bar.look.slot.b, 0.7)
    }
    Rectangle {
        visible: !bar.lifted
        y: bar.position
        width: bar.width
        height: bar.extent
        radius: bar.look.pill(width)
        color: Qt.rgba(bar.look.ink3.r, bar.look.ink3.g, bar.look.ink3.b, bar.active ? 0.6 : 0.45)
    }
    KeySurface {
        visible: bar.lifted
        y: bar.position
        width: bar.width
        height: bar.extent
        corner: bar.look.pill(width)
        pressed: bar.pressed
        hovered: bar.hovered
        drop: false
        dpr: bar.dpr
        look: bar.look
    }
}
