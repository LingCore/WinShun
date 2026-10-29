#pragma once

#include "Keycaps.h"

#include <QFont>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <span>
#include <vector>

namespace ws::capture::keycaps {

// The rows drawn into one picture whenever they change (GitHub #98), on the
// media worker: QPainter with Win顺's font, so the keycaps are as crisp as
// its own text. Premultiplied ARGB, the frames' BGRA order; the overlay
// takes each row from it to its place in the frame, with its own opacity.
struct Picture {
    QImage image;
    std::vector<QRect> rows; // each row's rect in the image, in the order given
};
Picture render(std::span<const Row> rows, const QFont& font, int unit);

// Each row's top left in the frame (`region` the frame's size): the last
// row (the newest) at the bottom, those before it above, to the right edge
// or to the left one.
std::vector<QPoint> place(const Picture& picture, QSize region, int unit, bool left);

} // namespace ws::capture::keycaps
