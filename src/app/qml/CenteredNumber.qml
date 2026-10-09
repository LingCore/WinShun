import QtQuick

// A number centred in its parent by the digits themselves: they stand on the
// baseline and reach the top of their outline. Centring the line instead
// leaves them low (the outline's bottom also comes out a little below the
// baseline, so it is not used). Drawn as outlines, exactly where they are
// put: in the launcher's window, native glyphs land a pixel or two lower
// than the font says.
Text {
    id: number

    x: (parent.width - ink.tightBoundingRect.width) / 2 - ink.tightBoundingRect.x
    y: (parent.height - ink.tightBoundingRect.y) / 2 - baselineOffset
    renderType: Text.CurveRendering
    textFormat: Text.PlainText

    TextMetrics {
        id: ink
        font: number.font
        text: number.text
    }
}
