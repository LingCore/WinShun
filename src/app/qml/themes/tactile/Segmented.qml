pragma ComponentBehavior: Bound
import QtQuick

// A segmented choice, two ways:
// - a slide switch (default; sort order, FileViewSwitch, PageTabs): a groove
//   with one key as the knob, 2 px from all four sides, sliding to the
//   current segment;
// - role "scope" (the scope keys in the search groove): a row of separate
//   keys, 4 px apart, each with a lamp; the current one is pressed down, lamp lit.
// The host lays its labels over segmentX(i) / segmentWidth(i), centred and
// moved by labelOffsetY(i), in labelColor(i) / labelWeight(i), after
// leadingInset (the lamp's room) for the scope keys.
Item {
    id: seg

    property int count: 0
    property int currentIndex: -1
    // The host's segments, in this item: [{x, width}] (or plain widths, laid
    // out here 4 apart; empty: equal parts).
    property var segments: []
    property int hoveredIndex: -1
    property int pressedIndex: -1
    property string role           // "" | scope | sort | tabs | switch
    property real corner: -1       // the keys' (knob's) radius; -1: 2 in the groove, 6 on a switch
    property real dpr: 1

    property QtObject look // injected by Look.qml
    readonly property bool keyedMode: role === "scope"
    readonly property real keyRadius: corner >= 0 ? corner : !look ? 0 : keyedMode ? look.grooveKeyRadius : look.knobRadius
    readonly property real pad: keyedMode || !look ? 0 : look.switchPadding
    readonly property real gap: keyedMode && look ? look.keyGap : 0
    readonly property int n: Math.max(count, segments ? segments.length : 0)

    function given(i: int): var { return segments && segments.length > i ? segments[i] : undefined }
    function segmentWidth(i: int): real {
        const s = given(i)
        if (s !== undefined)
            return typeof s === "number" ? s : s.width
        return n > 0 ? (width - 2 * pad - gap * (n - 1)) / n : 0
    }
    function segmentX(i: int): real {
        const s = given(i)
        if (s !== undefined && typeof s !== "number")
            return s.x
        let x = pad
        for (let k = 0; k < i; ++k)
            x += segmentWidth(k) + gap
        return x
    }
    readonly property real leadingInset: keyedMode && look ? look.ledInset + look.ledSize + look.ledGap : 0
    function labelOffsetY(i: int): real {
        if (!keyedMode)
            return -look.keyTravel / 2 // labels sit on the knob's face, above its side
        return (i === currentIndex || i === pressedIndex) ? look.keyTravel / 2 : -look.keyTravel / 2
    }
    function labelColor(i: int): color {
        if (!enabled)
            return look.ink3
        if (i === currentIndex)
            return look.ink
        if (keyedMode)
            return i === hoveredIndex || i === pressedIndex ? look.ink : look.keyInk
        return i === hoveredIndex ? look.ink : look.ink3
    }
    function labelWeight(i: int): int { return i === currentIndex ? Font.Bold : Font.Normal }

    readonly property real naturalWidth: {
        let w = 2 * pad
        for (let k = 0; k < n; ++k)
            w += (given(k) !== undefined ? segmentWidth(k) : 0) + (k > 0 ? gap : 0)
        return w
    }
    implicitWidth: naturalWidth

    // ---- slide switch ----
    Groove {
        visible: !seg.keyedMode
        anchors.fill: parent
        radius: seg.look ? (seg.look?.switchRadius ?? 0) : 0
        dpr: seg.dpr
        look: seg.look
    }
    KeySurface {
        visible: !seg.keyedMode && seg.currentIndex >= 0 && seg.currentIndex < seg.n
        x: seg.segmentX(Math.max(0, seg.currentIndex))
        y: seg.pad
        width: seg.segmentWidth(Math.max(0, seg.currentIndex))
        height: seg.height - 2 * seg.pad
        corner: seg.keyRadius
        hovered: seg.hoveredIndex === seg.currentIndex
        enabled: seg.enabled
        dpr: seg.dpr
        look: seg.look
        Behavior on x { NumberAnimation { duration: seg.look ? (seg.look?.animSlide ?? 0) : 0; easing.type: Easing.OutCubic } }
        Behavior on width { NumberAnimation { duration: seg.look ? (seg.look?.animSlide ?? 0) : 0; easing.type: Easing.OutCubic } }
    }

    // ---- keyed: separate keys with lamps ----
    Repeater {
        model: seg.keyedMode && seg.look ? seg.n : 0
        KeySurface {
            required property int index
            x: seg.segmentX(index)
            width: seg.segmentWidth(index)
            height: seg.height
            corner: seg.keyRadius
            indicator: true
            checked: index === seg.currentIndex
            pressed: index === seg.pressedIndex
            hovered: index === seg.hoveredIndex
            enabled: seg.enabled
            dpr: seg.dpr
            look: seg.look
        }
    }
}
