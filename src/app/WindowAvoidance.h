#pragma once

#include <QRect>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace ws::windowavoidance {

// Keep the user's width and result capacity. Among positions inside the
// work area, prefer the least overlap, then the least movement from home.
inline QPoint place(const QRect& area, const QRect& obstacle, QSize size, QPoint preferred, int gap = 12)
{
    size = size.boundedTo(area.size());
    const int right = area.x() + area.width() - size.width();
    const int bottom = area.y() + area.height() - size.height();
    const std::array xs {preferred.x(), obstacle.x() - size.width() - gap,
        obstacle.x() + obstacle.width() + gap, area.x(), right};
    const std::array ys {preferred.y(), obstacle.y() - size.height() - gap,
        obstacle.y() + obstacle.height() + gap, area.y(), bottom};
    std::pair<qint64, qint64> best {std::numeric_limits<qint64>::max(), std::numeric_limits<qint64>::max()};
    QPoint chosen = preferred;
    for (int x : xs) {
        for (int y : ys) {
            const QPoint at {std::clamp(x, area.x(), right), std::clamp(y, area.y(), bottom)};
            const QRect overlap = QRect(at, size).intersected(obstacle);
            const qint64 dx = qint64(at.x()) - preferred.x();
            const qint64 dy = qint64(at.y()) - preferred.y();
            const std::pair score {qint64(overlap.width()) * overlap.height(), dx * dx + dy * dy};
            if (score < best) {
                best = score;
                chosen = at;
            }
        }
    }
    return chosen;
}

// Over the file list typed in (Directory Opus, GitHub #82), as Listary opens
// its box: in the list's bottom right corner, `gap` in from its edges; as wide
// and as tall (the room for rows) as the list leaves, between `least` and
// `wanted`. The rectangle's size is the window's width and that room. Larger
// than the list still, it reaches over the lister's other file lists (`lists`,
// all of them together) before the rest of the lister (`frame`: its folder
// tree, viewer, toolbars), from the same corner, and stays on the work area.
// Empty `lists` or `frame`: not known.
inline QRect inList(const QRect& area, const QRect& list, const QRect& lists, const QRect& frame, QSize wanted, QSize least,
    int gap = 12)
{
    const auto span = [&](int want, int fewest, int room, int most) {
        return std::min(most, std::max(fewest, std::min(want, room - 2 * gap)));
    };
    const int width = span(wanted.width(), least.width(), list.width(), area.width());
    const int height = span(wanted.height(), least.height(), list.height(), area.height());
    int x = list.x() + list.width() - gap - width;
    int y = list.y() + list.height() - gap - height;
    // Into each of these it fits in, the list first; where it does not, it
    // stays at that corner.
    const auto into = [](int pos, int size, int from, int extent) {
        return size <= extent ? std::clamp(pos, from, from + extent - size) : pos;
    };
    for (const QRect& within : {list, lists.isEmpty() ? list : lists.united(list), frame, area}) {
        if (within.isEmpty())
            continue;
        x = into(x, width, within.x(), within.width());
        y = into(y, height, within.y(), within.height());
    }
    return {x, y, width, height};
}

} // namespace ws::windowavoidance
