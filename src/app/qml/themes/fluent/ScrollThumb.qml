import QtQuick

// scrollBar: an overlay scroll bar's track and thumb, as wide as the host
// makes it (a thin line, wider while the mouse is near: `active`).
// `position` and `extent` are the thumb's y and height in this item.
Item {
    id: part

    property QtObject look: null
    property bool active: false // the mouse is near: the bar is wide
    property bool hovered: false // over the thumb
    property bool pressed: false
    property real position: 0
    property real extent: 0
    property real dpr: 1

    Rectangle {
        anchors.fill: parent
        radius: part.look ? part.look.pill(width) : 0
        color: part.active && part.look ? part.look.track : "transparent"
    }

    Rectangle {
        y: part.position
        width: parent.width
        height: part.extent
        radius: part.look ? part.look.pill(width) : 0
        color: !part.look ? "transparent" : part.pressed ? part.look.text
               : part.hovered ? Qt.lighter(part.look.subtext, 1.15) : part.active ? part.look.subtext : part.look.faint
        opacity: part.active ? 1 : 0.6

        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
}
