import QtQuick

GlassSurface {
    id: part

    property bool hovered: false
    property bool pressed: false
    property bool checked: false
    property bool current: false
    property bool highlighted: false
    property bool danger: false
    property bool flat: true
    property real corner: -1
    readonly property bool filled: highlighted || danger || checked
    readonly property color inkFill: !look ? "transparent" : danger ? look.dangerFill : look.accentFill
    readonly property var foreground: !look ? undefined : filled ? look.accentText : current ? look.accent : undefined

    radius: corner >= 0 ? corner : look ? look.controlRadius : 0
    baseColor: !look ? "transparent"
               : filled ? (pressed ? Qt.darker(inkFill, 1.12) : inkFill)
               : pressed ? (flat ? look.pressed : look.controlPressed)
               : hovered || current ? (flat ? look.hover : look.controlHover)
               : flat ? "transparent" : look.control
    edgeColor: !look || flat || filled ? "transparent" : look.controlBorder
}
