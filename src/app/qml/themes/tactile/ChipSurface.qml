import QtQuick

// A type key (and every clickable chip: clipboard groups, tags). A key whose
// icon keeps its type colour; checked it is pressed down and filled with the
// type's deep colour, white label and icon. 不限 (isAll) instead carries the
// lamp at its left: checked it sinks and darkens like a pressed scope key
// (no colour) and the lamp lights orange. Only 不限 has a lamp.
Item {
    id: chip

    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    // -2 文件夹, -1 不限, 0.. TypeStrip.dotColors order (wraps); see Look.hueKey
    property int hueIndex: -1
    property bool isAll: false
    property string role           // "" | type | group | filter: all keys; only type keys carry hues
    property real corner: -1
    property real dpr: 1
    readonly property real radius: corner >= 0 ? corner : look ? look.keyRadius : 0

    property QtObject look // injected by Look.qml
    readonly property string hueKey: look ? look.hueKey(hueIndex) : ""
    readonly property bool coloured: !isAll && hueKey.length > 0
    readonly property color hueColor: !look ? "transparent" : coloured ? look.typeHue(hueKey) : look.ink2

    // for the host
    readonly property real contentOffsetY: key.contentOffsetY
    readonly property real leadingInset: key.leadingInset
    readonly property color foreground: key.foreground
    readonly property color iconColor: !look ? "transparent" : !enabled ? look.ink3 : checked && coloured ? "#FFFFFF" : hueColor
    readonly property int fontWeight: key.fontWeight
    readonly property real disabledOpacity: 1

    KeySurface {
        id: key
        anchors.fill: parent
        hovered: chip.hovered
        pressed: chip.pressed
        checked: chip.checked
        enabled: chip.enabled
        dpr: chip.dpr
        look: chip.look
        corner: chip.radius
        indicator: chip.isAll
        downFill: chip.coloured && chip.checked ? (chip.look?.typeDeep(chip.hueKey) ?? Qt.rgba(0, 0, 0, 0)) : "transparent"
    }
}
