#include "ClipImage.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace ws {

std::span<const ClipImageOffer> clipImageOffers()
{
    static constexpr std::array offers {
        ClipImageOffer {u"GIF", ClipImageFormat::Gif},
        ClipImageOffer {u"image/gif", ClipImageFormat::Gif},
        ClipImageOffer {u"JFIF", ClipImageFormat::Jpeg},
        ClipImageOffer {u"JPEG", ClipImageFormat::Jpeg},
        ClipImageOffer {u"image/jpeg", ClipImageFormat::Jpeg},
        ClipImageOffer {u"PNG", ClipImageFormat::Png},
        ClipImageOffer {u"image/png", ClipImageFormat::Png},
        ClipImageOffer {u"BMP", ClipImageFormat::Png},
        ClipImageOffer {u"image/bmp", ClipImageFormat::Png},
    };
    return offers;
}

DecodedClipImage encodeClipImage(const QImage& image)
{
    DecodedClipImage result;
    if (image.isNull())
        return result;
    if (static_cast<qint64>(image.width()) * image.height() > kClipMaxPixels) {
        result.tooBig = true;
        return result;
    }
    QBuffer buffer(&result.data);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    if (result.data.size() > kClipMaxImageBytes) {
        result.data.clear();
        result.tooBig = true;
        return result;
    }
    result.firstFrame = image;
    result.hash = imagePixelHash(image);
    return result;
}

DecodedClipImage decodeClipImage(QByteArray data, bool hashed)
{
    DecodedClipImage result;
    if (data.size() > kClipMaxImageBytes) {
        result.tooBig = true;
        return result;
    }
    if (data.isEmpty())
        return result;
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const QByteArray format = reader.format();
    if (format == "gif")
        result.format = ClipImageFormat::Gif;
    else if (format == "jpeg" || format == "jpg")
        result.format = ClipImageFormat::Jpeg;
    else if (format != "png" && format != "bmp")
        return result;
    const QSize size = reader.size();
    if (static_cast<qint64>(size.width()) * size.height() > kClipMaxPixels) {
        result.tooBig = true;
        return result;
    }
    result.animated = reader.supportsAnimation() && reader.imageCount() != 1;
    result.firstFrame = reader.read();
    if (result.firstFrame.isNull())
        return {};
    if (format == "bmp")
        return encodeClipImage(result.firstFrame);
    if (!hashed) {
        // nothing to tell it from the others by
    } else if (result.animated) {
        QCryptographicHash hash(QCryptographicHash::Sha1);
        hash.addData(QByteArrayView("WinShun.animated-image"));
        hash.addData(data);
        result.hash = hash.result();
    } else {
        result.hash = imagePixelHash(result.firstFrame);
    }
    result.data = std::move(data);
    return result;
}

QByteArray imagePixelHash(const QImage& image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const qint32 size[] {argb.width(), argb.height()};
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(size), sizeof size));
    for (int y = 0; y < argb.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(argb.constScanLine(y)), argb.width() * 4));
    return hash.result();
}

bool dibRowsReady(const QImage& image)
{
    const QImage::Format format = image.format();
    return format == QImage::Format_RGB32 || format == QImage::Format_ARGB32 || format == QImage::Format_ARGB32_Premultiplied;
}

std::size_t dibRowsSize(const QImage& image)
{
    return static_cast<std::size_t>(image.width()) * 4 * static_cast<std::size_t>(image.height());
}

void writeDibRows(const QImage& image, uchar* target)
{
    Q_ASSERT(dibRowsReady(image));
    const std::size_t row = static_cast<std::size_t>(image.width()) * 4;
    const bool premultiplied = image.format() == QImage::Format_ARGB32_Premultiplied;
    for (int y = image.height() - 1; y >= 0; --y, target += row) {
        if (!premultiplied) {
            std::memcpy(target, image.constScanLine(y), row);
            continue;
        }
        // Opaque pixels, a screenshot's all, are the same either way.
        const auto* from = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = qAlpha(from[x]) == 255 ? from[x] : qUnpremultiply(from[x]);
            std::memcpy(target + static_cast<std::size_t>(x) * 4, &pixel, sizeof pixel);
        }
    }
}

