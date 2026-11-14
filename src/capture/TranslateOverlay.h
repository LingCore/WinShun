#pragma once

#include "SmartRedact.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

// Screenshot translation in place (GitHub #191): from the lines the reader
// found to the blocks a translator gets (a paragraph the screen wrapped is one
// block, a label or a list item one each), and from their translations to the
// picture with those drawn where the words were. Plain values in, a picture
// out: the tests run it without the reader or a service.
//
// Made for what a screenshot shows: level text on a flat ground (windows, web
// pages, documents). The words are covered with the ground's colour, taken
// from just outside their box, and the translation is set in the words' own
// colour at about their size. No repainting of what was behind the words (a
// photo under them shows the box), and a line that is not level is left as
// it was.
namespace ws::capture {

struct TextBlock {
    QRect box; // image px: the lines' boxes together
    QString text; // the lines as one, wrapped words together again (Geometry: ocrGlue)
    double lineHeight = 0; // of one line's box, the mean
    int lines = 0;
    bool level = true; // false: a turned line, on its own, not drawn over
    friend bool operator==(const TextBlock&, const TextBlock&) = default;
};

// The lines as blocks, top to bottom. A line goes on in the next one when the
// two are alike in height, the next starts at the block's left edge right
// under it, the line is running text (eight of its heights wide at least)
// that reached the block's right edge, and the words do not say otherwise
// (ocrGoesOn: a sentence ended, a list item begins). Columns side by side are
// kept apart by the left edge.
QList<TextBlock> ocrBlocks(const QList<RedactLine>& lines);

// The ground under a block's words and the colour of the words themselves.
struct BlockColors {
    QColor paper;
    QColor ink;
};
BlockColors blockColors(const QImage& image, const QRect& box);

// `image` with each block's words covered and `translations[i]` set in their
// place in `font`'s family: at about the words' size, smaller down to three
// fifths of it (9 px at least) when it takes that to fit the box; one line
// may run on to the right over empty ground, short of the next block. A block
// with no translation, or one that says the same as the words, is left alone.
// Fewer or more translations than blocks: a null picture.
QImage translatedPicture(const QImage& image, const QList<TextBlock>& blocks, const QStringList& translations, const QFont& font);

} // namespace ws::capture
