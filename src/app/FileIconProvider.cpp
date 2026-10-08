#include "FileIconProvider.h"

#include "AppLogo.h"

#include <QColor>
#include <QFileInfo>
#include <QPainter>

#include <windows.h>
// commctrl.h must precede commoncontrols.h (IImageList types).
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws {

namespace {

// Types whose icon depends on the file itself, not just the extension.
bool hasOwnIcon(const QString& suffix)
{
    static const QStringList kTypes {
        u"exe"_s, u"lnk"_s, u"ico"_s, u"url"_s, u"msc"_s, u"cpl"_s, u"scr"_s, u"appref-ms"_s, u"cur"_s, u"ani"_s};
    return kTypes.contains(suffix, Qt::CaseInsensitive);
}

QString encode(const QString& s)
{
    return QString::fromLatin1(s.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString decode(QStringView s)
{
    return QString::fromUtf8(QByteArray::fromBase64(s.toLatin1(), QByteArray::Base64UrlEncoding));
}

struct ComApartment {
    ComApartment()
        : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {
    }
    ~ComApartment()
    {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
    HRESULT hr;
};

QImage fitTo(QImage image, int size)
{
    if (!image.isNull() && (image.width() != size || image.height() != size))
        image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

int imageListSize(int which)
{
    ComPtr<IImageList> list;
    int width = 0;
    int height = 0;
    if (FAILED(::SHGetImageList(which, IID_PPV_ARGS(&list))) || FAILED(list->GetIconSize(&width, &height)))
        return 0;
    return width;
}

QImage imageListIcon(int which, int index)
{
    ComPtr<IImageList> list;
    HICON icon = nullptr;
    if (FAILED(::SHGetImageList(which, IID_PPV_ARGS(&list))) || FAILED(list->GetIcon(index, ILD_TRANSPARENT, &icon))
        || !icon)
        return {};
    QImage image = QImage::fromHICON(icon);
    ::DestroyIcon(icon);
    return image;
}

// A file type without a 256 px picture comes out of the jumbo list as its
// 32 or 48 px one, unscaled, in the corner of an empty 256 px square.
bool onlyInCorner(const QImage& image, int corner)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        for (int x = y < corner ? corner : 0; x < argb.width(); ++x) {
            if (qAlpha(line[x]) != 0)
                return false;
        }
    }
    return true;
}

// An icon from the system image lists, which hold one size each: the
// smallest list at least `size` big, so the picture is only ever scaled down.
// (Their sizes follow the system DPI: SHIL_SMALL is 16 px at 100 %, 24 at 150 %.)
QImage shellIcon(const std::wstring& target, DWORD attributes, bool byAttributes, int size)
{
    SHFILEINFOW info {};
    UINT flags = SHGFI_SYSICONINDEX;
    if (byAttributes)
        flags |= SHGFI_USEFILEATTRIBUTES;
    if (!::SHGetFileInfoW(target.c_str(), attributes, &info, sizeof info, flags))
        return {};
    for (const int which : {SHIL_SMALL, SHIL_LARGE, SHIL_EXTRALARGE, SHIL_JUMBO}) {
        if (which != SHIL_JUMBO && imageListSize(which) < size)
            continue;
        QImage image = imageListIcon(which, info.iIcon);
        if (which == SHIL_JUMBO && (image.isNull() || onlyInCorner(image, 64)))
            image = imageListIcon(SHIL_EXTRALARGE, info.iIcon);
        return fitTo(image, size);
    }
    return {};
}

// A 32-bit DIB from IShellItemImageFactory. Icons come with straight alpha
// (thumbnails may be premultiplied): a colour channel above its pixel's alpha
// can only be straight, so that decides. Taken as premultiplied, straight
// colours overflow in blending and fringe the icon's edges with dark pixels.
QImage fromShellBitmap(HBITMAP bitmap)
{
    BITMAP bm {};
    if (!::GetObjectW(bitmap, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return {};
    QImage image(bm.bmWidth, bm.bmHeight, QImage::Format_ARGB32);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bm.bmWidth;
    info.bmiHeader.biHeight = -bm.bmHeight; // top-down, like QImage
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const HDC dc = ::GetDC(nullptr);
    const int lines = ::GetDIBits(dc, bitmap, 0, static_cast<UINT>(bm.bmHeight), image.bits(), &info, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    if (lines != bm.bmHeight)
        return {};
    auto* pixels = reinterpret_cast<QRgb*>(image.bits());
    const qsizetype count = static_cast<qsizetype>(image.width()) * image.height();
    if (std::none_of(pixels, pixels + count, [](QRgb p) { return qAlpha(p) != 0; })) {
        // No alpha at all means an opaque image, not an invisible one.
        std::for_each(pixels, pixels + count, [](QRgb& p) { p |= 0xFF000000u; });
        return image;
    }
    const bool straight = std::any_of(pixels, pixels + count, [](QRgb p) {
        return std::max({qRed(p), qGreen(p), qBlue(p)}) > qAlpha(p);
    });
    if (straight)
        return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
    return image;
}

// A shell item's icon at exactly `size` pixels, which Windows draws from the
// icon's nearest size (an .exe usually has 16 to 256 px pictures). No
// overlays such as the shortcut arrow.
QImage shellItemIcon(const std::wstring& parsingName, int size)
{
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(::SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(&factory))))
        return {};
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage({size, size}, SIIGBF_ICONONLY, &bitmap)) || !bitmap)
        return {};
    QImage image = fromShellBitmap(bitmap);
    ::DeleteObject(bitmap);
    return fitTo(image, size);
}

// A packaged app's logo file for this size and theme (see AppLogo.h). Logos
// made to sit on a plate get it: a rounded square in the app's colour.
QImage packagedAppLogo(const QString& appId, int size, bool dark)
{
    const std::optional<AppLogo> logo = findAppLogo(appId, size, dark);
    if (!logo)
        return {};
    QImage image(logo->path);
    if (image.isNull())
        return {};
    image = fitTo(image.convertToFormat(QImage::Format_ARGB32_Premultiplied), size);
    const QColor plate = QColor::fromString(logo->background);
    if (!logo->plated || !plate.isValid() || plate.alpha() == 0)
        return image;
    QImage plated(size, size, QImage::Format_ARGB32_Premultiplied);
    plated.fill(Qt::transparent);
    QPainter painter(&plated);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(plate);
    painter.drawRoundedRect(QRectF(0, 0, size, size), size / 8.0, size / 8.0);
    painter.drawImage((size - image.width()) / 2, (size - image.height()) / 2, image);
    painter.end();
    return plated;
}

} // namespace

FileIconProvider::FileIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading)
{
}

QString FileIconProvider::iconUrl(const QString& path, bool isDir)
{
    if (isDir)
        return u"image://fileicon/folder"_s;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (hasOwnIcon(suffix))
        return u"image://fileicon/file/"_s + encode(path);
    return u"image://fileicon/ext/"_s + encode(suffix);
}

QString FileIconProvider::appIconUrl(const QString& appId, bool packaged, bool dark)
{
    const QString url = u"image://fileicon/app/"_s + encode(appId);
    if (!packaged)
        return url;
    return url + (dark ? u"/dark"_s : u"/light"_s); // its logo has a variant per theme
}

QImage FileIconProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    thread_local ComApartment com; // SHGetFileInfo requires COM on the calling thread

    const int px = requestedSize.width() > 0 ? std::clamp(requestedSize.width(), 16, 256) : 32;
    const QString key = id + u'@' + QString::number(px);
    {
        QMutexLocker lock(&m_mutex);
        if (const QImage* cached = m_cache.object(key)) {
            *size = cached->size();
            return *cached;
        }
    }

    QImage image;
    if (id == u"folder") {
        image = shellIcon(L"folder", FILE_ATTRIBUTE_DIRECTORY, true, px);
    } else if (id.startsWith(u"ext/")) {
        const QString suffix = decode(QStringView(id).mid(4));
        image = shellIcon((u"file."_s + suffix).toStdWString(), FILE_ATTRIBUTE_NORMAL, true, px);
    } else if (id.startsWith(u"file/")) {
        const std::wstring path = decode(QStringView(id).mid(5)).toStdWString();
        image = shellItemIcon(path, px);
        if (image.isNull())
            image = shellIcon(path, 0, false, px);
        if (image.isNull()) // file vanished or unreadable: fall back to the type icon
            image = shellIcon(L"file.exe", FILE_ATTRIBUTE_NORMAL, true, px);
    } else if (id.startsWith(u"app/")) {
        // "app/<id>" for a desktop program, "app/<id>/dark" or ".../light" for a package.
        const QStringList parts = id.mid(4).split(u'/');
        const QString appId = decode(parts.value(0));
        if (parts.size() > 1)
            image = packagedAppLogo(appId, px, parts.value(1) == u"dark");
        if (image.isNull()) // a desktop program, or a package whose files cannot be read
            image = shellItemIcon((u"shell:AppsFolder\\"_s + appId).toStdWString(), px);
        if (image.isNull()) // uninstalled meanwhile
            image = shellIcon(L"file.exe", FILE_ATTRIBUTE_NORMAL, true, px);
    }
    if (image.isNull())
        image = shellIcon(L"file", FILE_ATTRIBUTE_NORMAL, true, px);

    *size = image.size();
    QMutexLocker lock(&m_mutex);
    m_cache.insert(key, new QImage(image));
    return image;
}

} // namespace ws
