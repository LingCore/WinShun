#include "ThumbnailProvider.h"

#include "FileIconProvider.h"
#include "PooledImageResponse.h"

#include <QCache>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <string>

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

// Pictures larger than this are only taken from the cache: making a
// thumbnail of a 2 GB TIFF reads all of it. Videos are read in part.
constexpr qint64 kLargestMade = 512LL * 1024 * 1024;

// On a pool thread.
QImage makeThumbnail(const QString& path, int px, const PooledImageResponse::Handle& response, bool* known)
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
    if (response.cancelled()) { // scrolled away meanwhile: not known yet
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

namespace {

// Whether a program installed on this PC makes thumbnails of the type: its
// thumbnail handler is registered for the extension (CorelDRAW's for .cdr,
// a slicer's for .stl and .3mf, a CAD program's, PowerToys' for .svg), by
// IThumbnailProvider or the older IExtractImage. Read from the registry, as
// Explorer finds it (the extension, its program's type, SystemFileAssociations).
bool hasHandler(const std::wstring& extension)
{
    for (const wchar_t* interfaceId : {L"{e357fccd-a995-4576-b01f-234630154e96}", L"{BB2E617C-0920-11d1-9A0B-00C04FC2D6C1}"}) {
        wchar_t handler[64];
        DWORD size = static_cast<DWORD>(std::size(handler));
        if (::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION, extension.c_str(), interfaceId, handler, &size) == S_OK)
            return true;
    }
    return false;
}

// A type outside the list below, shown by its thumbnail when a program here
// makes one. Not the kinds whose icon says more in a row than a small white
// page does (Word, Excel and PowerPoint files have handlers too: text,
// documents), nor music (a cover is no file's picture), nor what stands for
// something else (a shortcut's handler gives its target's, a library's its
// folders'). Asked once per extension: a program installed since is seen
// after a restart.
bool handlerType(const QString& suffix)
{
    static const QStringList kNever {u"lnk"_s, u"url"_s, u"theme"_s, u"themepack"_s, u"library-ms"_s, u"search-ms"_s};
    if (kNever.contains(suffix))
        return false;
    static QMutex mutex;
    static QHash<QString, bool> known;
    {
        QMutexLocker lock(&mutex);
        if (const auto it = known.constFind(suffix); it != known.cend())
            return *it;
    }
    const std::wstring extension = L'.' + suffix.toStdWString();
    PERCEIVED kind = PERCEIVED_TYPE_UNSPECIFIED;
    PERCEIVEDFLAG flags = PERCEIVEDFLAG_UNDEFINED;
    if (FAILED(::AssocGetPerceivedType(extension.c_str(), &kind, &flags, nullptr)))
        kind = PERCEIVED_TYPE_UNSPECIFIED;
    const bool pictured = kind == PERCEIVED_TYPE_IMAGE || kind == PERCEIVED_TYPE_VIDEO || kind == PERCEIVED_TYPE_UNSPECIFIED
        || kind == PERCEIVED_TYPE_CUSTOM || kind == PERCEIVED_TYPE_UNKNOWN;
    const bool has = pictured && hasHandler(extension);
    QMutexLocker lock(&mutex);
    known.insert(suffix, has);
    return has;
}

} // namespace

bool ThumbnailProvider::hasThumbnailType(const QString& path)
{
    // Types Windows (or a common add-on: HEIF, RAW, WebP image extensions,
    // a PDF reader) makes thumbnails of: asked about wherever they are. Any
    // other type only when a program on this PC makes its thumbnails
    // (handlerType): the rest are not asked about.
    static const QStringList kTypes {
        // pictures
        u"jpg"_s, u"jpeg"_s, u"jpe"_s, u"jfif"_s, u"png"_s, u"gif"_s, u"bmp"_s, u"dib"_s, u"tif"_s, u"tiff"_s,
        u"webp"_s, u"heic"_s, u"heif"_s, u"avif"_s, u"jxl"_s, u"jxr"_s, u"wdp"_s, u"psd"_s,
        // cameras' raw files
        u"dng"_s, u"cr2"_s, u"cr3"_s, u"nef"_s, u"arw"_s, u"orf"_s, u"rw2"_s, u"raf"_s,
        // documents
        u"pdf"_s};
    constexpr qsizetype kLongestSuffix = 12; // "afdesign", "sldprt": a longer last part is no type with thumbnails
    const qsizetype dot = path.lastIndexOf(u'.');
    if (dot < 0 || dot + 1 == path.size() || path.size() - dot - 1 > kLongestSuffix || path.indexOf(u'\\', dot) >= 0)
        return false;
    const QString suffix = path.mid(dot + 1).toLower();
    if (kTypes.contains(suffix) || isVideo(suffix))
        return true;
    // A name's last part with a space or the like is no extension ("v1.2 final").
    if (!std::ranges::all_of(suffix, [](QChar c) { return c.isLetterOrNumber() || c == u'-' || c == u'_'; }))
        return false;
    return handlerType(suffix);
}

qint64 ThumbnailProvider::cachedBytes()
{
    Store& s = store();
    QMutexLocker lock(&s.mutex);
    return s.images.totalCost();
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
    auto* response = new PooledImageResponse;
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
    s.pool.start(QRunnable::create([response = response->handle(), path, px, key] {
                     if (response.cancelled()) {
                         response.finish({});
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
                     response.finish(std::move(image));
                 }),
        s.order.fetch_add(1, std::memory_order_relaxed));
    return response;
}

} // namespace ws
