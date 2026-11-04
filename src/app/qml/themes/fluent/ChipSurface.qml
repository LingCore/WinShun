import QtQuick

// chipSurface: a pill to pick (the type strip, the clipboard's groups, a
// filter). Fluent's: nothing at rest, a hover fill, the accent fill when
// chosen; 不限 (isAll), the resting choice, chosen is neutral. The host
// writes on it in look.chipText(checked, isAll, hovered).
Rectangle {
    id: part

    property QtObject look: null
    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    property bool isAll: false
    property int hueIndex: -1
    property real dpr: 1

    radius: look ? look.pill(height) : 0
    color: !look ? "transparent"
           : checked ? (isAll ? look.chipRest : pressed ? Qt.darker(look.accentFill, 1.15) : look.accentFill)
           : pressed ? look.pressed : hovered ? look.hover
           : Qt.rgba(look.hover.r, look.hover.g, look.hover.b, 0)

    Behavior on color { ColorAnimation { duration: 120 } }
}
