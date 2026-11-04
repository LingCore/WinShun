import QtQuick

// chipSurface: a type chip, clipboard category or tag. Flat with a coloured
// line icon (the type palette) and grey words; under the pointer a neutral
// fill; chosen, the whole chip fills with the type's deep colour under white
// words and icon. "不限" (isAll) chosen fills with the neutral ink instead.
// hueIndex: the strip's place: -2 文件夹, -1 不限 / no hue, 0.. the kinds in
// TypeStrip.dotColors order (wraps past the palette's length).
Item {
    id: chip

    property QtObject look
    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    property bool isAll: false
    property int hueIndex: -1
    property real dpr: 1

    readonly property var tone: look.chipHue(hueIndex)
    readonly property real radius: look.chipRadius
    // For the host's words and icon.
    readonly property color ink: checked ? (isAll || hueIndex === -1 ? look.neutralFillInk : look.accentText) : hovered ? look.ink : look.ink2
    readonly property color iconInk: checked ? (isAll || hueIndex === -1 ? look.neutralFillInk : look.accentText) : tone.ink
    readonly property int weight: Font.Normal
    // Hook.out names.
    readonly property color foreground: ink
    readonly property color iconColor: iconInk
    readonly property int fontWeight: weight

    Rectangle {
        anchors.fill: parent
        radius: chip.radius
        color: chip.checked ? (chip.isAll || chip.hueIndex === -1 ? chip.look.neutralFill : chip.tone.deep)
             : chip.pressed ? chip.look.pressedFill
             : chip.hovered ? chip.look.hoverFill
             : Qt.rgba(chip.look.hoverFill.r, chip.look.hoverFill.g, chip.look.hoverFill.b, 0)
        Behavior on color { ColorAnimation { duration: chip.look.animFast } }
    }
}
