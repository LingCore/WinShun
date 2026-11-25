#include "PieGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ws::pie {

namespace {

constexpr double kDegree = std::numbers::pi / 180;

// How far `rect` is from the origin at its nearest point.
double distanceFromCentre(const QRectF& rect) noexcept
{
    const double dx = std::max({rect.left(), -rect.right(), 0.0});
    const double dy = std::max({rect.top(), -rect.bottom(), 0.0});
    return std::hypot(dx, dy);
}

// Straight up or down, or nearly (no page of up to eight chips puts one
// in between): centred there, not to a side.
bool upright(double angle) noexcept
{
    return std::abs(std::sin(angle * kDegree)) < 0.2;
}

QRectF chipAt(double radius, double angle, double width, double height) noexcept
{
    const QPointF anchor = polar(radius, angle);
    if (upright(angle))
        return {anchor.x() - width / 2, anchor.y() < 0 ? anchor.y() - height : anchor.y(), width, height};
    return {anchor.x() > 0 ? anchor.x() : anchor.x() - width, anchor.y() - height / 2, width, height};
}

// The chips above and below out past the side chips they would touch: a
// wide one there moves on its own, the ring stays.
void clearUpright(std::vector<QRectF>& chips, const std::vector<bool>& up, double gap) noexcept
{
    for (std::size_t i = 0; i < chips.size(); ++i) {
        if (!up[i])
            continue;
        QRectF& chip = chips[i];
        const bool above = chip.center().y() < 0;
        for (std::size_t j = 0; j < chips.size(); ++j) {
            const QRectF& side = chips[j];
            if (up[j] || side.left() >= chip.right() + gap || side.right() <= chip.left() - gap)
                continue;
            if (above)
                chip.moveBottom(std::min(chip.bottom(), side.top() - gap));
            else
                chip.moveTop(std::max(chip.top(), side.bottom() + gap));
        }
    }
}

bool clear(const std::vector<QRectF>& chips, const Metrics& m) noexcept
{
    const double half = m.chipGap / 2;
    for (std::size_t i = 0; i < chips.size(); ++i) {
        if (distanceFromCentre(chips[i]) < m.hub + m.chipGap)
            return false;
        const QRectF a = chips[i].adjusted(-half, -half, half, half);
        for (std::size_t j = i + 1; j < chips.size(); ++j) {
            if (a.intersects(chips[j].adjusted(-half, -half, half, half)))
                return false;
        }
    }
    return true;
}

} // namespace

Metrics Metrics::scaled(double k) const noexcept
{
    return {hub * k, dead * k, chipHeight * k, chipGap * k, minRadius * k, reach * k};
}

int pageCount(int folders) noexcept
{
    return std::max(1, (std::max(0, folders) + kPageSize - 1) / kPageSize);
}

int clampPage(int page, int folders) noexcept
{
    return std::clamp(page, 0, pageCount(folders) - 1);
}

PageSlice pageSlice(int folders, int page) noexcept
{
    const int first = clampPage(page, folders) * kPageSize;
    return {first, std::clamp(folders - first, 0, kPageSize)};
}

int turnedPage(int page, int folders, bool back) noexcept
{
    const int pages = pageCount(folders);
    return (clampPage(page, folders) + (back ? pages - 1 : 1)) % pages;
}

double scaleForStep(int step) noexcept
{
    return labelPixelSize(step) / 13.0;
}

int labelPixelSize(int step) noexcept
{
    return step <= 0 ? 12 : step >= 2 ? 14 : 13;
}

double angleOf(QPointF offset) noexcept
{
    double angle = std::atan2(offset.x(), -offset.y()) / kDegree; // y grows downwards on screen
    if (angle < 0)
        angle += 360;
    return angle >= 360 ? 0 : angle;
}

int sectorAt(double angle, int sectors) noexcept
{
    if (sectors <= 1)
        return sectors == 1 ? 0 : -1;
    const double step = 360.0 / sectors;
    const double shifted = std::fmod(angle + step / 2 + 360, 360);
    return std::clamp(static_cast<int>(shifted / step), 0, sectors - 1);
}

double sectorCentre(int index, int sectors) noexcept
{
    return sectors > 0 ? index * 360.0 / sectors : 0;
}

QPointF polar(double radius, double angle) noexcept
{
    return {radius * std::sin(angle * kDegree), -radius * std::cos(angle * kDegree)};
}

