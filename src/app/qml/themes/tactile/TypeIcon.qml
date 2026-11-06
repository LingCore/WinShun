pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes

// A file type icon as a thing with thickness: a sheet of paper with a
// folded corner and a second sheet's edge showing under it, the content
// printed straight on it; a folder is an amber folder with a front flap
// and thickness. A soft contact shadow under both. Drawn on the
// prototype's 34-unit grid and scaled to `size`, centred in the box.
// Apps and .exe keep their system icons (the host decides).
Item {
    id: icon

    property string kind: ""
    property real size: 32 // 0 (Hook's default): the box's smaller side
    readonly property real side: size > 0 ? size : Math.max(1, Math.min(width, height))
    property real dpr: 1

    property QtObject look // injected by Look.qml
    implicitWidth: size
    implicitHeight: size

    // kind names the app may use -> one print
    readonly property string printKind: {
        const k = kind.toLowerCase()
        const map = {
            folder: "folder", directory: "folder", dir: "folder",
            document: "document", doc: "document", docx: "document", word: "document", rtf: "document", wps: "document",
            spreadsheet: "spreadsheet", sheet: "spreadsheet", xls: "spreadsheet", xlsx: "spreadsheet", csv: "spreadsheet", excel: "spreadsheet", et: "spreadsheet",
            presentation: "presentation", slides: "presentation", ppt: "presentation", pptx: "presentation", powerpoint: "presentation", dps: "presentation",
            pdf: "pdf",
            markdown: "markdown", md: "markdown",
            image: "image", picture: "image", photo: "image", png: "image", jpg: "image", jpeg: "image", gif: "image", webp: "image", svg: "image",
            archive: "archive", zip: "archive", compressed: "archive", rar: "archive", "7z": "archive",
            code: "code", source: "code",
            audio: "audio", music: "audio", sound: "audio",
            video: "video", movie: "video",
            ebook: "ebook", book: "ebook", epub: "ebook",
            program: "program", app: "program", exe: "program",
            text: "text", txt: "text", log: "text"
        }
        return map[k] !== undefined ? map[k] : "file"
    }
    readonly property bool isFolder: printKind === "folder"

    // ---- small path helpers (34-unit grid) ----
    function rr(x: real, y: real, w: real, h: real, r: real): string {
        return "M" + (x + r) + " " + y + "h" + (w - 2 * r) + "a" + r + " " + r + " 0 0 1 " + r + " " + r
             + "v" + (h - 2 * r) + "a" + r + " " + r + " 0 0 1 " + (-r) + " " + r
             + "h" + (-(w - 2 * r)) + "a" + r + " " + r + " 0 0 1 " + (-r) + " " + (-r)
             + "v" + (-(h - 2 * r)) + "a" + r + " " + r + " 0 0 1 " + r + " " + (-r) + "z"
    }
    function circle(cx: real, cy: real, r: real): string {
        return "M" + (cx - r) + " " + cy + "a" + r + " " + r + " 0 1 0 " + (2 * r) + " 0a" + r + " " + r + " 0 1 0 " + (-2 * r) + " 0z"
    }
    function alpha(c: string, a: real): color { const q = Qt.color(c); return Qt.rgba(q.r, q.g, q.b, a) }

    // What is printed on the sheet: { d, fill, stroke, w } (prototype prints,
    // plus the same hand for the kinds it did not draw).
    readonly property var layers: {
        switch (printKind) {
        case "presentation": return [
            { d: rr(10.5, 13, 12, 9, 1), fill: "#F7D9CC" },
            { d: "M12.6 20.5v-2.6M15.4 20.5v-4.4M18.2 20.5v-6", stroke: "#D9531F", w: 2 },
            { d: "M10.5 25h8", stroke: alpha("#D9531F", 0.5), w: 1.3 }]
        case "spreadsheet": return [
            { d: rr(10.3, 12.5, 12.4, 11.5, 1.2), fill: "#D6EEDD", stroke: "#1E8A4C", w: 1.2 },
            { d: "M10.3 16.3h12.4M10.3 20.1h12.4M14.6 12.5v11.5", stroke: "#1E8A4C", w: 1.1, flat: true }]
        case "document": return [
            { d: "M10.5 13.5h11.5M10.5 16.8h11.5M10.5 20.1h11.5M10.5 23.4h7", stroke: "#2F6FDE", w: 1.6 }]
        case "pdf": return [
            { d: "M10.8 11.5h5.2v9l-2.6-2-2.6 2z", fill: "#D93A3A" },
            { d: "M18 14h4.2M18 17.2h4.2M10.8 23.6h11.4", stroke: alpha("#D93A3A", 0.75), w: 1.5 }]
        case "markdown": return [
            { d: "M10.4 21.8v-7.2l3 3.6 3-3.6v7.2", stroke: "#3E424D", w: 1.7 },
            { d: "M20.4 14.6v7M18.4 19.6l2 2.2 2-2.2", stroke: "#3E424D", w: 1.6 }]
        case "image": return [
            { d: rr(9.6, 11.6, 13.8, 12.8, 1), fill: "#7CC6E8" },
            { d: circle(19.6, 15, 1.7), fill: "#FFE07A" },
            { d: "M9.6 21.5l4.2-4.2 3.4 3.2 2.2-1.8 4 3.4v2.3H9.6z", fill: "#2E9C6A" }]
        case "text": return [
            { d: "M10.5 13.5h11.5M10.5 16.8h11.5M10.5 20.1h8.5", stroke: "#8C8478", w: 1.6 }]
        case "archive": return [
            { d: rr(14.1, 11.4, 2.4, 1.5, 0.3) + rr(16.5, 13.2, 2.4, 1.5, 0.3) + rr(14.1, 15, 2.4, 1.5, 0.3) + rr(16.5, 16.8, 2.4, 1.5, 0.3), fill: "#A97A25" },
            { d: rr(14.4, 19, 4.2, 5.6, 1.2), fill: "#F1DEB8", stroke: "#83601C", w: 1.1 },
            { d: "M16.5 21v1.8", stroke: "#83601C", w: 1.1 }]
        case "code": return [
            { d: "M13.6 15.2l-3.2 3.3 3.2 3.3M19.4 15.2l3.2 3.3-3.2 3.3", stroke: "#BC4CAB", w: 1.7 },
            { d: "M17.7 14.2l-2.4 8.6", stroke: alpha("#BC4CAB", 0.7), w: 1.5 }]
        case "audio": return [
            { d: "M15.2 21.6V14.2l7-1.8v7.4", stroke: "#E0562F", w: 1.5 },
            { d: circle(13.3, 21.6, 2) + circle(20.3, 19.8, 2), fill: "#E0562F" }]
        case "video": return [
            { d: rr(9.8, 13.2, 13.4, 10.2, 1.6), fill: "#E7DEFB" },
            { d: "M14.9 15.8v5l4.3-2.5z", fill: "#7F54E0" }]
        case "ebook": return [
            { d: "M16.5 15.2c-1.9-1.1-4-1.5-6.2-1.2v8.8c2.2-.3 4.3.1 6.2 1.2c1.9-1.1 4-1.5 6.2-1.2v-8.8c-2.2-.3-4.3.1-6.2 1.2z", fill: "#D3F0EF", stroke: "#0F9393", w: 1.2 },
            { d: "M16.5 15.2v8.8", stroke: "#0F9393", w: 1.2 }]
        case "program": return [
            { d: rr(11, 13, 4.8, 4.8, 1.2) + rr(17.2, 13, 4.8, 4.8, 1.2) + rr(11, 19.2, 4.8, 4.8, 1.2), fill: "#5558E6" },
            { d: rr(17.2, 19.2, 4.8, 4.8, 1.2), fill: alpha("#5558E6", 0.45) }]
        default: return []
        }
    }

    // contact shadow: a soft ellipse (radial gradient, squashed)
    component Drop: Item {
        id: drop
        property real cx
        property real cy
        property real rx
        property real ry
        readonly property real r: rx + 2
        x: cx - r
        y: cy - r
        width: 2 * r
        height: 2 * r
        transform: Scale { origin.x: drop.r; origin.y: drop.r; yScale: (drop.ry + 2) / drop.r }
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: "transparent"
                fillGradient: RadialGradient {
                    centerX: drop.r; centerY: drop.r; centerRadius: drop.r
                    focalX: drop.r; focalY: drop.r
                    GradientStop { position: 0; color: (icon.look?.iconDrop ?? Qt.rgba(0, 0, 0, 0)) }
                    GradientStop { position: 0.55; color: icon.alpha((icon.look?.iconDrop ?? Qt.rgba(0, 0, 0, 0)).toString(), (icon.look?.iconDrop ?? Qt.rgba(0, 0, 0, 0)).a * 0.7) }
                    GradientStop { position: 1; color: "transparent" }
                }
                PathSvg { path: icon.circle(drop.r, drop.r, drop.r) }
            }
        }
    }

    Item {
        id: grid
        width: 34
        height: 34
        x: icon.look ? (icon.look?.snap((icon.width - icon.side) / 2, icon.dpr) ?? 0) : 0
        y: icon.look ? (icon.look?.snap((icon.height - icon.side) / 2, icon.dpr) ?? 0) : 0
        scale: icon.side / 34
        transformOrigin: Item.TopLeft

        // ---- folder (loaded only for folders) ----
        Loader {
            anchors.fill: parent
            active: icon.isFolder && icon.look !== null
            sourceComponent: Item {
                Drop { cx: 17; cy: 29.6; rx: 12.5; ry: 1.8 }
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath { strokeColor: "transparent"; fillColor: "#C98516"
                        PathSvg { path: "M4.5 9.5a1.6 1.6 0 0 1 1.6-1.6h6.6l2.4 2.4h12.8a1.6 1.6 0 0 1 1.6 1.6V27.5H4.5z" } }
                    ShapePath { strokeColor: "transparent"; fillColor: "#FFFDF8"
                        PathSvg { path: icon.rr(7, 11, 20, 9, 1) } }
                    ShapePath { strokeColor: "transparent"; fillColor: "#B5740F"
                        PathSvg { path: "M3.9 14.6a1.6 1.6 0 0 1 1.6-1.6h23a1.6 1.6 0 0 1 1.6 1.6L29.5 27a1.6 1.6 0 0 1-1.6 1.5H6.1A1.6 1.6 0 0 1 4.5 27z" } }
                    ShapePath {
                        strokeColor: "transparent"
                        fillGradient: LinearGradient {
                            x1: 0; y1: 13; x2: 0; y2: 27.5
                            GradientStop { position: 0; color: "#FAC659" }
                            GradientStop { position: 1; color: "#EBA83A" }
                        }
                        PathSvg { path: "M3.9 14.6a1.6 1.6 0 0 1 1.6-1.6h23a1.6 1.6 0 0 1 1.6 1.6L29.6 26a1.6 1.6 0 0 1-1.6 1.5H6A1.6 1.6 0 0 1 4.4 26z" }
                    }
                    ShapePath { strokeColor: Qt.rgba(1, 1, 1, 0.7); strokeWidth: 1; fillColor: "transparent"; capStyle: ShapePath.RoundCap
                        PathSvg { path: "M5.6 13.7h22.8" } }
                }
            }
        }

        // ---- paper + its print ----
        Loader {
            anchors.fill: parent
            active: !icon.isFolder && icon.look !== null
            sourceComponent: Item {
                Drop { cx: 17.8; cy: 31.2; rx: 11; ry: 1.7 }
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath { strokeColor: "transparent"; fillColor: (icon.look?.paperSide ?? Qt.rgba(0, 0, 0, 0))
                        PathSvg { path: "M8.6 5.4h12.4l6.2 6.2v18.2a1.5 1.5 0 0 1-1.5 1.5H8.6a1.5 1.5 0 0 1-1.5-1.5V6.9a1.5 1.5 0 0 1 1.5-1.5z" } }
                    ShapePath {
                        strokeColor: (icon.look?.paperRim ?? Qt.rgba(0, 0, 0, 0))
                        strokeWidth: 0.8
                        fillGradient: LinearGradient {
                            x1: 0; y1: 2.8; x2: 0; y2: 29.1
                            GradientStop { position: 0; color: (icon.look?.paperTop ?? Qt.rgba(0, 0, 0, 0)) }
                            GradientStop { position: 1; color: (icon.look?.paperBottom ?? Qt.rgba(0, 0, 0, 0)) }
                        }
                        PathSvg { path: "M7.6 2.8h12.6l6.2 6.2v18.6a1.5 1.5 0 0 1-1.5 1.5H7.6a1.5 1.5 0 0 1-1.5-1.5V4.3a1.5 1.5 0 0 1 1.5-1.5z" }
                    }
                    ShapePath { strokeColor: (icon.look?.paperRim ?? Qt.rgba(0, 0, 0, 0)); strokeWidth: 0.8; fillColor: (icon.look?.paperFold ?? Qt.rgba(0, 0, 0, 0)); joinStyle: ShapePath.RoundJoin
                        PathSvg { path: "M20.2 3v4.6a1.4 1.4 0 0 0 1.4 1.4h4.6" } }
                }
                // the print: translate(16.2 17.6) scale(1.14) translate(-16.5 -18)
                Item {
                    x: 16.2 - 1.14 * 16.5
                    y: 17.6 - 1.14 * 18
                    width: 34
                    height: 34
                    scale: 1.14
                    transformOrigin: Item.TopLeft
                    Repeater {
                        model: icon.layers
                        Shape {
                            id: layer
                            required property var modelData
                            anchors.fill: parent
                            preferredRendererType: Shape.CurveRenderer
                            ShapePath {
                                fillColor: layer.modelData.fill !== undefined ? layer.modelData.fill : "transparent"
                                strokeColor: layer.modelData.stroke !== undefined ? layer.modelData.stroke : "transparent"
                                strokeWidth: layer.modelData.w !== undefined ? layer.modelData.w : 0
                                capStyle: layer.modelData.flat ? ShapePath.FlatCap : ShapePath.RoundCap
                                joinStyle: ShapePath.RoundJoin
                                PathSvg { path: layer.modelData.d }
                            }
                        }
                    }
                }
            }
        }
    }
}
