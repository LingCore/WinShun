pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: part

    property QtObject look: null
    property int count: 0
    property int currentIndex: -1
    property int hoveredIndex: -1
    property int pressedIndex: -1
    property bool iconOnly: false
    property string role
    property var segments: []
    property real dpr: 1
    readonly property bool tabs: !iconOnly && (role === "tabs" || role === "scope" || role === "sort")

    function snap(v: real): real { return Math.round(v * dpr) / dpr }

    function labelColor(i: int): color {
        if (!look) return "transparent"
        if (!enabled) return look.faint
        if (iconOnly && i === currentIndex) return look.accent
        return i === currentIndex || i === hoveredIndex || i === pressedIndex ? look.text : look.subtext
    }

    Rectangle {
        anchors.fill: parent
        visible: !part.tabs
        radius: part.look ? part.look.largeControlRadius : 0
        color: part.look ? part.look.track : "transparent"
    }
    Repeater {
        model: part.segments.length
        GlassSurface {
            required property int index
            readonly property bool chosen: index === part.currentIndex
            readonly property bool over: part.enabled && index === part.hoveredIndex
            readonly property bool down: part.enabled && index === part.pressedIndex
            look: part.look
            dpr: part.dpr
            x: part.segments[index].x + (part.tabs ? 0 : 2 / Math.max(1, dpr))
            y: part.tabs ? 0 : 2 / Math.max(1, dpr)
            width: Math.max(0, part.segments[index].width - 2 * y)
            height: Math.max(0, part.height - 2 * y)
            visible: (!part.tabs && chosen) || over || down
            baseColor: !look ? "transparent" : down ? look.pressed : part.tabs ? look.hover : chosen ? look.control : look.hover
            edgeColor: look && !part.tabs && chosen ? look.cardBorder : "transparent"
        }
    }

    Rectangle {
        id: pill
        readonly property var segment: part.currentIndex >= 0 && part.currentIndex < part.segments.length ? part.segments[part.currentIndex] : null
        readonly property bool animate: part.look !== null && part.look.animations && (part.Window.window?.visible ?? false)
        property bool sliding: false
        Timer { id: slideWindow; interval: (part.look ? part.look.animMove : 0) + 50; onTriggered: pill.sliding = false }
        readonly property int chosen: part.currentIndex
        onChosenChanged: {
            if (visible && animate) {
                sliding = true
                slideWindow.restart()
            } else {
                sliding = false
                slideWindow.stop()
            }
        }
        onAnimateChanged: if (!animate) { sliding = false; slideWindow.stop() }
        visible: part.tabs && segment !== null
        width: 16
        height: part.snap(3)
        x: segment ? part.snap(segment.x + ((segment.label !== undefined ? segment.label : segment.width) - width) / 2) : 0
        y: part.height - height - part.snap(1)
        radius: part.look ? part.look.pill(height) : 0
        color: part.look ? part.look.accentLine : "transparent"
        Behavior on x {
            enabled: pill.animate && pill.sliding
            NumberAnimation { duration: part.look ? part.look.animMove : 0; easing.type: Easing.OutCubic }
        }
    }
}
