import QtQuick

// keySurface: anything that can be clicked (FlatButton, RowAction, the pin
// and expand buttons, the footer's). Fluent's: flat until hovered (a
// toolbar's icon button) or a control fill with a 1 px neutral edge (a
// push button); checked (on, a toggle button) is the accent fill, with
// white marks on it, as `highlighted` (a primary button) and `danger` (an
// armed delete) are filled.
Rectangle {
    id: part

    property QtObject look: null
    property bool hovered: false
    property bool pressed: false
    property bool checked: false
    property bool current: false // what it opens is open (a row's preview button): the hover fill stays
    property bool highlighted: false
    property bool danger: false
    property bool flat: true
    property real corner: -1 // the host's own radius, or the style's
    property real dpr: 1

    readonly property bool filled: highlighted || danger || checked
    readonly property color base: !look ? "transparent" : danger ? look.dangerFill : look.accentFill
    // Keep each host's normal ink, but never leave its dark glyph on a filled
    // checked key (e.g. the panel sidebar or a recording option).
    readonly property var foreground: !look ? undefined : filled ? look.accentText : current ? look.accent : undefined

    radius: corner >= 0 || !look ? Math.max(0, corner) : look.controlRadius
    color: !look ? "transparent"
           : filled ? (pressed ? Qt.darker(base, 1.15) : hovered ? Qt.lighter(base, 1.08) : base)
           : flat ? (pressed ? look.pressed : hovered || current ? look.hover : Qt.rgba(look.hover.r, look.hover.g, look.hover.b, 0))
           : (pressed ? look.controlPressed : hovered ? look.controlHover : look.control)
    border.width: filled || flat ? 0 : Math.max(1, Math.round(dpr)) / dpr
    border.color: look ? look.controlBorder : "transparent"

    Behavior on color { ColorAnimation { duration: 100 } }
}
