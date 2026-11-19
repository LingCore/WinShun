#include "ClipThumbnailProvider.h"

#include "PictureFile.h"
#include "PooledImageResponse.h"

#include <QCache>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QMutex>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QUrl>

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// What is kept between requests. Never destroyed: at exit a thread may still
// be decoding, and nothing should wait for it.
struct Store {
    QMutex mutex;
    // 32 MB: about 480 pictures at 150 % (162 x 102 px, 66 KB each), 130
    // large ones (312 x 195). Rows scrolled back to come from here.
    QCache<QString, QImage> images {32 * 1024 * 1024};
    QThreadPool pool;
    std::atomic<int> order {0}; // the newest request runs first
};

Store& store()
{
    static Store* s = [] {
        auto* made = new Store;
        // A screenful at once; below normal, so decoding never makes the
        // window stutter (as ThumbnailProvider).
        made->pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 2, 2, 4));
        made->pool.setThreadPriority(QThread::LowPriority);
        return made;
    }();
    return *s;
}

// The size Qt reads a file at for a sourceSize (QQuickImageProviderWithOptions::
// loadSize, without a fill mode): shrunk to fit within it, the aspect kept,
// never enlarged.
QSize fitted(QSize original, QSize requested)
{
    if (original.isEmpty() || (requested.width() <= 0 && requested.height() <= 0))
        return {};
    qreal ratio = 0;
    if (requested.width() > 0 && requested.width() < original.width())
        ratio = qreal(requested.width()) / original.width();
    if (requested.height() > 0 && requested.height() < original.height()) {
        const qreal height = qreal(requested.height()) / original.height();
        ratio = ratio == 0 ? height : std::min(ratio, height);
    }
    if (ratio <= 0)
        return {};
    return {qRound(original.width() * ratio), qRound(original.height() * ratio)};
}

// COM for Windows Imaging Component, once on each pool thread.
struct ComApartment {
    ComApartment()
        : hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }
    ~ComApartment()
    {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
    HRESULT hr;
};

// On a pool thread. Qt's decoders first; what they cannot read (Win顺 ships
// only Qt's GIF and JPEG ones: WebP, TIFF, HEIC, AVIF, an icon ...) by
// Windows' own, as the preview beside the list shows it (GitHub #373).
QImage decode(const QString& path, QSize requested, QString* error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true); // as the row's Image (autoTransform)
    if (const QSize size = fitted(reader.size(), requested); size.isValid())
        reader.setScaledSize(size);
    QImage image;
    if (reader.read(&image))
        return image;
    thread_local const ComApartment com;
    image = decodePictureFile(path, requested);
    if (image.isNull())
        *error = u"Error decoding: %1: %2"_s.arg(QUrl::fromLocalFile(path).toString(), reader.errorString());
    return image;
}

} // namespace

ClipThumbnailProvider::ClipThumbnailProvider()
    : QQuickAsyncImageProvider()
{
}

qint64 ClipThumbnailProvider::cachedBytes()
{
    Store& s = store();
    QMutexLocker lock(&s.mutex);
    return s.images.totalCost();
}

QString ClipThumbnailProvider::url(const QString& path, qint64 stamp)
{
    // One address for a file however its path is written (C:\a or C:/a).
    const QByteArray bytes = QDir::fromNativeSeparators(path).toUtf8().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return u"image://clipthumb/"_s + QString::number(stamp) + u'/' + QString::fromLatin1(bytes);
}

// "<stamp>/<path>"
QQuickImageResponse* ClipThumbnailProvider::requestImageResponse(const QString& id, const QSize& requestedSize)
{
    auto* response = new PooledImageResponse;
    const qsizetype slash = id.indexOf(u'/');
    const QString path = slash > 0
        ? QString::fromUtf8(QByteArray::fromBase64(QStringView(id).mid(slash + 1).toLatin1(), QByteArray::Base64UrlEncoding))
        : QString();
    if (path.isEmpty()) {
        response->finish({}, u"No picture"_s);
        return response;
    }
    const QString key = id + u'@' + QString::number(requestedSize.width()) + u'x' + QString::number(requestedSize.height());
    Store& s = store();
    {
        QMutexLocker lock(&s.mutex);
        if (const QImage* kept = s.images.object(key)) {
            response->finish(*kept); // Qt picks this up although it came before it listened
            return response;
        }
    }
    s.pool.start(QRunnable::create([response = response->handle(), path, requestedSize, key] {
                     if (response.cancelled()) {
                         response.finish({});
                         return;
                     }
                     QString error;
                     QImage image = decode(path, requestedSize, &error);
                     if (!image.isNull()) {
                         Store& s = store();
                         QMutexLocker lock(&s.mutex);
                         s.images.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes()));
                     }
                     response.finish(std::move(image), std::move(error));
                 }),
        s.order.fetch_add(1, std::memory_order_relaxed));
    return response;
}

} // namespace ws
