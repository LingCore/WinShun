import QtQuick

// toggle: an on/off switch's track and knob (ToggleSwitch, 40 × 20 by the
// host); a check box as a square one (`kind` "check"). Windows 11's: an
// outlined track off, the accent fill on, the knob growing under the mouse.
Item {
    id: part

    property QtObject look: null
    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    property bool mixed: false // the knob in the middle
    property bool active: true // false: not read yet, just the track
    property string kind // "check": a check box
    property real dpr: 1

    readonly property bool lit: checked && !mixed && active
    readonly property real edge: Math.max(1, Math.round(dpr)) / dpr

    // The host's switch was taken for another (a recycled row) or shown
    // again: no animation is left half way.
    // (complete() on a Behavior's animation does not end it: the Behaviors
    // are switched off while each property is set to where it is going.)
    property bool still: false
    function settle() {
        still = true
        track.color = Qt.binding(() => track.goal)
        knob.x = Qt.binding(() => knob.goalX)
        knob.color = Qt.binding(() => knob.goal)
        still = false
    }

    Rectangle {
        id: track
        anchors.fill: parent
        radius: part.kind === "check" ? (part.look ? part.look.controlRadius : 0) : (part.look ? part.look.pill(height) : 0)
        // Fading to or from "transparent" would pass through black: from the fill's own colour instead.
        readonly property color goal: !part.look ? "transparent" : part.lit ? part.look.accentFill
               : part.hovered ? part.look.controlHover
               : Qt.rgba(part.look.controlHover.r, part.look.controlHover.g, part.look.controlHover.b, 0)
        color: goal
        border.width: part.lit ? 0 : part.edge
        border.color: part.look ? part.look.subtext : "transparent"

        Behavior on color { enabled: !part.still; ColorAnimation { duration: 120 } }
    }

    Rectangle {
        id: knob
        readonly property real size: part.pressed || part.hovered ? 14 : 12
        visible: part.kind !== "check" && part.active
        anchors.verticalCenter: parent.verticalCenter
        readonly property real goalX: part.mixed ? (parent.width - width) / 2 : part.checked ? parent.width - width - 4 : 4
        readonly property color goal: !part.look ? "transparent" : part.lit ? part.look.accentText : part.look.subtext
        x: goalX
        width: part.pressed ? 17 : size
        height: size
        radius: size / 2
        color: goal

        Behavior on x { enabled: !part.still; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        Behavior on color { enabled: !part.still; ColorAnimation { duration: 120 } }
    }

    Text { // a check box's mark
        visible: part.kind === "check" && part.lit
        anchors.centerIn: parent
        text: "" // CheckMark
        font.family: part.look ? part.look.iconFont : ""
        font.pixelSize: Math.round(part.height * 0.6)
        color: part.look ? part.look.accentText : "transparent"
    }
}
