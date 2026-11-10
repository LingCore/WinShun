#include "PinImage.h"

#include "ClipImage.h"
#include "ImageOrigin.h"
#include "PictureFile.h"

#include <QScopeGuard>
#include <QStringList>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <string>
#include <thread>

namespace ws::captureplugin::pinimage {

namespace {

// The pixels a pin's picture keeps: the long capture's own limit
// (ScrollStitcher::Limits), so what it stitches pins at its own resolution.
constexpr qint64 kPinSourcePixels = 60'000'000;

// The first frame's size as stored, from its header only. Windows Imaging
// Component, as decodePictureFile: dist has no Qt image plugins, so
// QImageReader knows PNG and BMP sizes but not a JPEG's.
QSize storedSize(const QString& path)
{
    QString native = path;
    native.replace(u'/', u'\\');
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))
        || FAILED(wic->CreateDecoderFromFilename(
            native.toStdWString().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height))
        || width > INT_MAX || height > INT_MAX)
        return {};
    return QSize(static_cast<int>(width), static_cast<int>(height));
}

QByteArray clipboardBytes(UINT format, SIZE_T limit)
{
    const HANDLE handle = ::GetClipboardData(format);
    if (!handle)
        return {};
    const SIZE_T size = ::GlobalSize(handle);
    if (!size || size > limit)
        return {};
    const void* data = ::GlobalLock(handle);
    if (!data)
        return {};
    const QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(size));
    ::GlobalUnlock(handle);
    return bytes;
}

} // namespace

std::optional<Placement> cursorPlacement()
{
    POINT cursor {};
    if (!::GetCursorPos(&cursor))
        return std::nullopt;
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info))
        return std::nullopt;
    return Placement {{cursor.x, cursor.y},
        QRect(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top)};
}

Prepared prepare(QImage image, const Placement& placement)
{
    if (image.isNull() || placement.work.isEmpty())
        return {};
    image = image.convertToFormat(QImage::Format_RGB32);
    if (image.isNull())
        return {};
    // Its own pixels stay: the pin shows a picture larger than the screen
    // fitted (PinManager::pinFitted), so a zoom still has the detail. This
    // is where the whole picture would start, its middle at the cursor; the
    // pin fits the shown picture about that middle and keeps it in the area.
    const QPoint origin = placement.cursor - QPoint(image.width() / 2, image.height() / 2);
    return {std::move(image), origin, placement.cursor};
}

QImage readFile(const QString& path)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    const auto uninit = qScopeGuard([com] {
        if (SUCCEEDED(com))
            ::CoUninitialize();
    });
    // Read at its own size, not the screen's: a long picture shown fitted and
    // zoomed in had nothing left to show. Only one past the pixels a pin is
    // worth keeping is read smaller, so it can still be read at all.
    int largest = 0;
    if (const QSize stored = storedSize(path);
        !stored.isEmpty() && qint64(stored.width()) * stored.height() > kPinSourcePixels) {
        const double scale = std::sqrt(static_cast<double>(kPinSourcePixels) / (qint64(stored.width()) * stored.height()));
        largest = static_cast<int>(std::lround((std::max)(stored.width(), stored.height()) * scale));
    }
    return decodePictureFile(path, largest);
}

ClipboardPicture readClipboard(const LastCapture& last)
{
    bool opened = false;
    for (int attempt = 0; attempt != 8 && !opened; ++attempt) {
        opened = ::OpenClipboard(nullptr) != FALSE;
        if (!opened)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!opened)
        return {ClipboardStatus::Busy, {}};
    auto close = qScopeGuard([] { ::CloseClipboard(); });
    const DWORD sequence = ::GetClipboardSequenceNumber();
    QByteArray origin = clipboardBytes(::RegisterClipboardFormatW(kImageOriginFormat), 128);
    if (const qsizetype end = origin.indexOf('\0'); end >= 0)
        origin.truncate(end);
    if (!last.id.isEmpty()
        && ((sequence != 0 && sequence == last.clipboardSequence) || origin == last.id.toUtf8()))
        return {ClipboardStatus::LastCapture, {}};

    // Keep the supported representations so a damaged rich offer can still
    // fall back to its bitmap. No decoding or disk access while holding the lock.
    QList<QByteArray> encoded;
    qsizetype totalBytes = 0;
    bool offeredImage = false;
    for (const ClipImageOffer& offer : clipImageOffers()) {
        const QString name = offer.name.toString();
        const UINT format = ::RegisterClipboardFormatW(qUtf16Printable(name));
        offeredImage = offeredImage || ::IsClipboardFormatAvailable(format);
        if (QByteArray bytes = clipboardBytes(format, static_cast<SIZE_T>(kClipMaxImageBytes - totalBytes)); !bytes.isEmpty()) {
            totalBytes += bytes.size();
            encoded.append(std::move(bytes));
        }
    }
    offeredImage = offeredImage || ::IsClipboardFormatAvailable(CF_DIBV5) || ::IsClipboardFormatAvailable(CF_DIB);
    const QByteArray dibV5 = clipboardBytes(CF_DIBV5, kClipMaxImageBytes);
    const QByteArray dib = clipboardBytes(CF_DIB, kClipMaxImageBytes);
    QStringList files;
    if (const HDROP drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP))) {
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
            if (!length || length > 32767)
                continue;
            std::wstring path(length + 1, L'\0');
            if (::DragQueryFileW(drop, i, path.data(), length + 1))
                files.append(QString::fromWCharArray(path.data(), static_cast<int>(length)));
        }
    }
    ::CloseClipboard();
    close.dismiss();

    for (QByteArray& bytes : encoded) {
        if (QImage image = decodeClipImage(std::move(bytes)).firstFrame; !image.isNull())
            return {ClipboardStatus::Image, std::move(image)};
    }
    if (QImage image = imageFromDib(dibV5); !image.isNull())
        return {ClipboardStatus::Image, std::move(image)};
    if (QImage image = imageFromDib(dib); !image.isNull())
        return {ClipboardStatus::Image, std::move(image)};
    for (const QString& file : files) {
        // Decoding, rather than a different codec's canRead(), chooses the
        // first readable file. The caller fits it to the saved work area.
        if (QImage image = readFile(file); !image.isNull())
            return {ClipboardStatus::Image, std::move(image)};
    }
    return {offeredImage ? ClipboardStatus::Unreadable : ClipboardStatus::Empty, {}};
}

} // namespace ws::captureplugin::pinimage
