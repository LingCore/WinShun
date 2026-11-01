#pragma once

#include <QList>
#include <QRect>

#include <algorithm>

namespace ws::dragbounds {

// Physical coordinates. A held window may straddle monitors; clipping it
// to the monitor under the pointer makes it jump by its width at the seam.
// Keep only the outer desktop limits here. Placement fits it to the final
// monitor's work area after release, including gaps and unequal heights.
inline QPoint constrain(QPoint proposed, QSize size, const QList<QRect>& workAreas)
{
    QRect desktop;
    for (const QRect& area : workAreas)
        desktop = desktop.united(area);
    if (desktop.isEmpty())
        return proposed;
    return {std::clamp(proposed.x(), desktop.left(), std::max(desktop.left(), desktop.x() + desktop.width() - size.width())),
        std::clamp(proposed.y(), desktop.top(), std::max(desktop.top(), desktop.y() + desktop.height() - size.height()))};
}

} // namespace ws::dragbounds
