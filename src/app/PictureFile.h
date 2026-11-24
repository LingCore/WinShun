#pragma once

#include <QImage>
#include <QByteArray>
#include <QSize>
#include <QString>

namespace ws {

// Whether 贴到屏幕 and 编辑 are offered for a file, by its extension: the
// kinds Windows reads out of the box (Windows Imaging Component's own
// codecs; WebP's comes with Windows; a GIF's first frame). Not the search's
// wider 图片 (PSD, SVG, camera RAW, HEIC), which they could not open.
bool pinnablePicture(QStringView path);

// A picture file's first frame (PNG, JPEG, BMP, GIF, WebP, … whatever
// Windows Imaging Component reads), upright as the camera says, at most
// `largest` pixels on its longer side (0: as stored); null if it cannot be
// read. Any thread with COM initialised. For 贴到屏幕 (dist has no Qt image
// plugins: QImage alone reads PNG and BMP only).
QImage decodePictureFile(
    const QString& path, int largest, QSize* original = nullptr, int frame = 0, int* frames = nullptr);
// The same, shrunk to fit within `bound` upright, the aspect kept (as an
// Image's sourceSize: a side of 0 or less is free; never enlarged).
QImage decodePictureFile(const QString& path, QSize bound, QSize* original = nullptr);
// The same decoder for an embedded picture, with optional multi-frame access.
// `maxPixels` > 0: a frame larger than that is refused from its header, before
// any pixel is decoded (null image, `original` set to its stored size).
QImage decodePictureData(const QByteArray& bytes, int largest, QSize* original = nullptr, int frame = 0,
    int* frames = nullptr, qint64 maxPixels = 0);

} // namespace ws
