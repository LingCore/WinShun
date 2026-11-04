import QtQuick

// scrollBar: the track and thumb of ListScrollBar. The host sizes the Hook
// (a thin 3 px line at rest, wider when the pointer comes near) and passes
// where the thumb is: position = its top, extent = its height, in the Hook.
// Neutral throughout: a faint track only while active (wide); the thumb in
// the third grey, the second under the pointer, ink while dragged.
Item {
    id: bar

    property QtObject look
    property bool active: false // wide: the pointer is near
    property bool hovered: false // over the thumb
    property bool pressed: false // dragging the thumb
    property real position: 0
    property real extent: 24
    property real dpr: 1

    Rectangle {
        anchors.fill: parent
        radius: bar.look.pillRadius(width)
        color: bar.active ? bar.look.segTrack : "transparent"
    }
    Rectangle {
        width: parent.width
        y: bar.look.snap(bar.position, bar.dpr)
        height: bar.look.snap(Math.max(bar.width, bar.extent), bar.dpr)
        radius: bar.look.pillRadius(width)
        color: bar.pressed ? bar.look.ink : bar.hovered ? bar.look.ink2 : bar.look.ink3
        opacity: bar.active || bar.pressed ? 1 : 0.6
        Behavior on opacity { NumberAnimation { duration: bar.look.animFast } }
    }
}
