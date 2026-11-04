import QtQuick
import QtQuick.Effects

// keySurface: the face of any button or icon button.
// flat (toolbar icon buttons, the footer's sorting): nothing at rest, a soft
// neutral fill under the pointer, a firmer one pressed; checked: the track's
// tint (the host bolds its words). Not flat: a white key with a faint 1 px
// edge. highlighted (primary) / danger (an armed delete): the deep fill under
// white words. raised (brand only; buttons in a track or capsule — the row
// actions, an open 更多): hovered or checked they rise as a white pill with a
// hairline shadow, as the prototype's row capsule.
Item {
    id: key

    property QtObject look
    property bool hovered: false
    property bool pressed: false
    property bool checked: false
    property bool highlighted: false
    property bool danger: false
    property bool flat: true
    property bool raised: false // role "rowAction" (Hook derives it)
    property string role: "" // "fieldKey": a key in the search field, bare at rest as a flat one
    property real corner: -1 // the host's radius (concentric with its container), -1: the style's
    property real dpr: 1
    // enabled: Item's own (inherited from the host)

    readonly property real radius: corner >= 0 ? corner : look.keyRadius
    readonly property bool filled: highlighted || danger
    readonly property bool pill: raised && enabled && (hovered || checked || pressed) && !filled
    // Nothing at rest: flat keys, the row actions (on their capsule), the field's keys.
    readonly property bool bare: flat || raised || role === "fieldKey"
    // For the host's glyph and words (Hook.out): bare keys rest in ink2, firm under the pointer.
    readonly property color ink: filled ? look.accentText : !enabled ? look.ink3
                               : !bare || hovered || checked || pressed ? look.ink : look.ink2
    readonly property int weight: checked && !raised ? Font.Bold : Font.Normal
    readonly property color foreground: ink
    readonly property color iconColor: ink
    readonly property int fontWeight: weight
    readonly property real contentOffsetY: 0 // keys do not sink when pressed
    readonly property real disabledOpacity: 1 // the face dims itself (below) and the words go ink3

    opacity: enabled ? 1 : 0.5

    RectangularShadow {
        visible: key.pill
        anchors.fill: parent
        radius: key.radius
        offset.y: 1
        blur: 2.4
        color: key.look.segShadow
    }
    Rectangle {
        readonly property color baseFill: key.danger ? key.look.dangerFill : key.look.accentFill
        anchors.fill: parent
        radius: key.radius
        color: key.filled ? (key.pressed ? Qt.darker(baseFill, 1.12) : key.hovered ? Qt.darker(baseFill, 1.06) : baseFill)
             : key.pill ? (key.pressed ? key.look.controlPressed : key.look.segPill)
             : !key.enabled ? (key.bare ? "transparent" : key.look.control)
             : key.pressed ? (key.bare ? key.look.pressedFill : key.look.controlPressed)
             : key.hovered ? (key.bare ? key.look.hoverFill : key.look.controlHover)
             : key.checked ? key.look.segTrack
             : key.bare ? Qt.rgba(key.look.hoverFill.r, key.look.hoverFill.g, key.look.hoverFill.b, 0) : key.look.control
        border.width: (key.pill && !key.look.dark) || (!key.bare && !key.filled && !key.pill) ? key.look.edge(key.dpr) : 0
        border.color: key.pill ? key.look.segRing : key.look.controlBorder
        Behavior on color { ColorAnimation { duration: key.look.animFast } }
    }
}
