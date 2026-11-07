import QtQuick

GlassSurface {
    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    property bool isAll: false
    property int hueIndex: -1
    readonly property bool filled: checked && !isAll
    readonly property var foreground: !look ? undefined : filled ? look.accentText : look.text

    radius: look ? look.pill(height) : 0
    baseColor: !look ? "transparent" : filled ? look.accentFill : pressed ? look.controlPressed
               : checked ? look.chipRest : hovered ? look.controlHover : "transparent"
    edgeColor: "transparent"
}
