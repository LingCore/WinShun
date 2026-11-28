import QtQuick
import WinShun

// An icon by its Phosphor name (iconName), or by its legacy Segoe Fluent /
// MDL2 code point (glyph), drawn with Phosphor (see phosphor::resolve); or
// the name of one drawn by PhosphorGlyph itself ("copyPath": it shares
// Phosphor Regular's 256-unit grid, 16-unit stroke and optical scale; SVG
// source: resources/icons/copy-path.svg).
//
// The item is the icon: PhosphorGlyph works out which one and draws it, with
// nothing made beside it. `size` is its container (an even square: 14, 16,
// 18, 20, 24), `color`, `filled`, `inkScale`, `caption` and `overlay` are
// PhosphorGlyph's too.
PhosphorGlyph {
    id: icon

    color: Theme.subtext

    // The system icon font's glyphs (fontDrawn): a caption button's, at its
    // size as Windows draws it; any other only where the Phosphor fonts did
    // not load. A Text, made for the few icons that need one. By the file's
    // name, not an inline Component: that would be one more object in every
    // icon, for a part nearly none of them has.
    property Item fontPart: null
    onFontDrawnChanged: {
        if (!fontDrawn || fontPart)
            return
        const part = Qt.createComponent("GlyphFont.qml")
        fontPart = part.createObject(icon, { icon: icon }) as Item
        if (!fontPart)
            console.warn("Glyph: the system font's part could not be made:", part.errorString())
    }
}
