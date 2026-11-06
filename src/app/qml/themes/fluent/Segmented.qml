import QtQuick

// segmented: one choice out of a few, side by side (the launcher's scopes,
// the footer's sort, FileViewSwitch, PageTabs, a setting's choices). The
// host lays out the labels over it; this draws the rest. Fluent's is
// WinUI's SelectorBar: no track, a hover fill per segment, and a short
// accent pill under the chosen one that slides to the next.
Item {
    id: part

    property QtObject look: null
    property int count: 0
    property int currentIndex: -1
    property int hoveredIndex: -1
    property int pressedIndex: -1
    property var segments: [] // [{x, width, label}]: label = the words' part of the width (a chip may follow them)
    property real dpr: 1

    function snap(v) { return Math.round(v * dpr) / dpr }

    Repeater {
        model: part.segments.length

        Rectangle {
            required property int index
            x: part.segments[index].x
            width: part.segments[index].width
            height: part.height
            radius: part.look ? part.look.largeControlRadius : 0
            color: !part.look ? "transparent"
                   : index === part.pressedIndex ? part.look.pressed
                   : index === part.hoveredIndex ? part.look.hover
                   : Qt.rgba(part.look.hover.r, part.look.hover.g, part.look.hover.b, 0)

            Behavior on color { ColorAnimation { duration: 100 } }
        }
    }

    Rectangle { // the chosen one's pill
        id: pill
        readonly property var at: part.currentIndex >= 0 && part.currentIndex < part.segments.length ? part.segments[part.currentIndex] : null
        visible: at !== null
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
