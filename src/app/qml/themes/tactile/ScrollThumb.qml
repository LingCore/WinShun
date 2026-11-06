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
    function pill(size: real): real { return look ? look.pill(size) : 0 }
    readonly property color slot: look ? look.slot : "transparent"
    readonly property color ink3: look ? look.ink3 : "transparent"

    // track: a faint groove while near
    Rectangle {
        visible: bar.active
        anchors.fill: parent
        radius: bar.pill(width)
        color: Qt.rgba(bar.slot.r, bar.slot.g, bar.slot.b, 0.7)
    }
    Rectangle {
        visible: !bar.lifted
        y: bar.position
        width: bar.width
        height: bar.extent
        radius: bar.pill(width)
        color: Qt.rgba(bar.ink3.r, bar.ink3.g, bar.ink3.b, bar.active ? 0.6 : 0.45)
    }
    KeySurface {
        visible: bar.lifted
        y: bar.position
        width: bar.width
        height: bar.extent
        corner: bar.pill(width)
        pressed: bar.pressed
        hovered: bar.hovered
        drop: false
        dpr: bar.dpr
        look: bar.look
    }
}
