pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A Segoe Fluent / MDL2 icon glyph, e.g. "" (search), or the name of an
// icon the font lacks, drawn here instead (see `drawn`).
Item {
    id: icon

    property string glyph
    property real size: 16
    property color color: Theme.subtext

    // Icons drawn like Fluent System Icons: a master per device-pixel size,
    // with outer edges on whole pixels and the stroke growing inwards (1 px at
    // 16 and 20, 1.5 px at 24 and 28), round caps, dots on whole pixels.
    // `stroke` and `fill` are SVG path data in device pixels; the SVG sources
    // are in resources/icons.
    readonly property var drawn: ({
        copyPath: { // a box holding "\\..": copy-path-<size>.svg
            16: {
                width: 1,
                stroke: "M3.5 2.5H12.5A2 2 0 0 1 14.5 4.5V11.5A2 2 0 0 1 12.5 13.5H3.5A2 2 0 0 1 1.5 11.5V4.5A2 2 0 0 1 3.5 2.5Z"
                        + "M3.5 5.5L5.5 10.5M6.5 5.5L8.5 10.5",
                fill: "M10 10H11V11H10ZM12 10H13V11H12Z"
            },
            20: {
                width: 1,
                stroke: "M4 2.5H16A2.5 2.5 0 0 1 18.5 5V15A2.5 2.5 0 0 1 16 17.5H4A2.5 2.5 0 0 1 1.5 15V5A2.5 2.5 0 0 1 4 2.5Z"
                        + "M3.5 6.5L6.5 13.5M6.5 6.5L9.5 13.5",
                fill: "M11.5 12H12.5A0.5 0.5 0 0 1 13 12.5V13.5A0.5 0.5 0 0 1 12.5 14H11.5A0.5 0.5 0 0 1 11 13.5V12.5A0.5 0.5 0 0 1 11.5 12Z"
                      + "M15.5 12H16.5A0.5 0.5 0 0 1 17 12.5V13.5A0.5 0.5 0 0 1 16.5 14H15.5A0.5 0.5 0 0 1 15 13.5V12.5A0.5 0.5 0 0 1 15.5 12Z"
            },
            24: {
                width: 1.5,
                stroke: "M4.25 2.75H19.75A2.5 2.5 0 0 1 22.25 5.25V18.75A2.5 2.5 0 0 1 19.75 21.25H4.25A2.5 2.5 0 0 1 1.75 18.75V5.25A2.5 2.5 0 0 1 4.25 2.75Z"
                        + "M4.5 7.75L7.5 16.25M8.5 7.75L11.5 16.25",
                fill: "M14.5 15H15.5A0.5 0.5 0 0 1 16 15.5V16.5A0.5 0.5 0 0 1 15.5 17H14.5A0.5 0.5 0 0 1 14 16.5V15.5A0.5 0.5 0 0 1 14.5 15Z"
                      + "M18.5 15H19.5A0.5 0.5 0 0 1 20 15.5V16.5A0.5 0.5 0 0 1 19.5 17H18.5A0.5 0.5 0 0 1 18 16.5V15.5A0.5 0.5 0 0 1 18.5 15Z"
            },
            28: {
                width: 1.5,
                stroke: "M5.75 3.75H22.25A3 3 0 0 1 25.25 6.75V21.25A3 3 0 0 1 22.25 24.25H5.75A3 3 0 0 1 2.75 21.25V6.75A3 3 0 0 1 5.75 3.75Z"
                        + "M5.5 8.75L9.5 19.25M9.5 8.75L13.5 19.25",
                fill: "M17.5 18H18.5A0.5 0.5 0 0 1 19 18.5V19.5A0.5 0.5 0 0 1 18.5 20H17.5A0.5 0.5 0 0 1 17 19.5V18.5A0.5 0.5 0 0 1 17.5 18Z"
                      + "M21.5 18H22.5A0.5 0.5 0 0 1 23 18.5V19.5A0.5 0.5 0 0 1 22.5 20H21.5A0.5 0.5 0 0 1 21 19.5V18.5A0.5 0.5 0 0 1 21.5 18Z"
            }
        }
    })[glyph] ?? null

    // The icon fonts' line box is exactly their em square, so drawn icons
    // line up with the glyphs.
    implicitWidth: drawn ? size : label.implicitWidth
    implicitHeight: drawn ? size : label.implicitHeight

    Text {
        id: label
        anchors.centerIn: parent
        visible: !icon.drawn
        text: icon.drawn ? "" : icon.glyph
        font.family: Theme.iconFont
        // In points: font.pixelSize is a whole number of logical pixels, which
        // would round a size like 64/3 (32 device pixels at 150%) down.
        font.pointSize: icon.size * 72 / 96
        color: icon.color
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    Loader {
        anchors.centerIn: parent
        active: icon.drawn !== null
        sourceComponent: Canvas {
            id: canvas

            readonly property real dpr: Screen.devicePixelRatio

            // The image has exactly the device pixels it covers, so it is
            // sampled unsmoothed: smoothly it would blur wherever it sits on a
            // half pixel. Nudged off the exact half pixel, where the nearest
            // sample is a tie that can take one column twice.
            x: 1 / 64
            y: 1 / 64
            width: icon.size
            height: icon.size
            antialiasing: true
            smooth: false
            onDprChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                if (!icon.drawn)
                    return // switched to a font glyph; the Loader is letting go of this
                // The master made for this many device pixels, else the
                // biggest one that scales to it by a whole factor (32 = 2 × 16),
                // else the nearest.
                const pixels = Math.round(width * dpr)
                const sizes = Object.keys(icon.drawn).map(Number)
                const master = sizes.filter(s => pixels % s === 0).pop()
                               ?? sizes.reduce((a, b) => Math.abs(b - pixels) < Math.abs(a - pixels) ? b : a)
                const shapes = icon.drawn[master]
                ctx.scale(width / master, height / master)
                ctx.strokeStyle = icon.color
                ctx.fillStyle = icon.color
                ctx.lineWidth = shapes.width
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                ctx.path = shapes.stroke
                ctx.stroke()
                ctx.path = shapes.fill
                ctx.fill()
            }

            Connections {
                target: icon
                function onColorChanged() { canvas.requestPaint() }
                function onDrawnChanged() { canvas.requestPaint() }
            }
        }
    }
}