Layout layOut(std::span<const double> chipWidths, const Metrics& metrics)
{
    const int count = static_cast<int>(chipWidths.size());
    Layout layout;
    layout.chips.resize(chipWidths.size());
    std::vector<bool> up(chipWidths.size());
    for (int i = 0; i < count; ++i)
        up[static_cast<std::size_t>(i)] = upright(sectorCentre(i, count));
    const auto place = [&](double radius) {
        for (int i = 0; i < count; ++i)
            layout.chips[static_cast<std::size_t>(i)] = chipAt(radius, sectorCentre(i, count),
                chipWidths[static_cast<std::size_t>(i)], metrics.chipHeight);
        clearUpright(layout.chips, up, metrics.chipGap);
    };
    // A logical pixel at a time: a few dozen steps at most.
    double radius = metrics.minRadius;
    for (place(radius); !clear(layout.chips, metrics) && radius < metrics.minRadius + 400; place(radius))
        radius += 1;
    layout.radius = radius;
    layout.bounds = QRectF(-metrics.hub, -metrics.hub, 2 * metrics.hub, 2 * metrics.hub);
    for (const QRectF& chip : layout.chips)
        layout.bounds = layout.bounds.united(chip);
    layout.reach = layout.bounds.adjusted(-metrics.reach, -metrics.reach, metrics.reach, metrics.reach);
    return layout;
}

Hit hitTest(QPointF offset, const Layout& layout, const Metrics& metrics) noexcept
{
    if (std::hypot(offset.x(), offset.y()) < metrics.dead)
        return {Zone::Dead};
    if (!layout.reach.contains(offset) || layout.chips.empty())
        return {Zone::Outside};
    // On a chip, or within half the gap round it: that one, whichever
    // direction its far end is in.
    const double half = metrics.chipGap / 2;
    for (std::size_t i = 0; i < layout.chips.size(); ++i) {
        if (layout.chips[i].adjusted(-half, -half, half, half).contains(offset))
            return {Zone::Sector, static_cast<int>(i)};
    }
    return {Zone::Sector, sectorAt(angleOf(offset), static_cast<int>(layout.chips.size()))};
}

ColumnBox placeColumn(const QRectF& beside, double levelY, int side, double width, int rows, const QRectF& area,
    const ColumnMetrics& m) noexcept
{
    ColumnBox box;
    const double tallest = std::max(m.row + 2 * m.padding, std::min(m.maxHeight, area.height() - 2 * m.margin));
    const int count = std::max(1, rows);
    double height = count * m.row + 2 * m.padding;
    box.shown = count;
    if (height > tallest) {
        box.scrolls = true;
        box.shown = std::max(1, static_cast<int>(std::floor((tallest - 2 * m.padding - 2 * m.strip()) / m.row)));
        height = box.shown * m.row + 2 * m.padding + 2 * m.strip();
    }
    const double right = beside.right() + m.gap;
    const double left = beside.left() - m.gap - width;
    const bool roomRight = right + width <= area.right() - m.margin;
    const bool roomLeft = left >= area.left() + m.margin;
    box.side = side < 0 ? -1 : 1;
    if (box.side > 0 ? !roomRight && roomLeft : !roomLeft && roomRight)
        box.side = -box.side;
    const double x = std::clamp(box.side > 0 ? right : left, area.left() + m.margin,
        std::max(area.left() + m.margin, area.right() - m.margin - width));
    const double y = levelY - m.row / 2 - m.padding - (box.scrolls ? m.strip() : 0);
    box.rect = QRectF(x, std::clamp(y, area.top() + m.margin, std::max(area.top() + m.margin, area.bottom() - m.margin - height)),
        width, height);
    return box;
}

ColumnHit hitColumn(QPointF point, const ColumnBox& box, int first, int rows, const ColumnMetrics& m) noexcept
{
    if (!box.rect.contains(point))
        return {};
    if (box.scrolls) {
        if (point.y() < box.rect.top() + m.padding + m.strip())
            return {ColumnPart::Up};
        if (point.y() >= box.rect.bottom() - m.padding - m.strip())
            return {ColumnPart::Down};
    }
    const double offset = point.y() - box.rowsTop(m);
    const int inSight = static_cast<int>(std::floor(offset / m.row));
    const int row = first + inSight;
    if (offset < 0 || inSight >= box.shown || row < 0 || row >= rows)
        return {ColumnPart::Box};
    return {ColumnPart::Row, row};
}

int clampFirst(int first, int rows, int shown) noexcept
{
    return std::clamp(first, 0, std::max(0, rows - shown));
}

