#pragma once

#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QSize>

// How the editing window shows a picture (docs/design-capture-roadmap.md,
// 2.2, 3.3, 3.4): the zoom, where the picture lies in the viewport, and
// which of the canvas's 256 px tiles that needs. Everything here is plain
// numbers, in device pixels unless a name says image pixels: the window's
// real buffer pixels, the unit in which 100 % is one image pixel to one
// screen pixel and nothing is resampled.
namespace ws::capture::editview {

// The smallest and largest zoom (Geometry.h's zoomSteps() goes from 10 % to 800 %).
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 8.0;
// Fit-to-window below this would show a long picture as a sliver (a
// 1080 x 30000 one is 3 % of a window): fit to the other side then.
constexpr double kLeastFit = 0.25;

// The zoom that shows `image` whole in `viewport` (device px), never above
// 100 %: a picture smaller than the window is shown as it is. For a picture
// so long or wide that the whole of it would be under kLeastFit, the zoom
// that fits its shorter side instead (its width in a tall picture), at least
// kLeastFit, and never above 100 %.
double fitZoom(QSize image, QSize viewport);
// `zoom` kept within kMinZoom and kMaxZoom.
double clampZoom(double zoom);
// The picture's size on the screen at `zoom`: exactly `image` at 1.0, never
// under 1 x 1.
QSize shownSize(QSize image, double zoom);
// Where the picture's top left may be (device px from the viewport's top
// left) for `origin` asked for: centred along an axis on which the picture
// fits, else not leaving a gap at either end.
QPoint clampOrigin(QPoint origin, QSize shown, QSize viewport);
// The picture's pixel under the device pixel `at` of the viewport.
QPoint imagePixelAt(QPoint at, QPoint origin, double zoom);
// The part of the picture the viewport shows (image px, as exact numbers).
QRectF visibleImage(QPoint origin, double zoom, QSize viewport);

// The canvas tiles `visible` needs: columns and rows of tiles (not pixels)
// of a picture of `image` pixels cut into `tile` pixel squares, with `ring`
// more tiles round them. Empty when nothing is visible.
QRect tileRange(const QRectF& visible, QSize image, int tile, int ring);
// The tile indexes of `range` (row by row in a picture `columns` tiles wide),
// nearest to `centre` (image px) first, and at most `limit` of them.
QList<int> nearestTiles(const QRect& range, int columns, int tile, QPointF centre, int limit);

// Whole pixels from a length: a logical length at the window's real buffer
// scale (device px per logical px).
int deviceLength(double logical, double bufferScale);

// ---- the ways out (docs/design-capture-roadmap.md, 2.2) ----

// Where the picture came from.
enum class Origin {
    Fresh, // a picture with no history entry behind it
    History, // a history entry (jt, the clipboard window)
    Pin, // a pin on the screen
};
enum class Exit {
    NewEntry, // copied: a new history entry with a project of its own
    TouchOriginal, // copied as it was: the original entry moves to the top, nothing is written
    ReplacePin, // 完成: the pin shows the new picture where it is
    Close, // nothing to hand over
};
struct ExitPlan {
    Exit exit = Exit::Close;
    bool record = false; // the history gets the picture as an entry too
};
// What Enter does. `originalThere`: the history entry (History) or the pin
// (Pin) is still there. A change makes a new entry, the original left as it
// is, like saving a copy; no change copies the original again; on a pin
// Enter is 完成, which hands over a changed picture only; a pin gone
// meanwhile leaves the copy as the way out.
ExitPlan planDone(Origin origin, bool modified, bool originalThere);

// The window that puts the canvas where a pin is: `picture` is the pin's
// picture on the screen, `imageInWindow` where the canvas's top left is in
// the window (device px), `window` the window's size, `work` the area it
// must stay in (all physical, virtual desktop). Its top left, moved into
// `work` when it does not fit there, and whether the canvas then lies
// exactly where the picture was.
struct WindowPlace {
    QPoint topLeft;
    bool exact = true;
};
WindowPlace placeForPin(const QRect& picture, QPoint imageInWindow, QSize window, const QRect& work);

// ---- the editing window on a pin (GitHub #343): the pin grows a toolbar ----

// The device px between the canvas and the window's edge where they meet
// (Theme.edge's width): DWM's border lies there, not on the picture.
int pinInset(double dpr);

// The window that is the pin with a toolbar: the canvas exactly over the
// pin's picture at the pin's zoom, the band of the toolbar's row (next to
// the canvas) and the style panel's row (beyond it) above the canvas, or
// below it when the work area has no room above; as wide as the canvas or
// the band, whichever is wider, nothing else round it. `canvas`: the whole
// canvas on the screen at the pin's zoom (physical, virtual desktop: its top
// left is where the window must put it); `band`, `chrome`: the band's height
// and the width its rows need (logical px); `work`: the monitor's work area.
struct PinLayout {
    // Logical px, at Windows' scales a whole number of device px each way:
    // the band then lies on whole device px.
    QSize window;
    // The canvas's left in the window (device px): centred under a wider
    // band, moved so that the window stays in `work`.
    int canvasX = 0;
    bool below = false; // the band under the canvas
};
PinLayout pinLayout(const QRect& canvas, double dpr, int band, int chrome, const QRect& work);
// The canvas's part of such a window (device px, window coordinates):
// `window` its size, `shown` the canvas's size at the zoom now,
// `bandDevice` the band's height; never past the window's insets.
QRect pinViewport(QSize window, QSize shown, int bandDevice, int canvasX, bool below, int inset);

} // namespace ws::capture::editview
