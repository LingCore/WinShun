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

} // namespace ws::windowavoidance
