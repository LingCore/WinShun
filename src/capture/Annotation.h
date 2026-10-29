#pragma once

#include <QGlyphRun>
#include <QImage>
#include <QHash>
#include <QList>
#include <QPainterPath>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QRgb>
#include <QSize>
#include <QString>
#include <QStringView>

#include <optional>

class QFont;

// Annotations drawn over a screenshot (docs/design-capture-roadmap.md, 3.2):
// the document, its objects, and the pure functions that the commands, the
// compositor, the project format and the tests share. Everything is in image
// pixels, one image pixel being one pixel of the screenshot, so that a stroke
// lands on whole pixels at any Windows scale (3.4).
//
// How each kind keeps its points:
//   Rect, Ellipse, Redact  two pixel edges (whole numbers), the box's top left
//                          and bottom right corners, right and bottom
//                          excluded; either order
//   Arrow, Line            start and end, pixel centres (whole + 0.5)
//   Pen, Marker            a polyline of pixel centres
//   Text                   the top left of the text itself (whole numbers);
//                          its coloured box (Style::boxed) reaches further out
//   Number                 the centre of its disc, a pixel centre; its
//                          label (Item::text, may be empty) stands to the right
namespace ws::capture::annot {

enum class Kind : quint8 { Rect, Ellipse, Arrow, Line, Pen, Marker, Text, Number, Redact };
// Soft is the stroke's colour at 20 % opacity.
enum class Fill : quint8 { None, Soft, Solid };
// Smooth fills the area from the background around it, with no noise.
enum class RedactMode : quint8 { Mosaic, Blur, Solid, Smooth };
// Brush: the pixels along a stroke (Style::width wide) instead of a box.
enum class RedactShape : quint8 { Box, Brush };
enum class LineMode : quint8 { Straight, Polyline, Bezier };
enum class MarkerMode : quint8 { Freehand, Line, Rectangle };
QString lineModeName(LineMode mode);
std::optional<LineMode> lineModeFromName(QStringView name);
QString markerModeName(MarkerMode mode);
std::optional<MarkerMode> markerModeFromName(QStringView name);

constexpr int kKindCount = 9;

// How a series writes its numbers (#57): 1 2 3, A B C (after Z comes AA),
// a b c, I II III (to 3999), and the same digits inside a ring (a hollow
// disc, the colour on the digits) in place of the filled disc. A number the
// format cannot write (zero, a negative, past 3999 in Roman) shows as digits.
enum class NumberFormat : quint8 { Arabic, UpperLetters, LowerLetters, Roman, Circled };
constexpr int kNumberFormatCount = 5;

struct Style {
    QRgb color = qRgb(0xE5, 0x39, 0x35);
    int width = 6; // line width, image px
    Fill fill = Fill::None;
    int fontPx = 27; // Text and Number, image px
    bool bold = false;
    bool boxed = false; // Text: a coloured box behind white words
    int textStroke = 0; // Text: an outline round the glyphs, image px wide (0: none); not drawn when boxed
    QRgb textStrokeColor = 0; // Text: the outline's colour, opaque; 0: automatic (textStrokeColorOf)
    RedactMode redact = RedactMode::Mosaic;
    RedactShape shape = RedactShape::Box;
    int cell = 18; // mosaic cell, image px
    LineMode lineMode = LineMode::Straight;
    MarkerMode markerMode = MarkerMode::Freehand;
    bool operator==(const Style&) const = default;
};

struct Item {
    quint32 id = 0; // unique in the document while it exists
    Kind kind = Kind::Rect;
    quint32 seq = 0; // creation order; numbers count along it
    QList<QPointF> points; // image px, see above
    Style style;
    QString text; // Text; a Number's label (empty: none)
    quint32 series = 0; // Number: its group
    std::optional<int> pinned; // Number: a number set by hand; the next ones count on from it
    quint64 seed = 0; // Redact: the noise's seed, drawn at random when made and saved with it
    bool closed = false; // a polyline joins its last vertex to its first
    bool operator==(const Item&) const = default;
};

struct Series {
    quint32 id = 1;
    int start = 1;
    NumberFormat format = NumberFormat::Arabic;
    bool operator==(const Series&) const = default;
};

struct Document {
    static constexpr int kVersion = 2;
    QSize canvas; // the base picture's size, image px
    QRect crop; // what is exported; the selection in the overlay
    qreal dpr = 1.0; // the captured screen's scale: logical sizes on the panel and new styles go through it
    QList<Item> items; // bottom to top; the redactions are always the first ones
    QList<Series> series{Series{}}; // at least one
    quint32 currentSeries = 1;
    quint32 nextId = 1;
    quint32 nextSeq = 1;
    bool operator==(const Document&) const = default;
};

// The kinds by how they keep their points (see above).
bool isBox(Kind kind);
bool isLine(Kind kind);
bool isStroke(Kind kind);
// A Redact item drawn as a brush stroke.
bool isBrush(const Item& item);
// Pen, Marker and a brush redaction: items that keep a polyline of points.
bool isStrokeItem(const Item& item);
bool isBoxItem(const Item& item);
bool isLineItem(const Item& item);
// Bezier points: start, end, first control, second control. Polyline: vertices.
QPainterPath linePath(const Item& item);
QPainterPath arrowPath(const Item& item, qreal dpr);
QPointF snapAngle(QPointF from, QPointF to); // 15 degree steps, on pixel centres

// The names in doc.json and in the remembered styles.
QString kindName(Kind kind);
std::optional<Kind> kindFromName(QStringView name);
QString fillName(Fill fill);
std::optional<Fill> fillFromName(QStringView name);
QString redactModeName(RedactMode mode);
std::optional<RedactMode> redactModeFromName(QStringView name);
QString redactShapeName(RedactShape shape);
std::optional<RedactShape> redactShapeFromName(QStringView name);
QString numberFormatName(NumberFormat format);
std::optional<NumberFormat> numberFormatFromName(QStringView name);
// What `number` shows in `format`; see NumberFormat.
QString formatNumber(int number, NumberFormat format);
// "#RRGGBB" (as colorHex() in Geometry.h writes it) as an opaque colour.
std::optional<QRgb> parseColor(QStringView text);

// The redactions are one segment at the bottom of the list (its front):
// drawing them first keeps a later redaction from covering an earlier arrow
// or text (Snow Shot #2041). This is the index where that segment ends.
int redactionEnd(const Document& doc);
// The index an item of `kind` goes to when asked for `at` (-1: the top of
// its segment), kept inside its segment.
int insertionIndex(const Document& doc, Kind kind, int at);
// The index of the item `id`, -1 when there is none.
int indexOf(const Document& doc, quint32 id);
const Item* findItem(const Document& doc, quint32 id);

// The pixel under an image position, and that pixel's centre.
QPoint pixelAt(QPointF p);
QPointF pixelCentre(QPointF p);
// The box spanned by pressing on pixel `a` and dragging to pixel `b`: both
// pixels inside it, so its edges are [min, max + 1).
QRect pixelBox(QPoint a, QPoint b);
// A box item's pixels (Rect, Ellipse, Redact). A Brush redaction gives the
// pixels around all its points.
QRect itemBox(const Item& item);
// The boxes of all redactions, bottom to top. Their union is what no
// redaction reads from (3.10).
QList<QRect> redactionBoxes(const Document& doc);
// The pixels of a Brush redaction: Format_Grayscale8 of itemBox()'s size,
// 255 where a pixel's centre is within half the width of the stroke. Not
// antialiased: every pixel is covered or not. Null for any other item.
QImage brushCoverage(const Item& item);
// The redactions, bottom to top: what the union (3.10) is made of.
QList<Item> redactionItems(const Document& doc);
// The two points a box item keeps for `box`.
QList<QPointF> boxPoints(const QRect& box);
// The item moved by whole pixels: edges stay edges and centres centres.
Item translated(const Item& item, QPoint delta);
// The points an Arrow, Line, Pen or Marker is drawn through. A horizontal or
// vertical one of even width moves half a pixel up or left, onto the line
// between two pixels, so that both of its long edges fall on pixel edges;
// one of odd width stays on the pixel centres, where they fall already
// (3.4, rule 5). Anything slanted or curved keeps its points.
QList<QPointF> alignedPoints(const Item& item);

// The arrow head: max(4 x width, 12 logical px) long, 0.9 of that wide.
// Item keeps no scale, so the document's dpr comes in from outside.
qreal arrowHeadLength(int width, qreal dpr);
// An arrow's solid head (tip at the end point) and its shaft, which ends
// inside the head's root and never reaches the tip; both wind the same way
// so that one non-zero fill joins them. The shaft is empty when the arrow is
// no longer than its head; both are empty for an arrow of zero length.
struct ArrowShape {
    QPolygonF head;
    QPolygonF shaft;
};
ArrowShape arrowShape(const Item& item, qreal dpr);

// The centre line of a Pen or Marker stroke: through the first and the last
// point, with quadratic curves through the midpoints of the segments in
// between, which smooths the corners of a hand-drawn polyline.
QPainterPath strokeCentreline(const QList<QPointF>& points);
// Ramer-Douglas-Peucker: the fewest of `points` that stay within `tolerance`
// of the polyline, the first and last kept. Run on a stroke when the mouse
// is released, so the stored points stay pixel centres.
QList<QPointF> simplifyStroke(const QList<QPointF>& points, qreal tolerance = 0.75);

// The font of Text and Number: whole pixel sizes and grey antialiasing (no
// ClearType fringes, which would differ wherever the picture is pasted).
QFont annotationFont(int pixelSize, bool bold);

// A text's lines as drawn: whole-pixel baselines, so that moving the text by
// whole pixels moves every glyph by exactly that much.
struct TextMetrics {
    QSize size; // the widest line (rounded up) by the lines' height
    qreal width = 0; // the widest line's advance, not rounded
    int ascent = 0; // the first baseline, below the top
    int pitch = 0; // from one baseline to the next
    qreal capHeight = 0;
    QRectF ink; // where glyphs may reach, relative to the top left
    bool operator==(const TextMetrics&) const = default;
};
struct TextLayout {
    TextMetrics metrics;
    QList<QGlyphRun> runs; // glyph positions relative to the top left
};
// The layout of `text` ('\n' between lines) in annotationFont().
TextLayout layoutText(const QString& text, int pixelSize, bool bold);
// layoutText()'s metrics, remembered for the last few hundred texts of this
// thread: bounds() and hitTest() ask for them at every mouse move. Only plain
// numbers are kept, never fonts, so nothing is left to free at exit.
TextMetrics textMetrics(const QString& text, int pixelSize, bool bold);
void clearTextMetrics();

// White words and marks on a colour, #212121 on a light one (yellow, white),
// where white cannot be read (3.5, the exception for the user's colours).
QRgb inkOn(QRgb fill);

// A Text's outline (Style::textStroke): its width in image px, 0 when there is
// none or when the text is boxed (the white words sit on a colour already);
// never over kMaxTextStroke, whatever a file says.
constexpr int kMaxTextStroke = 16; // 3 logical px at 400 %, more than any screen has
int textStrokeWidth(const Item& item);
// Its colour: the one chosen, else the opposite of the text's own: dark
// round light words, white round dark ones (inkOn()), so it reads on any picture.
QRgb textStrokeColorOf(const Item& item);

// Where a Text item's parts are.
struct TextGeometry {
    QRect content; // the text, its top left at points[0]
    QRect box; // the coloured box when boxed, else the same as content
    qreal radius = 0; // the box's corners
    QRectF ink; // where glyphs may reach, the outline's width included
};
TextGeometry textGeometry(const Item& item);

// Where a Number item's parts are: a disc about 1.5 x the font size, a
// capsule for two digits and more.
struct NumberGeometry {
    QRectF disc;
    QString label; // what the number shows ("III", "c")
    QPoint origin; // the label's top left, whole pixels
    QRectF ink; // where the label's glyphs may reach
    int outline = 0; // the dark ring on a white disc (1 logical px), 0 for none
    qreal ring = 0; // Circled: the ring's width, the disc not filled; 0 for the filled disc
    // The item's own words (Item::text) to the right of the disc, their
    // first baseline on the number's; empty when there are none.
    QPoint wordsOrigin; // top left, whole pixels, also for no words
    QRect wordsBox;
    QRectF wordsInk;
};
NumberGeometry numberGeometry(const Item& item, int number, qreal dpr, NumberFormat format = NumberFormat::Arabic);
// The words of a Number: its colour, #212121 where that cannot be read (white).
QRgb numberWordsColor(const Item& item);
// The ring a white disc gets so that it shows on a white page: #212121 at 30 %.
QRgb numberOutlineColor();

// Every pixel the item may paint (line width, arrow head, text box and glyphs
// included). Export, dirty regions and hit testing go by it. `dpr` is the
// document's (the arrow head), `number` what a Number shows (two digits make
// a capsule).
QRectF bounds(const Item& item, qreal dpr, int number = 1, NumberFormat format = NumberFormat::Arabic);
// The same, the scale and the number taken from the document.
QRectF bounds(const Document& doc, const Item& item);
// The topmost item at `p` (the pixel under it). Lines count within half
// their width plus `tolerance` (4 logical px, given in image px); filled
// boxes inside, open ones near their edge; texts, numbers and redactions by
// their box. The redactions are at the bottom, so they come last.
std::optional<quint32> hitTest(const Document& doc, QPointF p, qreal tolerance);

// The handles of a box (Rect, Ellipse, Redact) in the order handles() lists
// them, at its edges; those of an Arrow or Line are its two points. Pen,
// Marker, Text and Number have none.
enum class BoxHandle : int { TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
enum class LineHandle : int { Start, End };
QList<QPointF> handles(const Item& item);
// The item with `handle` dragged to `to` (the pixel under it):
// - a box keeps the pixels of the opposite side and spans to that pixel, as
//   when it was drawn; dragged past the other side it turns over; with
//   `constrain` (Shift) a corner makes a square;
// - an Arrow or Line moves that end to the pixel's centre; `constrain` turns
//   it to a multiple of 15 degrees around the other end, the offset rounded
//   to whole pixels so it stays a pixel centre;
// - a Pen or Marker with LineHandle::End grows by that point; `constrain`
//   makes it one straight line from its first point, at 15 degree steps.
// Other kinds and handles come back unchanged.
Item reshaped(const Item& item, int handle, QPointF to, bool constrain);

// What every Number shows (3.9): per series, its numbers in creation order,
// counting from the series' start; a pinned number restarts the count there.
// Derived on every call, never stored, so deleting, undoing and redoing
// renumber by themselves.
QHash<quint32, int> numbers(const Document& doc);
// The format of series `series` (digits for one the document does not know).
NumberFormat formatOf(const Document& doc, quint32 series);
}
