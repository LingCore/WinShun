import QtQuick

// segmented: one choice out of a few, side by side (the launcher's scopes,
// the footer's sort, FileViewSwitch, PageTabs, a setting's choices). The
// host lays out the labels over it; this draws the rest. Fluent's is
// WinUI's SelectorBar: no track, a hover fill per segment, and a short
// accent pill under the chosen text tab. Icon choices use accent ink and a
// neutral selected surface instead of putting an underline below a glyph.
Item {
    id: part

    property QtObject look: null
    property int count: 0
    property int currentIndex: -1
    property int hoveredIndex: -1
    property int pressedIndex: -1
    property bool iconOnly: false
    property var segments: [] // [{x, width, label}]: label = the words' part of the width (a chip may follow them)
    property real dpr: 1

    function snap(v) { return Math.round(v * dpr) / dpr }
    function labelColor(i: int): color {
        if (!look) return "transparent"
        if (!enabled) return look.faint
        if (iconOnly && i === currentIndex) return look.accent
        return i === currentIndex || i === hoveredIndex || i === pressedIndex ? look.text : look.subtext
    }

    Repeater {
        model: part.segments.length

        Rectangle {
            required property int index
            x: part.segments[index].x
            y: part.iconOnly ? part.snap(2) : 0
            width: part.segments[index].width
            height: part.height - 2 * y
            radius: part.look ? part.look.largeControlRadius : 0
            color: !part.look ? "transparent"
                   : part.enabled && index === part.pressedIndex ? part.look.pressed
                   : (part.enabled && index === part.hoveredIndex) || (part.iconOnly && index === part.currentIndex) ? part.look.hover
                   : Qt.rgba(part.look.hover.r, part.look.hover.g, part.look.hover.b, 0)

            Behavior on color { ColorAnimation { duration: 100 } }
        }
    }

    Rectangle { // the chosen one's pill
        id: pill
        readonly property var at: part.currentIndex >= 0 && part.currentIndex < part.segments.length ? part.segments[part.currentIndex] : null
        visible: at !== null && !part.iconOnly
        width: 16
        height: part.snap(3)
        radius: part.look ? part.look.pill(height) : 0
        x: at ? part.snap(at.x + ((at.label !== undefined ? at.label : at.width) - width) / 2) : 0
        y: part.height - height - part.snap(1)
        color: part.look ? part.look.accentLine : "transparent"
        transform: Scale { id: stretch; origin.x: pill.width / 2 }

        Behavior on x {
            NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
        }
        readonly property int chosen: part.currentIndex
        onChosenChanged: if (visible) stretchAnim.restart()

        SequentialAnimation {
            id: stretchAnim
            NumberAnimation { target: stretch; property: "xScale"; to: 1.9; duration: 90; easing.type: Easing.OutCubic }
            NumberAnimation { target: stretch; property: "xScale"; to: 1; duration: 110; easing.type: Easing.OutCubic }
        }
    }
}
