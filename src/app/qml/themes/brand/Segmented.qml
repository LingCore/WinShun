import QtQuick
import QtQuick.Effects

// segmented: a sunken neutral track with one white pill that slides to the
// current segment (180 ms, cubic-bezier(0,0,0,1)). The host draws the words
// (current: text + bold, the others the third grey). Fills its host; the
// pill keeps 2 from the track on every side and its radius is the track's
// minus 2 (concentric; 2 at the least). inField (brand only): inside the
// search field, where the track's radius is held to 2 (field 4 − its 6 inset).
// segments: [{x, width}] in the host (README); a number (one width for all)
// or a list of widths also works; empty: equal shares.
Item {
    id: seg

    property QtObject look
    property int count: 0
    property int currentIndex: -1
    property int hoveredIndex: -1
    property int pressedIndex: -1
    property var segments: []
    property bool inField: false
    property real dpr: 1

    readonly property real padding: look ? look.segPad : 0
    readonly property real radius: look ? (inField ? look.segInFieldRadius : look.segRadius) : 0
    readonly property real pillRadius: look ? (Math.max(look.rounded ? 2 : 0, radius - padding)) : 0
    readonly property int shown: Math.max(count, segments && segments.length ? segments.length : 0)

    // Hook.call("segmentX", [i]): segment i's left edge in the Hook (the
    // host lays its words out on these).
    function segmentX(i: int): real { return boxOf(i).x }

    function boxOf(i: int): var {
        const s = segments
        if (s && s.length > i && typeof s[i] === "object")
            return { x: s[i].x, width: s[i].width }
        let w = typeof s === "number" ? s : -1
        let x = padding
        for (let k = 0; k <= i; k++) {
            const wk = w >= 0 ? w : s && s.length > k ? s[k] : (width - 2 * padding) / Math.max(1, shown)
            if (k === i)
                return { x: x, width: wk }
            x += wk
        }
        return { x: padding, width: 0 }
    }

    Rectangle {
        anchors.fill: parent
        radius: seg.radius
        visible: !seg.inField // in the search row there is no track: the words alone
        color: seg.look ? seg.look.segTrack : "transparent"
    }
    Rectangle { // the segment under the pointer, when it is not the current one
        readonly property var box: seg.boxOf(Math.max(0, seg.hoveredIndex))
        visible: seg.hoveredIndex >= 0 && seg.hoveredIndex !== seg.currentIndex && seg.hoveredIndex < seg.shown
        x: box.x; y: seg.padding
        width: box.width; height: seg.height - 2 * seg.padding
        radius: seg.pillRadius
        color: seg.look ? (seg.pressedIndex === seg.hoveredIndex ? seg.look.pressedFill : seg.look.hoverFill) : "transparent"
    }

    Item {
        id: pill
        readonly property var box: seg.boxOf(Math.max(0, Math.min(seg.shown - 1, seg.currentIndex)))
        x: box.x
        y: seg.padding
        width: box.width
        height: seg.height - 2 * seg.padding
        visible: !seg.inField && seg.currentIndex >= 0 && seg.currentIndex < seg.shown
        Behavior on x { NumberAnimation { duration: seg.look ? seg.look.animMove : 0; easing.type: Easing.Bezier; easing.bezierCurve: seg.look ? seg.look.easeOut : [0, 0, 0, 1, 1, 1] } }
        Behavior on width { NumberAnimation { duration: seg.look ? seg.look.animMove : 0; easing.type: Easing.Bezier; easing.bezierCurve: seg.look ? seg.look.easeOut : [0, 0, 0, 1, 1, 1] } }

        RectangularShadow {
            anchors.fill: parent
            radius: seg.pillRadius
            offset.y: 1
            blur: 2.4
            color: seg.look ? seg.look.segShadow : "transparent"
        }
        Rectangle { // the 1 px ring sits inside the pill, so its gap to the track is a true 2
            anchors.fill: parent
            radius: seg.pillRadius
            color: seg.look ? seg.look.segPill : "transparent"
            border.width: seg.look ? (seg.look.dark ? 0 : seg.look.edge(seg.dpr)) : 0
            border.color: seg.look ? seg.look.segRing : "transparent"
        }
    }

    // In the search row: a short line under the chosen word, in the brand's ink.
    Rectangle {
        id: mark
        readonly property var at: seg.currentIndex >= 0 && seg.currentIndex < seg.segments.length ? seg.segments[seg.currentIndex] : null
        readonly property real labelWidth: at && at.label !== undefined ? at.label : at ? at.width : 0
        visible: seg.inField && at !== null
        width: seg.look ? seg.look.snap(18, seg.dpr) : 18
        height: seg.look ? seg.look.snap(2, seg.dpr) : 2
        radius: height / 2
        x: at && seg.look ? seg.look.snap(at.x + (labelWidth - width) / 2, seg.dpr) : 0
        y: seg.height - height - (seg.look ? seg.look.snap(2, seg.dpr) : 2)
        color: seg.look ? seg.look.accentFill : "transparent"
        Behavior on x { NumberAnimation { duration: seg.look ? seg.look.animMove : 0; easing.type: Easing.Bezier; easing.bezierCurve: seg.look ? seg.look.easeOut : [0, 0, 0, 1, 1, 1] } }
    }
}
