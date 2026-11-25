#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringView>

#include <algorithm>
#include <array>

namespace ws {

// Picture extensions Windows Imaging Component reads (built-in codecs plus
// Windows' WebP, HEIF and AVIF extensions): the files Explorer's 贴到屏幕 is
// on (ShellMenu.cpp; the uninstaller's list in installer/WinShun.iss is the
// same, kept so by tst_settings), and 贴到屏幕 and 编辑 on a picture file in
// the launcher's and the clipboard history's menus.
inline constexpr std::array<QStringView, 18> kPinnablePictureExtensions {
    u"png", u"jpg", u"jpeg", u"jpe", u"jfif", u"bmp", u"dib", u"gif", u"webp",
    u"tif", u"tiff", u"ico", u"heic", u"heif", u"avif", u"jxr", u"wdp", u"hdp",
};

inline bool isPinnablePicture(QStringView path)
{
    const qsizetype slash = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    const qsizetype dot = path.lastIndexOf(u'.');
    if (dot <= slash || dot + 1 >= path.size())
        return false;
    const QStringView suffix = path.sliced(dot + 1);
    for (const QStringView ext : kPinnablePictureExtensions) {
        if (suffix.compare(ext, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

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
