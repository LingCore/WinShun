import QtQuick
import QtQuick.Shapes

// typeIcon, 淡彩: a rounded tile in the type's quiet tint (radius 9 at 32)
// with a line symbol in the same hue's deep tone. One symbol set on a 16 px
// grid, drawn at 18/32 of the tile and centred; the stroke stays 1.6 px
// whatever the size (1.25 below 24). kind: folder/doc/xls/ppt/pdf/image/md/
// zip/code/audio/video/ebook/text/other (and the aliases in Look.kindAliases).
// Apps and exe files keep their system icons (the host does not ask here).
Item {
    id: icon

    property QtObject look
    property string kind: "other"
    property real size: 32
    property real dpr: 1

    readonly property string family: look.kindOf(kind)
    readonly property color ink: look.kindInk(kind)
    readonly property color tint: look.kindTint(kind)
    readonly property real stroke: size >= 24 ? 1.6 : 1.25

    implicitWidth: size
    implicitHeight: size

    // Paths on the 16 px grid (rects and circles spelled out as arcs).
    function rect(x, y, w, h, r) {
        return "M" + (x + r) + " " + y + "H" + (x + w - r) + "A" + r + " " + r + " 0 0 1 " + (x + w) + " " + (y + r)
             + "V" + (y + h - r) + "A" + r + " " + r + " 0 0 1 " + (x + w - r) + " " + (y + h)
             + "H" + (x + r) + "A" + r + " " + r + " 0 0 1 " + x + " " + (y + h - r)
             + "V" + (y + r) + "A" + r + " " + r + " 0 0 1 " + (x + r) + " " + y + "Z"
    }
    function circle(cx, cy, r) {
        return "M" + (cx - r) + " " + cy + "A" + r + " " + r + " 0 1 0 " + (cx + r) + " " + cy
             + "A" + r + " " + r + " 0 1 0 " + (cx - r) + " " + cy + "Z"
    }
    readonly property var symbols: ({
        slides: rect(1.5, 2, 13, 9, 1.5) + "M8 11v3M5.5 14h5M4.5 8.5l2.3-2.3 2 1.4 2.7-2.8",
        sheet: rect(1.75, 1.75, 12.5, 12.5, 2) + "M1.75 6h12.5M1.75 10h12.5M6 1.75v12.5",
        doc: "M2.5 3h11M2.5 6.3h11M2.5 9.6h11M2.5 12.9h7",
        pdf: "M2.5 3h11M2.5 6.3h11M2.5 9.6h4.5" + circle(11, 11.6, 2.5),
        md: rect(1, 3, 14, 10, 2) + "M4 10.2V5.8l2 2.2 2-2.2v4.4M11.5 5.8v4.4M9.9 8.7l1.6 1.6 1.6-1.6",
        image: rect(1.75, 2.25, 12.5, 11.5, 2) + circle(5.5, 6, 1.25) + "M2 12l3.8-3.4 2.6 2.2 1.8-1.5 3.8 3.2",
        folder: "M1.5 4.25A1.25 1.25 0 0 1 2.75 3h3.4l1.6 1.6h5.5a1.25 1.25 0 0 1 1.25 1.25v6.9A1.25 1.25 0 0 1 13.25 14H2.75A1.25 1.25 0 0 1 1.5 12.75zM1.5 6.8h13",
        archive: rect(2.75, 1.75, 10.5, 12.5, 2) + "M8 1.75v1.5M8 4.75v1.5M8 7.75v1" + rect(6.5, 9.25, 3, 2.75, 0.75),
        code: "M5.25 4.5L1.75 8l3.5 3.5M10.75 4.5l3.5 3.5-3.5 3.5M9.25 2.75l-2.5 10.5",
        audio: "M6 11.75V3.5l7.25-1.5v8.25" + circle(4.25, 11.75, 1.75) + circle(11.5, 10.25, 1.75),
        video: rect(1.5, 3.5, 9.5, 9, 2) + "M11 6.6l3.5-2v6.8L11 9.4",
        ebook: "M8 4.25C6.6 3.15 4.6 2.6 1.75 2.75v9.5c2.85-.15 4.85.35 6.25 1.5 1.4-1.15 3.4-1.65 6.25-1.5v-9.5C11.4 2.6 9.4 3.15 8 4.25zM8 4.25v9.5",
        app: rect(2, 2, 5, 5, 1.25) + rect(9, 2, 5, 5, 1.25) + rect(2, 9, 5, 5, 1.25) + rect(9, 9, 5, 5, 1.25),
        text: "M3.75 1.75h5.5l3 3v9.5h-8.5zM9.25 1.75v3h3M6 8.25h4M6 11h4",
        file: "M3.75 1.75h5.5l3 3v9.5h-8.5zM9.25 1.75v3h3"
    })

    // Scale and move an SVG path (absolute and relative commands, arcs
    // included), so the symbol is drawn at its size with an unscaled stroke.
    function place(d, s, ox, oy) {
        const tokens = d.match(/[A-Za-z]|-?(?:\d*\.\d+|\d+)(?:e-?\d+)?/g)
        let out = "", cmd = "", n = 0
        for (let i = 0; i < tokens.length; i++) {
            const t = tokens[i]
            if (/[A-Za-z]/.test(t)) { cmd = t; n = 0; out += t; continue }
            const v = parseFloat(t), rel = cmd === cmd.toLowerCase(), C = cmd.toUpperCase()
            let w = v * s
            if (C === "A") {
                const k = n % 7
                if (k === 2 || k === 3 || k === 4) w = v
                else if (!rel && k === 5) w = v * s + ox
                else if (!rel && k === 6) w = v * s + oy
            } else if (C === "H") {
                if (!rel) w = v * s + ox
            } else if (C === "V") {
                if (!rel) w = v * s + oy
            } else if (!rel) {
                w = v * s + (n % 2 === 0 ? ox : oy)
            }
            out += (out.length && !/[A-Za-z]$/.test(out) ? " " : "") + (Math.round(w * 1000) / 1000)
            n++
        }
        return out
    }

    Rectangle {
        anchors.fill: parent
        radius: icon.look.rounded ? icon.size * 9 / 32 : 0
        color: icon.tint
    }
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: "transparent"
            strokeColor: icon.ink
            strokeWidth: icon.stroke
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.place(icon.symbols[icon.family] || icon.symbols.file, icon.size * 18 / 32 / 16, icon.size * 7 / 32, icon.size * 7 / 32) }
        }
    }
}
