pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import WinShun

// An icon named by its Segoe Fluent / MDL2 code point, e.g. "" (search),
// drawn with Phosphor (see phosphor::forSegoe); or the name of one drawn here
// (see `drawn`).
Item {
    id: icon

    property string glyph
    // A second font glyph drawn over the first, as MDL2 fills pair with their
    // outlines: PinnedFill is only the head, the needle is in Pin. Not for a
    // Phosphor icon, whole in one glyph.
    property string overlay
    property real size: 16
    property color color: Theme.subtext
    // A window's caption button: Windows' own glyph, as its own windows have.
    property bool caption: false

    // The custom copy-path icon shares Phosphor Regular's 256-unit grid,
    // 16-unit stroke and optical scale. SVG source: resources/icons/copy-path.svg.
    readonly property bool drawn: glyph === "copyPath"

    // {text, family}, or undefined: the system icon font draws it. The custom
    // path uses terminal-window's matching frame to resolve the same font em.
    readonly property var phosphor: caption ? undefined : SystemTheme.phosphorIcon(drawn ? "\uE756" : glyph)
    // Not in the system icon font (an older Windows 10; seen 2026-10-29):
    // nothing, never the character some other font has there (see
    // SystemTheme.hasGlyph).
    readonly property string shown: drawn ? glyph
                                  : phosphor ? phosphor.text
                                  : SystemTheme.hasGlyph(glyph) ? glyph : ""
    readonly property bool missing: shown.length === 0 && glyph.length > 0 // keeps its room

    // The icon fonts' line box is exactly their em square, so drawn icons
    // line up with the glyphs. A Phosphor icon, drawn larger, stays in its box.
    implicitWidth: drawn || phosphor || missing ? size : label.implicitWidth
    implicitHeight: drawn || phosphor || missing ? size : label.implicitHeight

    // A Phosphor icon: drawn by PhosphorGlyph, the em's centre on a pixel
    // centre or edge, the same across and down. As text it came out warped:
    // native text puts the baseline on a whole pixel, 15/16 em below the
    // em's top, so an x or a chevron sat a fraction of a pixel lower than
    // across, its axis between pixels.
    PhosphorGlyph {
        readonly property bool used: !icon.drawn && !!icon.phosphor
        visible: used
        text: used ? icon.phosphor.text : ""
        family: used ? icon.phosphor.family : ""
        color: icon.color
        em: label.fontInfo.pixelSize // the font's own rounding of the size, as before
        box: Qt.size(icon.width, icon.height)
    }

    // The system icon font's glyphs, and the font metrics for the others
    // (its text stays: fontInfo is resolved only by laying some out).
    Text {
        id: label
        anchors.centerIn: parent
        visible: !icon.drawn && !icon.phosphor
        text: icon.drawn ? icon.phosphor.text : icon.shown
        font.family: icon.phosphor ? icon.phosphor.family : Theme.iconFont
        // In points: font.pixelSize is a whole number of logical pixels, which
        // would round a size like 64/3 (32 device pixels at 150%) down.
        font.pointSize: icon.size * (icon.phosphor ? SystemTheme.iconScale : 1) * 72 / 96
        color: icon.color
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    // Made only where wanted (a settings window has over a thousand icons,
    // nearly all one Phosphor glyph): the overlay, or the drawn icon.
    readonly property bool overlaid: !drawn && !phosphor && overlay.length > 0 && SystemTheme.hasGlyph(overlay)
    Loader {
        anchors.centerIn: parent
        active: icon.drawn || icon.overlaid
        sourceComponent: icon.drawn ? drawnIcon : overlayGlyph
    }

    Component {
        id: overlayGlyph
        Text {
            text: icon.overlay
            font: label.font
            color: icon.color
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    Component {
        id: drawnIcon
        Shape {
            width: 256
            height: 256
            // Match the font's resolved em, including Windows' font DPI.
            scale: label.fontInfo.pixelSize / 256
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeWidth: 16
                strokeColor: icon.color
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg {
                    path: "M40 48H216A8 8 0 0 1 224 56V200A8 8 0 0 1 216 208H40A8 8 0 0 1 32 200V56A8 8 0 0 1 40 48Z"
                          + "M72 96L96 160M112 96L136 160"
                }
            }
            ShapePath {
                strokeColor: "transparent"
                fillColor: icon.color
                PathSvg {
                    path: "M176 160A8 8 0 1 1 160 160A8 8 0 1 1 176 160Z"
                          + "M208 160A8 8 0 1 1 192 160A8 8 0 1 1 208 160Z"
                }
            }
        }
    }
}
