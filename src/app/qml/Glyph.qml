pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// An icon by its Phosphor name (iconName), or by its legacy Segoe Fluent /
// MDL2 code point (glyph), drawn with Phosphor (see phosphor::resolve); or
// the name of one drawn here (see `drawn`).
Item {
    id: icon

    property string glyph
    // A Phosphor icon by name, e.g. "heart"; filled picks the Fill weight.
    // Prefer this for new icons: the name is stable, a Segoe code is not.
    property string iconName
    property bool filled: false
    // A second font glyph drawn over the first, as MDL2 fills pair with their
    // outlines: PinnedFill is only the head, the needle is in Pin. Not for a
    // Phosphor icon, whole in one glyph.
    property string overlay
    // The icon's container: an even square (14, 16, 18, 20, 24), and the item's own size. A Phosphor
    // icon's em square is exactly this box, its ink inside it with the margin Phosphor's grid gives
    // every icon (about 82% of the box drawn on), so what stands beside an icon keeps its distance from
    // the box and never looks at the ink. In logical pixels, whatever the font DPI.
    property real size: 16
    property color color: Theme.subtext
    // A pressed key's icon, a little smaller (KeySurface's contentScale): the
    // box stays, so nothing beside it moves; a Phosphor icon is drawn afresh
    // at the smaller size (PhosphorGlyph.inkScale), never a scaled picture.
    property real inkScale: 1
    // A window's caption button: Windows' own glyph, as its own windows have.
    property bool caption: false

    // The custom copy-path icon shares Phosphor Regular's 256-unit grid,
    // 16-unit stroke and optical scale, and is outlined and drawn by
    // PhosphorGlyph as a font's icon is (its kDrawnFamily; a Shape with the
    // curve renderer drew nothing in the settings window). SVG source:
    // resources/icons/copy-path.svg.
    readonly property bool drawn: glyph === "copyPath"

    // {text, family}, or undefined: the system icon font draws it.
    readonly property string spec: iconName.length > 0
                                   ? (filled ? "ph-fill:" : "ph:") + iconName
                                   : glyph
    readonly property var phosphor: caption || drawn ? undefined : SystemTheme.phosphorIcon(spec)
    // Not in the system icon font (an older Windows 10; seen 2026-10-29):
    // nothing, never the character some other font has there (see
    // SystemTheme.hasGlyph). An unknown name stays empty too: a "ph:..." spec
    // must never reach the system font, as Phosphor and Segoe share the PUA.
    readonly property string shown: drawn ? glyph
                                  : phosphor ? phosphor.text
                                  : iconName.length > 0 ? ""
                                  : SystemTheme.hasGlyph(glyph) ? glyph : ""
    readonly property bool missing: shown.length === 0 && (glyph.length > 0 || iconName.length > 0) // keeps its room

    // The container, whatever draws in it (the system font's line box is its em square, smaller).
    implicitWidth: size
    implicitHeight: size

    // A Phosphor icon: drawn by PhosphorGlyph, the em's centre on a pixel
    // centre or edge, the same across and down. As text it came out warped:
    // native text puts the baseline on a whole pixel, 15/16 em below the
    // em's top, so an x or a chevron sat a fraction of a pixel lower than
    // across, its axis between pixels.
    PhosphorGlyph {
        id: phosphorItem
        readonly property bool used: icon.drawn || !!icon.phosphor
        visible: used
        text: icon.drawn ? icon.glyph : used ? icon.phosphor.text : ""
        family: icon.drawn ? "ws-drawn" : used ? icon.phosphor.family : ""
        color: icon.color
        em: icon.size
        inkScale: icon.inkScale
        box: Qt.size(icon.width, icon.height)
    }

    // The system icon font's glyphs: a caption button's, at its size as Windows draws it; any other
    // only where the Phosphor fonts did not load, and then smaller than its container, as a font that
    // draws on its whole em weighs the same as Phosphor in the box (SystemTheme.iconScale).
    Text {
        id: label
        anchors.centerIn: parent
        visible: !icon.drawn && !icon.phosphor
        text: icon.drawn || icon.phosphor ? "" : icon.shown
        // In points: font.pixelSize is a whole number of logical pixels, which
        // would round a size like 64/3 (32 device pixels at 150%) down.
        // Never merged with other fonts (SystemTheme.glyphFont).
        font: SystemTheme.glyphFont(Theme.iconFont, icon.size / (icon.caption ? 1 : SystemTheme.iconScale) * 72 / 96)
        scale: icon.inkScale // the system font's (an older Windows 10)
        color: icon.color
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    // Made only where wanted (a settings window has over a thousand icons,
    // nearly all one Phosphor glyph): the overlay.
    readonly property bool overlaid: !drawn && !phosphor && overlay.length > 0 && SystemTheme.hasGlyph(overlay)
    Loader {
        anchors.centerIn: parent
        active: icon.overlaid
        sourceComponent: overlayGlyph
    }

    Component {
        id: overlayGlyph
        Text {
            text: icon.overlay
            font: label.font
            scale: icon.inkScale
            color: icon.color
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
