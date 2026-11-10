#pragma once

#include <QImage>
#include <QPoint>
#include <QStringList>

namespace ws {

// What follows the pointer while results or clips are dragged out of Win顺
// (dragout::drag): the first item's thumbnail or large icon with a soft
// shadow, photos stacked up to three when there are several, and their count
// on an accent badge. Drawn in device pixels for `dpr`, never scaled
// afterwards; `hotspot` puts the pointer's tip just under the picture, so it
// does not hide where the items go.
struct DragPicture {
    QImage image;
    QPoint hotspot;
};

// Quick enough for the start of a drag: a picture file read small, else
// Windows' cached thumbnail (no thumbnail handler runs), else the icon.
DragPicture dragPicture(const QStringList& paths, qreal dpr);

} // namespace ws
