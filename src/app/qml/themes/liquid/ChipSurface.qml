import QtQuick

GlassSurface {
    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    property bool isAll: false
    property int hueIndex: -1
    readonly property bool filled: checked && !isAll
    // Over whatever is behind (the panel is translucent: the opaque controlHover was the same grey as it).
    readonly property color hoverTint: look && look.dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.06)
    readonly property var foreground: !look ? undefined : filled ? look.accentText : look.text

    radius: look ? look.pill(height) : 0
    baseColor: !look ? "transparent" : filled ? look.accentFill : pressed ? look.controlPressed
               : checked ? look.chipRest : hovered ? hoverTint : "transparent"
    edgeColor: "transparent"
}
