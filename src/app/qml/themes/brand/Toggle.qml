import QtQuick
import QtQuick.Shapes

// toggle: on/off switch, or a check box (kind "check"). Off: a 1 px grey
// outline (switch: with a grey knob). On: the neutral ink fill — near-black
// in light, near-white in dark — with the knob / tick in the opposite tone;
// the brand colours stay on the ring and the matches. The knob grows under
// the pointer and stretches while pressed, 120 ms, keeping the same room on
// all its sides. mixed: knob in the middle / a dash. active false: not read
// yet, just the outline. Fills its host (40 × 20; a check box 20 × 20).
Item {
    id: sw

    property QtObject look
    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    property bool mixed: false
    property bool active: true
    property string kind: ""
    property real dpr: 1
    // enabled: Item's own (inherited from the host)

    readonly property bool isCheck: kind === "check"
    readonly property bool lit: (checked || (isCheck && mixed)) && active

    implicitWidth: look ? (isCheck ? look.toggleHeight : look.toggleWidth) : 0
    implicitHeight: look ? look.toggleHeight : 0
    opacity: enabled ? 1 : 0.45
    readonly property real disabledOpacity: 1 // it dims itself (above)

    // Hook.call("settle"): every running animation to its end at once (a
    // recycled row's switch is its new state from the first frame). A
    // Behavior's animation neither jumps on complete() nor may be stopped
    // (not a root node): write the end values with the Behaviors off, which
    // ends the running ones there.
    property bool still: false
    function settle() {
        still = true
        track.color = Qt.binding(() => track.goal)
        knob.x = Qt.binding(() => knob.goalX)
        knob.width = Qt.binding(() => knob.goalWidth)
        knob.color = Qt.binding(() => knob.goal)
        still = false
    }

    Rectangle {
        id: track
        anchors.fill: parent
        radius: sw.look ? (sw.isCheck ? sw.look.smallControlRadius : sw.look.pillRadius(height)) : 0
        readonly property color goal: !sw.look ? "transparent"
             : sw.lit ? sw.look.neutralFill
             : sw.hovered && sw.enabled ? sw.look.hoverFill
             : Qt.rgba(sw.look.hoverFill.r, sw.look.hoverFill.g, sw.look.hoverFill.b, 0)
        color: goal
        border.width: sw.look ? (sw.lit ? 0 : sw.look.edge(sw.dpr)) : 0
        border.color: sw.look ? sw.look.ink3 : "transparent"
        Behavior on color { enabled: !sw.still; ColorAnimation { duration: sw.look ? sw.look.animFast : 0 } }
    }

    // ---- switch knob ----
    Rectangle {
        id: knob
        visible: !sw.isCheck && sw.active
        readonly property real size: sw.enabled && (sw.hovered || sw.pressed) ? 14 : 12
        readonly property real gap: (sw.height - size) / 2 // the same room beside the knob as above and below
        readonly property real goalWidth: sw.enabled && sw.pressed ? 17 : size
        readonly property real goalX: sw.look ? (sw.look.snap(sw.mixed ? (sw.width - goalWidth) / 2 : sw.checked ? sw.width - goalWidth - gap : gap, sw.dpr)) : 0
        readonly property color goal: sw.look ? (sw.lit ? sw.look.neutralFillInk : sw.look.ink2) : "transparent"
        width: goalWidth
        height: size
        radius: sw.look ? sw.look.pillRadius(size) : 0
        y: sw.look ? sw.look.snap((sw.height - height) / 2, sw.dpr) : 0
        x: goalX
        color: goal
        Behavior on x { enabled: !sw.still; NumberAnimation { duration: sw.look ? sw.look.animFast : 0; easing.type: Easing.OutCubic } }
        Behavior on width { enabled: !sw.still; NumberAnimation { duration: sw.look ? sw.look.animFast : 0; easing.type: Easing.OutCubic } }
        Behavior on color { enabled: !sw.still; ColorAnimation { duration: sw.look ? sw.look.animFast : 0 } }
    }

    // ---- check mark / dash ----
    Shape {
        visible: sw.isCheck && sw.lit
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: "transparent"
            strokeColor: sw.look ? sw.look.neutralFillInk : "transparent"
            strokeWidth: 1.6
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg {
                readonly property real s: sw.height / 20
                path: sw.mixed ? "M" + 6 * s + " " + 10 * s + "H" + 14 * s
                               : "M" + 5.5 * s + " " + 10.25 * s + "L" + 8.5 * s + " " + 13.25 * s + "L" + 14.5 * s + " " + 7 * s
            }
        }
    }
}
