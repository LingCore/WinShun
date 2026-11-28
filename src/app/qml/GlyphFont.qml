pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// What the system icon font draws of a Glyph (made by it, over it, only where
// PhosphorGlyph.fontDrawn): a window's caption button, at its size as Windows
// draws it; any other only where the Phosphor fonts did not load, and then
// smaller than its container, as a font that draws on its whole em weighs the
// same as Phosphor in the box (SystemTheme.iconScale).
Item {
    id: part

    required property PhosphorGlyph icon
    // Not in the system icon font (an older Windows 10; seen 2026-10-29):
    // nothing, never the character some other font has there (see
    // SystemTheme.hasGlyph); the icon keeps its room.
    readonly property string shown: icon.fontDrawn && SystemTheme.hasGlyph(icon.glyph) ? icon.glyph : ""

    anchors.fill: parent
    visible: icon.fontDrawn

    Text {
        id: label
        anchors.centerIn: parent
        text: part.shown
        // In points: font.pixelSize is a whole number of logical pixels, which
        // would round a size like 64/3 (32 device pixels at 150%) down.
        // Never merged with other fonts (SystemTheme.glyphFont).
        font: SystemTheme.glyphFont(Theme.iconFont, part.icon.size / (part.icon.caption ? 1 : SystemTheme.iconScale) * 72 / 96)
        scale: part.icon.inkScale
        color: part.icon.color
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    // A second glyph over the first (PhosphorGlyph.overlay), where there is one.
    Loader {
        anchors.centerIn: parent
        active: part.icon.fontDrawn && part.icon.overlay.length > 0 && SystemTheme.hasGlyph(part.icon.overlay)
        sourceComponent: Text {
            text: part.icon.overlay
            font: label.font
            scale: part.icon.inkScale
            color: part.icon.color
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
