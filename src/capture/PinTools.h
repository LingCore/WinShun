#pragma once

#include "SmartRedact.h"

#include <QImage>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>

#include <optional>
#include <vector>

// What the pins' extra actions (stage 7) need that asks nothing of a window:
// turning and flipping the picture, the small forms a pin collapses to and
// where they go, dragging a corner, and the text picked out of the lines the
// reader found. Physical pixels, the virtual desktop's, as Geometry.h.
namespace ws::capture {

// ---- rotating and flipping -----------------------------------------------------

// The picture turned a quarter, exactly (no filtering): four turns give the
// same pixels back.
QImage turnedImage(const QImage& image, bool clockwise);
// The picture mirrored left to right, or top to bottom.
QImage flippedImage(const QImage& image, bool horizontal);
// Where a picture's top left goes when its size on the screen changes from
// `before` to `after` about its middle. The half-differences are cut toward
// zero, so turning back (after -> before) undoes it to the pixel.
QPoint recenteredOrigin(const QRect& before, QSize after);

// ---- collapsing ------------------------------------------------------------------

// A pin is shown in full, as a small tile (its thumbnail) or as a tab on the
// top edge of its monitor's work area. Only the PNG bytes of the picture are
// kept while it is not in full.
enum class PinForm { Full, Tile, Edge };

// What a double click on a pin in full does ([Capture] PinDoubleClick):
// back to 100% (the default), close it, collapse it to a tile, or nothing.
enum class PinDoubleClick { Reset, Close, Collapse, None };
PinDoubleClick pinDoubleClickFromName(const QString& name); // unknown: Reset

// The thumbnail's size: the picture fitted in a box of `box` logical pixels
// at `dpr`, never larger than the picture itself, at least 1x1.
QSize tileThumbSize(QSize image, double dpr, int box = 96);
// The tab on the top edge: 112 x 28 logical pixels at `dpr`.
QSize edgeTabSize(double dpr);
// The tile with its top left as near `wanted` as the work area allows.
QRect tileRect(QPoint wanted, QSize size, const QRect& work);
// The tab on the top edge of `work`, its middle as near `centerX` as it can
// be, moved along the edge past the tabs already `taken` (a gap of `gap`
// between). All taken: overlapping the first choice.
QRect edgeTabRect(int centerX, QSize size, const QRect& work, const std::vector<QRect>& taken, int gap = 4);
// Where the full picture's top left goes when a tile is opened: where it was
// when collapsed, moved as far as the tile was dragged meanwhile.
QPoint expandedOrigin(QPoint savedOrigin, QPoint tileAtCollapse, QPoint tileNow);

// ---- dragging a corner -----------------------------------------------------------

enum class Corner { None, TopLeft, TopRight, BottomLeft, BottomRight };
// The corner of `picture` within `reach` pixels of `p` (a square grip around
// the corner point; no larger than a third of the picture, or a small pin
// would be all corner).
Corner cornerAt(const QRect& picture, QPoint p, int reach);
struct Resized {
    QPoint origin; // the picture's new top left
    double zoom = 1.0;
};
// The picture (`start` at the press, of `image` pixels) with `corner` dragged
// to `cursor`: the opposite corner stays, the aspect ratio stays, the zoom
// is kept in minZoom..maxZoom.
Resized resizeFromCorner(Corner corner, const QRect& start, QSize image, QPoint cursor, double minZoom, double maxZoom);

// ---- the close button shown while the cursor is on it ------------------------------

// The corner on the side the cursor came in from: the quarter of `picture` `p` is in.
Corner entryCorner(const QRect& picture, QPoint p);
// The round close button in `corner` of `picture` (its size and inset follow
// `dpr`), a little in from the corner point so the very corner still resizes;
// empty for None or a picture too small to carry it.
QRect closeButtonRect(const QRect& picture, Corner corner, double dpr);

// ---- picking text --------------------------------------------------------------

// One line (or the part of one) the selection took.
struct PickedLine {
    QRectF box; // image px
    QString text;
};
// Every recognized line of `lines` (image px), top to bottom and left to right on one row.
QList<PickedLine> allPickedLines(const QList<RedactLine>& lines);
// Whether `point` (image px) sits on any recognized line (within `padding` image px).
bool hasTextAt(const QList<RedactLine>& lines, QPointF point, double padding = 2.0);
// The recognized line under `point` (image px, within `padding`), if any.
std::optional<PickedLine> pickedLineAt(const QList<RedactLine>& lines, QPointF point, double padding = 2.0);
// The lines of `lines` (image px) that `selection` touches, each cut to the
// characters the selection covers when it covers only part of the line along
// its length. Top to bottom, left to right on one row.
QList<PickedLine> pickedLines(const QList<RedactLine>& lines, const QRectF& selection);
// Flowing selection from `from` to `to` (image px), as in a document: partial
// start and end lines, full lines in between (keeping to the column spanned by
// the drag so a separate sidebar column is not swept in).
QList<PickedLine> pickedLinesFlow(const QList<RedactLine>& lines, QPointF from, QPointF to);
// Their text: pieces on one row joined by a space, rows by a line break.
QString pickedText(const QList<PickedLine>& picked);

} // namespace ws::capture
