#pragma once

#include <QPointF>
#include <QRect>
#include <QRectF>

#include <span>
#include <vector>

namespace ws::pie {

// The drop pie's shape (DropPie, DropPieWindow.qml): a small middle at the
// pointer and up to eight folder chips round it, chip 0 above it and the
// rest clockwise. A folder is chosen by the direction the pointer went from
// the middle, as in a marking menu, long before it reaches the chip, or by
// being on the chip itself. Pure geometry in logical pixels, unit-tested
// (tst_piegeometry); angles in degrees clockwise from the top, as a clock's.
struct Metrics {
    double hub = 20; // the middle's radius, as drawn
    double dead = 26; // within it nothing is chosen: let go there and nothing happens
    double chipHeight = 32;
    double chipGap = 8; // at least between two chips, and between a chip and the middle
    double minRadius = 44; // the chips' ring at its smallest
    double reach = 36; // past the chips before the pointer has left the pie

    // All of it times `k` (界面大小: the label type's size over 13 px).
    Metrics scaled(double k) const noexcept;
};

// The scale for a 界面大小 step (Theme.step: 0 dense and compact, 1 standard, 2 roomy):
// the labels' type is 12, 13 or 14 px, the pie grows with it.
double scaleForStep(int step) noexcept;
int labelPixelSize(int step) noexcept;

double angleOf(QPointF offset) noexcept; // 0 up, 90 right, in [0, 360)
int sectorAt(double angle, int sectors) noexcept;
double sectorCentre(int index, int sectors) noexcept;
QPointF polar(double radius, double angle) noexcept; // the offset from the centre

// The chips round the centre, one per sector, as the labels of a marking
// menu: those to a side start (to the right) or end (to the left) on a ring
// at their direction, centred on it from top to bottom, and go out
// sideways, so a long name makes the pie wider, not the ring bigger; those
// straight above and below are centred on their direction with their side
// nearest the centre on the ring, or further out where a side chip is in
// the way. The ring is as small as it can be from minRadius on, with no
// chip within chipGap of another or of the middle.
struct Layout {
    double radius = 0;
    std::vector<QRectF> chips; // from the centre
    QRectF bounds; // the chips and the middle, from the centre
    QRectF reach; // bounds widened by Metrics::reach: where the pointer is still on the pie
};
Layout layOut(std::span<const double> chipWidths, const Metrics& metrics);

enum class Zone { Dead, Sector, Outside };
struct Hit {
    Zone zone = Zone::Outside;
    int sector = -1; // Sector: which one
};

// Where `offset` (from the centre) falls: the middle; a chip it is on (or
// within half the gap round it), else the sector whose direction it is in
// (anywhere within the layout's reach); or outside the pie.
Hit hitTest(QPointF offset, const Layout& layout, const Metrics& metrics) noexcept;

// What the drag's source allows (DROPEFFECT_MOVE, DROPEFFECT_COPY), and the
// keys held.
struct Allowed {
    bool move = true;
    bool copy = true;
};
struct Keys {
    bool ctrl = false;
    bool shift = false;
};
enum class Mode { None, Move, Copy };

// What letting go on a folder does, as in File Explorer: Ctrl copies and
// Shift moves while held (both: neither key counts, Windows' "make a
// shortcut"); else moving within one drive and copying to another. One the
// source does not allow gives way to the other; None when it allows neither.
Mode modeFor(bool sameDrive, Keys keys, Allowed allowed) noexcept;

// Where the middle goes for a pointer at `cursor`: on it, moved in just
// enough for `extent` (around the centre) to be inside `workArea`, or the
// work area's middle on an axis too small for it. Moved more than
// `safeShift`, the pointer would start beside a folder: `warp` says to move
// it to the middle then, as pie menus do by a screen's edge.
struct Placement {
    QPointF centre;
    bool warp = false;
};
Placement placeCentre(QPointF cursor, const QRectF& workArea, const QRectF& extent, double safeShift) noexcept;

// The whole logical pixel from `v` on, down (`step` -1) or up (+1), that is
// a whole device pixel too at the scale `dpr`, counted from 0 (a monitor's
// corner, where both start): every second one at 150 %, every fourth at
// 125 % and 175 %. One that is not is drawn a fraction off.
int wholePixel(double v, double dpr, int step) noexcept;

// The window: `extent` around `centre`, outwards to whole pixels (as
// wholePixel, from the monitor's corner: a window whose size is not a whole
// number of device pixels is drawn stretched a little to fit, all its edges
// blurred), within the one monitor it is on (never across two: they may
// differ in scale; its rectangle in whole pixels too).
QRect windowRect(QPointF centre, const QRectF& extent, const QRect& monitor, double dpr) noexcept;

} // namespace ws::pie