// A DIB (CF_DIB, CF_DIBV5) is a BMP file without its 14-byte file header:
// give it one and Qt reads it, top-down, bit fields, alpha and all.
QImage imageFromDib(const QByteArray& dib)
{
    if (dib.size() < static_cast<qsizetype>(sizeof(BITMAPINFOHEADER)))
        return {};
    BITMAPINFOHEADER info;
    std::memcpy(&info, dib.constData(), sizeof info);
    if (info.biSize < sizeof(BITMAPINFOHEADER) || info.biWidth <= 0 || info.biHeight == 0)
        return {};
    if (static_cast<qint64>(info.biWidth) * std::abs(static_cast<qint64>(info.biHeight)) > kClipMaxPixels)
        return {};
    qint64 colors = info.biClrUsed;
    if (colors == 0 && info.biBitCount <= 8)
        colors = 1ll << info.biBitCount;
    qint64 offset = info.biSize + colors * 4;
    // Masks after a plain header with BI_BITFIELDS; some programs leave them
    // after a V5 header too, where they are already inside it.
    const qint64 stride = ((static_cast<qint64>(info.biWidth) * info.biBitCount + 31) / 32) * 4;
    const qint64 pixels = stride * std::abs(static_cast<qint64>(info.biHeight));
    if (info.biCompression == BI_BITFIELDS
        && (info.biSize == sizeof(BITMAPINFOHEADER) || dib.size() - offset == pixels + 12))
        offset += 12;
    BITMAPFILEHEADER file {};
    file.bfType = 0x4D42; // "BM"
    file.bfSize = static_cast<DWORD>(sizeof file + dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof file + offset);
    QByteArray bmp(reinterpret_cast<const char*>(&file), sizeof file);
    bmp += dib;
    return QImage::fromData(bmp, "BMP");
}

QImage imageFromEmf(const QByteArray& bits, double times, bool whole)
{
    const double kScale = times * 96 / 2540.0; // from hundredths of a millimetre (rclFrame)
    constexpr double kMaxSide = 4096; // pixels (64 MB drawn at most): a bigger frame is drawn smaller
    const int kMargin = static_cast<int>(4 * times + 0.5); // pixels of white kept around what it draws
    if (bits.size() < static_cast<qsizetype>(sizeof(ENHMETAHEADER)))
        return {};
    const HENHMETAFILE emf
        = ::SetEnhMetaFileBits(static_cast<UINT>(bits.size()), reinterpret_cast<const BYTE*>(bits.constData()));
    if (!emf)
        return {};
    ENHMETAHEADER header {};
    ::GetEnhMetaFileHeader(emf, sizeof header, &header);
    const double frameWidth = header.rclFrame.right - header.rclFrame.left;
    const double frameHeight = header.rclFrame.bottom - header.rclFrame.top;
    QImage image;
    if (frameWidth > 0 && frameHeight > 0 && (!whole || std::max(frameWidth, frameHeight) * kScale <= kMaxSide)) {
        const double scale = std::min(kScale, kMaxSide / std::max(frameWidth, frameHeight));
        const int width = std::max(1, static_cast<int>(frameWidth * scale + 0.5));
        const int height = std::max(1, static_cast<int>(frameHeight * scale + 0.5));
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // top-down, as QImage's rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        const HDC dc = ::CreateCompatibleDC(nullptr);
        const HBITMAP bitmap = dc ? ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
        if (bitmap && pixels) {
            const HGDIOBJ old = ::SelectObject(dc, bitmap);
            const RECT area {0, 0, width, height};
            // On white: an equation is black, and the dark theme shows
            // pictures on dark grey.
            ::FillRect(dc, &area, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
            ::PlayEnhMetaFile(dc, emf, &area);
            ::GdiFlush();
            image = QImage(static_cast<const uchar*>(pixels), width, height, width * 4, QImage::Format_RGB32).copy();
            ::SelectObject(dc, old);
        }
        if (bitmap)
            ::DeleteObject(bitmap);
        if (dc)
            ::DeleteDC(dc);
    }
    ::DeleteEnhMetaFile(emf);
    if (image.isNull())
        return {};
    // GDI leaves the fourth byte 0; RGB32 wants it 0xFF. And what it draws
    // is all there is to show: a display equation's frame is as wide as the page.
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            row[x] |= 0xFF000000;
            if ((row[x] & 0xFFFFFF) != 0xFFFFFF) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < 0)
        return {}; // nothing drawn
    return image.copy(
        QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-kMargin, -kMargin, kMargin, kMargin) & image.rect());
}

} // namespace ws
