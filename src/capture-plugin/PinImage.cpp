#include "PinImage.h"

#include "ClipImage.h"
#include "ImageOrigin.h"
#include "PictureFile.h"

#include <QScopeGuard>
#include <QStringList>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace ws::captureplugin::pinimage {

namespace {

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
    const QSize room = placement.work.size() * 0.9;
    if (image.isNull() || room.isEmpty())
        return {};
    if (image.width() > room.width() || image.height() > room.height())
        image = image.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    QPoint origin = placement.cursor - QPoint(image.width() / 2, image.height() / 2);
    origin.setX(std::clamp(origin.x(), placement.work.left(), (std::max)(placement.work.left(), placement.work.right() + 1 - image.width())));
    origin.setY(std::clamp(origin.y(), placement.work.top(), (std::max)(placement.work.top(), placement.work.bottom() + 1 - image.height())));
    return {std::move(image), origin};
}

QImage readFile(const QString& path, const Placement& placement)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    const auto uninit = qScopeGuard([com] {
        if (SUCCEEDED(com))
            ::CoUninitialize();
    });
    // Let WIC downsample while decoding; the final fit also handles narrow,
    // tall pictures and converts their pixels, still on this worker.
    const QSize room = placement.work.size() * 0.9;
    return decodePictureFile(path, (std::max)(room.width(), room.height()));
}

ClipboardPicture readClipboard(const LastCapture& last, const Placement& placement)
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
        if (QImage image = readFile(file, placement); !image.isNull())
            return {ClipboardStatus::Image, std::move(image)};
    }
    return {offeredImage ? ClipboardStatus::Unreadable : ClipboardStatus::Empty, {}};
}

} // namespace ws::captureplugin::pinimage