bool inTriangle(QPointF point, QPointF a, QPointF b, QPointF c) noexcept
{
    // Which side of each edge it is on: all the same side (or on one), inside.
    const auto cross = [](QPointF o, QPointF p, QPointF q) {
        return (p.x() - o.x()) * (q.y() - o.y()) - (p.y() - o.y()) * (q.x() - o.x());
    };
    if (std::abs(cross(a, b, c)) < 1e-9)
        return false;
    const double ab = cross(a, b, point);
    const double bc = cross(b, c, point);
    const double ca = cross(c, a, point);
    const bool below = ab < 0 || bc < 0 || ca < 0;
    const bool above = ab > 0 || bc > 0 || ca > 0;
    return !(below && above);
}

bool inHullOf(QPointF point, const QRectF& a, const QRectF& b) noexcept
{
    if (a.contains(point) || b.contains(point))
        return true;
    const auto cross = [](QPointF o, QPointF p, QPointF q) {
        return (p.x() - o.x()) * (q.y() - o.y()) - (p.y() - o.y()) * (q.x() - o.x());
    };
    // The hull of their eight corners (Andrew's monotone chain), then the
    // point on the inner side of each of its edges.
    std::array corners {a.topLeft(), a.topRight(), a.bottomRight(), a.bottomLeft(), b.topLeft(), b.topRight(),
        b.bottomRight(), b.bottomLeft()};
    std::ranges::sort(corners, [](QPointF p, QPointF q) { return p.x() < q.x() || (p.x() == q.x() && p.y() < q.y()); });
    std::array<QPointF, 2 * corners.size()> hull {};
    std::size_t k = 0;
    for (const QPointF& p : corners) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], p) <= 0)
            --k;
        hull[k++] = p;
    }
    for (std::size_t i = corners.size() - 1, lower = k + 1; i-- > 0;) {
        while (k >= lower && cross(hull[k - 2], hull[k - 1], corners[i]) <= 0)
            --k;
        hull[k++] = corners[i];
    }
    if (k < 4)
        return false; // no area: only on a or b, asked above
    for (std::size_t i = 0; i + 1 < k; ++i) {
        if (cross(hull[i], hull[i + 1], point) < -1e-9)
            return false;
    }
    return true;
}

Mode modeFor(bool sameDrive, Keys keys, Allowed allowed) noexcept
{
    Mode wanted = sameDrive ? Mode::Move : Mode::Copy;
    if (keys.ctrl != keys.shift)
        wanted = keys.ctrl ? Mode::Copy : Mode::Move;
    if (wanted == Mode::Move && !allowed.move)
        wanted = Mode::Copy;
    else if (wanted == Mode::Copy && !allowed.copy)
        wanted = Mode::Move;
    if ((wanted == Mode::Move && !allowed.move) || (wanted == Mode::Copy && !allowed.copy))
        return Mode::None;
    return wanted;
}

Placement placeCentre(QPointF cursor, const QRectF& workArea, const QRectF& extent, double safeShift) noexcept
{
    // Each axis into the band where the extent fits; a work area too small
    // for it: its middle.
    const auto fit = [](double v, double low, double high, double before, double after) {
        return high - low < after - before ? (low + high - before - after) / 2
                                           : std::clamp(v, low - before, high - after);
    };
    const QPointF centre(fit(cursor.x(), workArea.left(), workArea.right(), extent.left(), extent.right()),
        fit(cursor.y(), workArea.top(), workArea.bottom(), extent.top(), extent.bottom()));
    const QPointF shift = centre - cursor;
    return {centre, std::hypot(shift.x(), shift.y()) > safeShift};
}

int wholePixel(double v, double dpr, int step) noexcept
{
    const int start = static_cast<int>(step < 0 ? std::floor(v + 1e-9) : std::ceil(v - 1e-9));
    // A scale of Windows' (a multiple of 1/4, or of 1/8 as set by hand) is
    // whole within eight; for any other the whole logical pixel does.
    for (int i = 0; i < 8; ++i) {
        const int at = start + i * step;
        const double device = at * dpr;
        if (std::abs(device - std::round(device)) < 1e-6)
            return at;
    }
    return start;
}

QRect windowRect(QPointF centre, const QRectF& extent, const QRect& monitor, double dpr) noexcept
{
    // Outwards, so all of the extent is inside.
    const QRectF box = extent.translated(centre - monitor.topLeft());
    const int left = wholePixel(box.left(), dpr, -1);
    const int top = wholePixel(box.top(), dpr, -1);
    const int right = wholePixel(box.right(), dpr, 1);
    const int bottom = wholePixel(box.bottom(), dpr, 1);
    return QRect(left, top, right - left, bottom - top).translated(monitor.topLeft()).intersected(monitor);
}

} // namespace ws::pie
