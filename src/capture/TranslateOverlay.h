#pragma once

#include "SmartRedact.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdlib>

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
// How thick the words' strokes are for their size: the middle one of the runs
// of ink along the rows of the box (their smoothed ends counted for what they
// cover), over the line's height. Measured on Segoe UI and Microsoft YaHei UI
// from 14 to 48 px: regular 0.053 to 0.064, bold 0.100 to 0.126; kBoldStroke
// is nearer the bold, for a font heavier than those to stay regular (CJK in a
// font that has none, made bold by the fallback, measures 0.08 and stays
// regular too). 0 when too little stands out to tell.
double strokeWeight(const QImage& image, const QRect& box, const BlockColors& colors, double lineHeight);
inline constexpr double kBoldStroke = 0.085;
// Light words on a dark ground bloom: a browser's regular white on a blue
// button or a dark bar measured 0.077 to 0.080 on a real screen (its dark
// regular 0.061, its bold heading 0.146), so they are bold from here.
inline constexpr double kBoldStrokeLight = 0.105;
// The words in the box are bold, by strokeWeight and the mark for their kind.
bool boldWords(const QImage& image, const QRect& box, const BlockColors& colors, double lineHeight);
// The ground free of anything else left and right of the box, on its middle
// rows, in px: up to what is not the paper's colour (a button's edge, an
// icon). -1 for a side where the ground runs to the picture's edge.
struct Margins {
    int left = -1;
    int right = -1;
    // The words sit in the middle of something: both sides end, about as far away.
    bool centred() const { return left >= 4 && right >= 4 && std::abs(left - right) <= std::max(3, std::max(left, right) / 8); }
};
Margins blockMargins(const QImage& image, const QRect& box, const QColor& paper);

// `image` with each block's words covered and `translations[i]` set in their
// place in `font`'s family: at about the words' size, smaller down to three
// fifths of it (9 px at least) when it takes that to fit the box; one line
// may run on to the right over empty ground, short of the next block. Bold
// where the words were (strokeWeight), and one line that sat in the middle of
// its button or cell (blockMargins) in the middle again, with room both ways. A block
// with no translation, or one that says the same as the words, is left alone.
// Fewer or more translations than blocks: a null picture.
QImage translatedPicture(const QImage& image, const QList<TextBlock>& blocks, const QStringList& translations, const QFont& font);

} // namespace ws::capture
