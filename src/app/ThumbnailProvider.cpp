#include "ThumbnailProvider.h"

#include "FileIconProvider.h"

#include <QCache>
#include <QFileInfo>
#include <QImage>
#include <QMutex>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>

#include <windows.h>

#include <algorithm>
#include <atomic>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// What is kept between requests. Never destroyed: at exit a thread may still
// be inside a shell handler, and nothing should wait for it.
struct Store {
    QMutex mutex;
    // 48 MB: about 5000 thumbnails at 150 % (48 px, 9 KB each), 3000 at 200 %.
    QCache<QString, QImage> images {48 * 1024 * 1024};
    QCache<QString, bool> misses {20000}; // no thumbnail (or not to be made)
    QThreadPool pool;
    std::atomic<int> order {0}; // the newest request runs first
};

Store& store()
{
    static Store* s = [] {
        auto* made = new Store;
        // Few: the cache answers at once; making one reads the file and
        // decodes it. Below normal, so a screenful being made never makes
        // the window stutter.
        made->pool.setMaxThreadCount(4);
        made->pool.setThreadPriority(QThread::LowPriority);
        return made;
    }();
    return *s;
}

QString encode(const QString& s)
{
    return QString::fromLatin1(s.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString decode(QStringView s)
{
    return QString::fromUtf8(QByteArray::fromBase64(s.toLatin1(), QByteArray::Base64UrlEncoding));
}

bool isVideo(const QString& suffix)
{
    static const QStringList kTypes {u"mp4"_s, u"m4v"_s, u"mov"_s, u"avi"_s, u"wmv"_s, u"mkv"_s, u"webm"_s, u"mpg"_s,
        u"mpeg"_s, u"m2ts"_s, u"mts"_s, u"ts"_s, u"3gp"_s, u"flv"_s};
    return kTypes.contains(suffix);
}

class Response : public QQuickImageResponse {
public:
    QQuickTextureFactory* textureFactory() const override
    {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    void cancel() override { m_cancelled.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return m_cancelled.load(std::memory_order_relaxed); }

    // Always, once, even when cancelled: Qt deletes the response only after this.
    void finish(QImage image)
    {
        m_image = std::move(image);
        emit finished();
    }

private:
    QImage m_image;
    std::atomic<bool> m_cancelled {false};
};

// Pictures larger than this are only taken from the cache: making a
// thumbnail of a 2 GB TIFF reads all of it. Videos are read in part.
constexpr qint64 kLargestMade = 512LL * 1024 * 1024;

// On a pool thread.
QImage makeThumbnail(const QString& path, int px, Response* response, bool* known)
{
    *known = true; // what is found out here holds until the file is written again
    // A network drive, before anything touches the file: as slow as a server
    // that went away (UNC paths have no address at all).
    if (path.size() >= 2 && path[1] == u':') {
        const wchar_t root[] = {static_cast<wchar_t>(path[0].unicode()), L':', L'\\', 0};
        if (::GetDriveTypeW(root) == DRIVE_REMOTE)
            return {};
    }
    const std::wstring native = path.toStdWString();
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data))
        return {};
    // A cloud file not on this PC (OneDrive's "online only"): a thumbnail
    // would download it.
    constexpr DWORD kNotHere = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (data.dwFileAttributes & (kNotHere | FILE_ATTRIBUTE_DIRECTORY))
        return {};
    QImage image = FileIconProvider::thumbnail(path, px, true);
    // The compact list may have cached a 32px preview. It is not a usable
    // large preview: ask the handler for the new size instead of enlarging it.
    if (!image.isNull() && std::max(image.width(), image.height()) >= px)
        return image;
    if (response->cancelled()) { // scrolled away meanwhile: not known yet
        *known = false;
        return {};
    }
    const qint64 bytes = (static_cast<qint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (bytes > kLargestMade && !isVideo(QFileInfo(path).suffix().toLower()))
        return image;
    QImage made = FileIconProvider::thumbnail(path, px, false);
    return made.isNull() ? image : made;
}

} // namespace

ThumbnailProvider::ThumbnailProvider() = default;

bool ThumbnailProvider::hasThumbnailType(const QString& path)
{
    // Types Windows (or a common add-on: HEIF, RAW, WebP image extensions,
    // a PDF reader) makes thumbnails of. Others are not asked about.
    static const QStringList kTypes {
        // pictures
        u"jpg"_s, u"jpeg"_s, u"jpe"_s, u"jfif"_s, u"png"_s, u"gif"_s, u"bmp"_s, u"dib"_s, u"tif"_s, u"tiff"_s,
        u"webp"_s, u"heic"_s, u"heif"_s, u"avif"_s, u"jxl"_s, u"jxr"_s, u"wdp"_s, u"psd"_s,
        // cameras' raw files
        u"dng"_s, u"cr2"_s, u"cr3"_s, u"nef"_s, u"arw"_s, u"orf"_s, u"rw2"_s, u"raf"_s,
        // documents
        u"pdf"_s};
    const qsizetype dot = path.lastIndexOf(u'.');
    if (dot < 0 || path.size() - dot > 6 || path.indexOf(u'\\', dot) >= 0)
        return false;
    const QString suffix = path.mid(dot + 1).toLower();
    return kTypes.contains(suffix) || isVideo(suffix);
}

QString ThumbnailProvider::url(const QString& path, std::uint32_t modified)
{
    if (path.startsWith(u"\\\\") || !hasThumbnailType(path))
        return {};
    return u"image://thumbnail/"_s + QString::number(modified) + u'/' + encode(path);
}

// "<modified>/<path>"
QQuickImageResponse* ThumbnailProvider::requestImageResponse(const QString& id, const QSize& requestedSize)
{
    auto* response = new Response;
    const qsizetype slash = id.indexOf(u'/');
    const QString path = slash > 0 ? decode(QStringView(id).mid(slash + 1)) : QString();
    if (path.isEmpty()) {
        response->finish({});
        return response;
    }
    const int asked = std::max(requestedSize.width(), requestedSize.height());
    const int px = asked > 0 ? std::clamp(asked, 16, 1024) : 64;
    const QString key = id + u'@' + QString::number(px);

    Store& s = store();
    {
        QMutexLocker lock(&s.mutex);
        if (const QImage* kept = s.images.object(key)) {
            response->finish(*kept); // Qt picks this up although it came before it listened
            return response;
        }
        if (s.misses.contains(key)) {
            response->finish({});
            return response;
        }
    }
    s.pool.start(QRunnable::create([response, path, px, key] {
                     if (response->cancelled()) {
                         response->finish({});
                         return;
                     }
                     bool known = false;
                     QImage image = makeThumbnail(path, px, response, &known);
                     if (known) {
                         Store& s = store();
                         QMutexLocker lock(&s.mutex);
                         if (image.isNull())
                             s.misses.insert(key, new bool(true));
                         else
                             s.images.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes()));
                     }
                     response->finish(std::move(image));
                 }),
        s.order.fetch_add(1, std::memory_order_relaxed));
    return response;
}

} // namespace ws
