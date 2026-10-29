#pragma once

#include <QDateTime>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QRgb>
#include <QSize>
#include <QString>

#include <functional>
#include <array>
#include <vector>

// The computations of the screen capture that need no window, no screen and
// no Windows API: how Qt really scales a window, the selection and its
// handles, where the toolbar, the size label and the magnifier go, which
// window or control is under the mouse, the file name. Everything here is in
// physical pixels unless a name says otherwise (docs/design-capture.md, 4.2).
// Rectangles are QRect: x, y, width, height, the right and bottom pixels
// included (right() = x + width - 1).
namespace ws::capture {

// Qt keeps a window's size in whole logical pixels and its buffer is that
// size times the device pixel ratio, rounded: at 150% a 2560 px monitor is
// 1707 logical, a buffer of 2561 px, and the scene is stretched onto it at
// 2561 / 1707, not 1.5. A frozen picture shown "physical / DPR" lands one
// pixel off at the right and bottom (stage 0, capture-probe-qt). This is the
// scale Qt uses: buffer pixels per logical pixel.
double bufferScale(int logicalSize, double dpr);
// The logical length that covers `physical` buffer pixels of such a window.
double toLogical(double physical, int logicalSize, double dpr);
// The pixel under a mouse position Qt gave in logical units: Qt divided the
// native position by the window's DPR, so multiplying gives it back.
int toPhysical(double logical, double dpr);

// The rectangle spanned by two pixels, both inside it.
QRect spanRect(QPoint a, QPoint b);
// `r` moved (not resized) into `bounds`; shrunk to fit when larger.
QRect moveInto(QRect r, const QRect& bounds);

// What a press on or near the selection takes hold of.
enum class Handle { None, Move, Left, Top, Right, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
// The handle within `reach` pixels of `p` (corners before edges), Move inside
// the selection, None elsewhere.
Handle handleAt(const QRect& selection, QPoint p, int reach);
// The selection `start` with `handle` dragged by `delta`, kept in `bounds`.
// Edges dragged past the opposite edge swap with it; never under 1x1.
QRect dragHandle(const QRect& start, Handle handle, QPoint delta, const QRect& bounds);
// The arrow keys: the selection moves one pixel (dx, dy in -1..1); with
// `resize` (Shift) its right edge moves by dx and its bottom edge by dy
// instead, never under 1x1. Kept in `bounds`.
QRect nudge(const QRect& r, int dx, int dy, bool resize, const QRect& bounds);
// The cursor shape (Qt::CursorShape) for a handle.
int cursorFor(Handle handle);

// The toolbar's place: under the selection, its right edges aligned; above
// it where there is no room below; inside its bottom when neither fits.
// Always in `screen`.
struct ToolbarPlace {
    enum Where { Below, Above, Inside };
    QPoint topLeft;
    Where where = Below;
};
ToolbarPlace placeToolbar(const QRect& selection, QSize bar, const QRect& screen, int gap);
// Long capture's four click-through strips: outside the selection where
// there is room, inside the screen edge when the selection touches it.
std::array<QRect, 4> longCaptureBorder(const QRect& selection, const QRect& screen, int thickness);
// The size label: over the selection's top left corner, inside it at the
// top when the screen's edge is in the way.
QPoint placeSizeLabel(const QRect& selection, QSize label, const QRect& screen, int gap);
// The magnifier: right of and below the cursor, flipped to the other side
// where the screen ends.
QPoint placeMagnifier(QPoint cursor, QSize box, const QRect& screen, int offset);

// What can be picked under the mouse, smallest first: the controls (child
// windows) of the topmost window there that contain the point, by size,
// then that window, then the whole screen. `windows` are the top-level
// windows from the top of the Z order down; `children(i)` lists window i's
// child windows (asked for only when the point is in window i). All rects
// in the screen's own coordinates; each is cut to the screen, and one equal
// to the previous is left out.
std::vector<QRect> hitChain(const std::vector<QRect>& windows, const std::function<std::vector<QRect>(std::size_t)>& children,
    QPoint p, const QRect& screen);

// "WinShun_20261019_143005.png", or with _1, _2 … while `taken` says the
// name is in use.
QString screenshotFileName(const QDateTime& when, const std::function<bool(const QString&)>& taken);
// "#1E90FF"
QString colorHex(QRgb color);

// ---- pins (PinWindow): a picture left on the screen ---------------------------

// The zoom steps the wheel and +/- go through (1.0 among them, so 100% is
// always reached exactly): every 10% from 10% to 200%, then 3x to 8x.
const std::vector<double>& zoomSteps();
// The step `steps` notches from `zoom` (positive: larger), no further than
// `maxZoom`. A zoom between two steps goes to the next one in that direction.
double nextZoom(double zoom, int steps, double maxZoom = 8.0);
// How far a picture of `image` pixels may be zoomed without its window
// passing `budgetPixels` (a 2560x1600 pin at 800% would be 1 GB of window):
// at most 8, never under 1.
double maxZoom(QSize image, qint64 budgetPixels);
// The picture's size on the screen at `zoom`: exactly `image` at 1.0, never
// under 1x1.
QSize zoomedSize(QSize image, double zoom);
// Where the picture's top left goes when its zoom changes from `oldZoom` to
// `newZoom` with the cursor at `cursor` (all physical, virtual desktop): the
// picture's pixel under the cursor stays under it. Whole pixels.
QPoint zoomAbout(QPoint origin, QPoint cursor, double oldZoom, double newZoom);
// The opacity `steps` tenths from `opacity`, from 0.1 to 1.
double nextOpacity(double opacity, int steps);

// What surrounds a pin's picture, in physical pixels at a monitor's DPI
// scale: a 1 logical pixel border and a soft shadow of 8 logical pixels.
// Its window is the picture with both around it; the picture keeps its
// physical size on every monitor, only these follow the DPI.
struct PinFrame {
    int border = 1;
    int shadow = 8;
    int margin() const { return border + shadow; }
};
PinFrame pinFrame(double dpr);
// Whether a pin lets the mouse through (WS_EX_TRANSPARENT) right now. A pin
// set to 鼠标穿透 takes the mouse back while Ctrl is held with the cursor over
// it (#59: the tray was the only way back), and keeps it while it is being
// dragged or its menu is up (`busy`), whatever Ctrl does meanwhile.
bool pinLetsMouseThrough(bool clickThrough, bool ctrlDown, bool cursorOver, bool busy);
// The pin's window for a picture shown at `picture` (physical, virtual desktop).
QRect pinWindowRect(const QRect& picture, const PinFrame& frame);

// ---- recognising text (OCR) ------------------------------------------------

// How much larger to make a picture before reading it: text captured at
// 100% or 125% is small for the text finder (a simulated 100% capture read
// "Ctrl" as "Ctr1" and found stray glyphs; twice the size read it as at
// 150%). Up to 2x, no wider than `maxWidth` (PaddleOcr shrinks wider ones
// back); 1 when that gains under a quarter.
double ocrUpscale(QSize image, double dpr, int maxWidth = 2000);
// The words read as one line for a search box or a web search: runs of
// whitespace (the line breaks) become one space, trimmed, cut at `most`
// characters; empty when there are no words.
QString ocrOneLine(const QString& text, int most = 256);

// A window of `box` beside `anchor`: right of it, else left, else under,
// else over, else inside the area's middle; always in `area`.
QPoint placeBeside(const QRect& anchor, QSize box, const QRect& area, int gap);

// ---- the Start menu (it is not among the windows EnumWindows lists) ---------

// Where a flyout was: the part of `area` where `before` (with it) differs
// from `after` (the same monitor once it closed; same size, Format_RGB32,
// `area` in their pixels). The longest runs of columns and rows in which at
// least `density` of the pixels changed by more than `threshold` in some
// channel, without the shadow round it. Null when nothing changed that much.
QRect changedRegion(const QImage& before, const QImage& after, const QRect& area, int threshold, double density);

} // namespace ws::capture
