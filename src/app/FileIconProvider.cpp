#include "FileIconProvider.h"

#include "AppLogo.h"
#include "Favorites.h"
#include "Items.h"
#include "Manifest.h"
#include "Phosphor.h"
#include "SiteIconImage.h"
#include "SystemTheme.h"
#include "SystemCatalog.h"
#include "WebShortcut.h"
#include "Win32Util.h"
#include "platform/Shell.h"
#include "platform/SiteIcons.h"

#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <windows.h>
// commctrl.h must precede commoncontrols.h (IImageList types).
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
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

// A file's thumbnail as Explorer shows it, at most `size` pixels on its
// longer side (never enlarged); null if the file has none. `cachedOnly`:
// only if Windows' thumbnail cache has it (no handler runs, nothing is read).
QImage shellThumbnail(const std::wstring& path, int size, bool cachedOnly = false)
{
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))))
        return {};
    HBITMAP bitmap = nullptr;
    const SIIGBF flags = SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK
        | (cachedOnly ? SIIGBF_INCACHEONLY : SIIGBF_RESIZETOFIT);
    if (FAILED(factory->GetImage({size, size}, flags, &bitmap)) || !bitmap)
        return {};
    QImage image = fromShellBitmap(bitmap);
    ::DeleteObject(bitmap);
    if (image.width() > size || image.height() > size)
        image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

// A place's icon (see placeIconUrl): "<module>,<index>", where a negative
// index is a resource id, else a file or shell path ("shell:startup").
QImage placeIcon(const QString& icon, int size)
{
    static const QRegularExpression inModule(u"^(.+),\\s*(-?\\d+)$"_s);
    const QRegularExpressionMatch m = inModule.match(icon);
    if (m.hasMatch()) {
        const std::wstring module = win32::expandEnvironment(m.captured(1).trimmed().toStdWString());
        HICON handle = nullptr;
        if (::SHDefExtractIconW(module.c_str(), m.captured(2).toInt(), 0, &handle, nullptr, static_cast<UINT>(size)) == S_OK
            && handle) {
            QImage image = QImage::fromHICON(handle);
            ::DestroyIcon(handle);
            return fitTo(image, size);
        }
        return {};
    }
    return shellItemIcon(win32::expandEnvironment(icon.toStdWString()), size);
}

// The program that opens links of a scheme ("https": the default browser,
// as the user chose it), else an Internet shortcut's icon.
QImage schemeIcon(const QString& scheme, int size)
{
    wchar_t program[MAX_PATH] {};
    DWORD length = MAX_PATH;
    if (::AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, reinterpret_cast<const wchar_t*>(scheme.utf16()),
            L"open", program, &length)
        == S_OK) {
        // A packaged browser's program may be unreadable: then the shortcut's.
        if (QImage image = shellItemIcon(program, size); !image.isNull())
            return image;
    }
    return shellIcon(L"file.url", FILE_ATTRIBUTE_NORMAL, true, size);
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

// ---- plugin icons (the plugin design, §4.5) ------------------------------------

QMutex g_pluginMutex;
QHash<QString, QString> g_pluginFolders; // plugin id -> its folder (native)
QHash<QString, QByteArray> g_pluginData; // sha1 -> the bytes of a data: icon
constexpr qsizetype kPluginDataKept = 64;

// "U+E8E5" (or "E8E5"), a Segoe Fluent / MDL2 code point, drawn with Phosphor
// where mapped (phosphor::forSegoe), else with the system icon font; in the
// subtext colour of the theme, about three quarters of the box, as Glyph.qml
// shows them.
QImage glyphIcon(const QString& code, int px, bool dark)
{
    bool ok = false;
    const QString hex = code.startsWith(u"U+", Qt::CaseInsensitive) ? code.mid(2) : code;
    const uint point = hex.toUInt(&ok, 16);
    if (!ok || point == 0 || point > 0x10FFFF)
        return {};
    const QString text = QString::fromUcs4(reinterpret_cast<const char32_t*>(&point), 1);
    const std::optional<phosphor::Icon> icon = phosphor::forSegoe(text);
    QFont font(icon ? icon->family : SystemTheme::iconFontName());
    font.setPixelSize(std::max(8, int(px * 3 / 4 * (icon ? phosphor::kScale : 1.0))));
    // Never a character from another font: where an older icon font lacks
    // one, nothing, and the caller's fallback icon instead.
    font.setStyleStrategy(QFont::NoFontMerging);
    if (!icon && !QFontMetrics(font).inFontUcs4(point))
        return {};
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(dark ? QColor(0xA7, 0xA7, 0xA7) : QColor(0x66, 0x66, 0x66));
    painter.drawText(QRect(0, 0, px, px), Qt::AlignCenter, icon ? icon->text : text);
    painter.end();
    return image;
}

// "#ff8800" (or any name QColor reads) as a rounded square with the
// neutral 1 px edge every box has (Theme.windowEdge), a little inset so it
// weighs like an app's icon.
QImage colorIcon(const QString& text, int px, bool dark)
{
    const QColor color = QColor::fromString(text);
    if (!color.isValid())
        return {};
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal border = std::max(1, px / 32); // one logical pixel
    const qreal inset = px / 8.0;
    const QRectF box(inset + border / 2, inset + border / 2, px - 2 * inset - border, px - 2 * inset - border);
    painter.setPen(QPen(dark ? QColor(0x6A, 0x6A, 0x6A) : QColor(0xB4, 0xB4, 0xB4), border));
    painter.setBrush(color);
    painter.drawRoundedRect(box, px / 6.0, px / 6.0);
    painter.end();
    return image;
}

bool localIconPath(const QString& path)
{
    const QString native = QDir::toNativeSeparators(path);
    if (native.startsWith(u"\\\\"))
        return false;
    if (native.size() >= 2 && native[1] == u':') {
        const wchar_t root[] {static_cast<wchar_t>(native[0].unicode()), L':', L'\\', L'\0'};
        return ::GetDriveTypeW(root) != DRIVE_REMOTE;
    }
    return true;
}

// Resolve names the shell can launch without doing any work on the GUI
// thread. Network locations keep the command glyph instead of holding up
// every other icon when their server is asleep.
QString commandProgram(const QString& program, const QString& directory)
{
    if (!localIconPath(program))
        return {};
    if (QDir::isAbsolutePath(program))
        return program;
    const QString base = directory.isEmpty() ? QDir::currentPath()
        : QString::fromStdWString(win32::expandEnvironment(directory.toStdWString()));
    if (program.contains(u'\\') || program.contains(u'/'))
        return localIconPath(base) ? QDir(base).absoluteFilePath(program) : QString();

    QStringList paths {base, QCoreApplication::applicationDirPath(),
        QString::fromStdWString(win32::expandEnvironment(L"%SystemRoot%\\System32")),
        QString::fromStdWString(win32::expandEnvironment(L"%SystemRoot%"))};
    paths.append(qEnvironmentVariable("PATH").split(u';', Qt::SkipEmptyParts));
    paths.removeIf([](const QString& path) { return !localIconPath(path); });
    if (const QString found = QStandardPaths::findExecutable(program, paths); !found.isEmpty())
        return found;

    const QString name = program.endsWith(u".exe", Qt::CaseInsensitive) ? program : program + u".exe"_s;
    const std::wstring key = (u"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"_s + name).toStdWString();
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t registered[32768] {};
        DWORD bytes = sizeof registered;
        if (::RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr,
                registered, &bytes) == ERROR_SUCCESS) {
            const QString path = QString::fromWCharArray(registered).trimmed();
            const QString unquoted = path.startsWith(u'"') && path.endsWith(u'"') ? path.mid(1, path.size() - 2) : path;
            if (localIconPath(unquoted))
                return unquoted;
        }
    }
    return {};
}

QImage commandImage(const QStringList& parts, int px)
{
    const bool dark = parts.value(3) == u"dark";
    const QByteArray png = favoriteIconPng(decode(parts.value(2)));
    if (!png.isEmpty()) {
        if (const QImage custom = siteicon::decode(png); !custom.isNull())
            return siteicon::drawn(custom, px, dark);
    }
    const QString line = QString::fromStdWString(win32::expandEnvironment(decode(parts.value(0)).toStdWString())).trimmed();
    if (!localIconPath(line.startsWith(u'"') ? line.mid(1) : line))
        return glyphIcon(u"U+E756"_s, px, dark);
    const auto [program, arguments] = splitCommand(line);
    if (!program.isEmpty()) {
        const QString scheme = QUrl(program).scheme();
        if (scheme.size() > 1) {
            if (scheme == u"shell") {
                if (QImage image = shellItemIcon(program.toStdWString(), px); !image.isNull())
                    return image;
            } else if (QImage image = schemeIcon(scheme, px); !image.isNull()) {
                return image;
            }
        } else if (const QString path = commandProgram(program, decode(parts.value(1))); !path.isEmpty()) {
            if (QImage image = FileIconProvider::fileIcon(path, px); !image.isNull())
                return image;
        }
    }
    return glyphIcon(u"U+E756"_s, px, dark);
}

} // namespace

QString FileIconProvider::pluginIconUrl(const QString& pluginId, const QString& icon, bool dark)
{
    const QString theme = dark ? u"/dark"_s : u"/light"_s;
    const plugin::IconSpec spec = plugin::parseIcon(icon);
    switch (spec.kind) {
    case plugin::IconSpec::Kind::File:
        return iconUrl(spec.value, false);
    case plugin::IconSpec::Kind::Ext:
        return u"image://fileicon/ext/"_s + encode(spec.value.toLower());
    case plugin::IconSpec::Kind::App:
        return appIconUrl(spec.value, true, dark); // a packaged logo if it has one, else the program's icon
    case plugin::IconSpec::Kind::Glyph:
        return u"image://fileicon/glyph/"_s + encode(spec.value) + theme;
    case plugin::IconSpec::Kind::Color:
        return u"image://fileicon/color/"_s + encode(spec.value) + theme;
    case plugin::IconSpec::Kind::Data: {
        // "data:image/png;base64,…": decoded once, kept by its hash (a URL
        // holding the bytes themselves would be as long as the picture).
        const qsizetype comma = spec.value.indexOf(u',');
        if (comma < 0 || !spec.value.left(comma).endsWith(u";base64"_s, Qt::CaseInsensitive))
            break;
        const QByteArray bytes = QByteArray::fromBase64(spec.value.mid(comma + 1).toLatin1());
        if (bytes.isEmpty() || bytes.size() > 64 * 1024)
            break;
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
        QMutexLocker lock(&g_pluginMutex);
        if (g_pluginData.size() >= kPluginDataKept)
            g_pluginData.clear();
        g_pluginData.insert(hash, bytes);
        return u"image://fileicon/pdata/"_s + hash;
    }
    case plugin::IconSpec::Kind::Site:
        return webIconUrl(spec.value, dark); // the browser's until the site's is here
    case plugin::IconSpec::Kind::PluginFile:
        return u"image://fileicon/plugin/"_s + encode(pluginId) + u'/' + encode(spec.value) + theme;
    case plugin::IconSpec::Kind::None:
        break;
    }
    return u"image://fileicon/glyph/"_s + encode(u"U+EA86"_s) + theme; // Puzzle
}

void FileIconProvider::setPluginFolders(QHash<QString, QString> folders)
{
    QMutexLocker lock(&g_pluginMutex);
    g_pluginFolders = std::move(folders);
}

// "plugin/<id>/<file>/<dark|light>", "glyph/<code>/<theme>", "color/<text>/<theme>", "pdata/<hash>".
QImage FileIconProvider::pluginImage(const QString& id, int px)
{
    const QStringList parts = id.split(u'/');
    const bool dark = parts.last() == u"dark";
    if (parts.value(0) == u"glyph" && parts.size() == 3)
        return glyphIcon(decode(parts[1]), px, dark);
    if (parts.value(0) == u"color" && parts.size() == 3)
        return colorIcon(decode(parts[1]), px, dark);
    if (parts.value(0) == u"pdata" && parts.size() == 2) {
        QByteArray bytes;
        {
            QMutexLocker lock(&g_pluginMutex);
            bytes = g_pluginData.value(parts[1]);
        }
        return fitTo(QImage::fromData(bytes).convertToFormat(QImage::Format_ARGB32_Premultiplied), px);
    }
    if (parts.value(0) != u"plugin" || parts.size() != 4)
        return {};
    QString folder;
    {
        QMutexLocker lock(&g_pluginMutex);
        folder = g_pluginFolders.value(decode(parts[1]));
    }
    const QString file = decode(parts[2]);
    if (folder.isEmpty() || !plugin::insideFolder(file))
        return {};
    const QString path = QDir(folder).filePath(QDir::fromNativeSeparators(file));
    QString chosen = path;
    if (dark) { // "icon.dark.png" beside "icon.png", when there is one
        const QFileInfo info(path);
        const QString variant = info.path() + u'/' + info.completeBaseName() + u".dark."_s + info.suffix();
        if (QFileInfo::exists(variant))
            chosen = variant;
    }
    QImage image(chosen);
    if (image.isNull())
        return {};
    return fitTo(image.convertToFormat(QImage::Format_ARGB32_Premultiplied), px);
}

FileIconProvider::FileIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading)
{
}

QString FileIconProvider::iconUrl(const QString& path, bool isDir)
{
    if (isDir)
        return u"image://fileicon/folder"_s;
    const QString suffix = QFileInfo(path).suffix().toLower();
    // Not a network file's own icon: reading it from a server that went
    // away would take half a minute. Its type's then, as for every file.
    if (hasOwnIcon(suffix) && !path.startsWith(u"\\\\"))
        return u"image://fileicon/file/"_s + encode(path);
    return u"image://fileicon/ext/"_s + encode(suffix);
}

QString FileIconProvider::commandIconUrl(const QString& command, const QString& directory, const QString& icon, bool dark)
{
    return u"image://fileicon/command/"_s + encode(command) + u'/' + encode(directory) + u'/' + encode(icon)
        + (dark ? u"/dark"_s : u"/light"_s);
}

QImage FileIconProvider::fileIcon(const QString& path, int px)
{
    thread_local ComApartment com;
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    QImage image = shellItemIcon(native, px);
    return image.isNull() ? shellIcon(native, 0, false, px) : image;
}

QImage FileIconProvider::thumbnail(const QString& path, int px, bool cachedOnly)
{
    thread_local ComApartment com;
    return shellThumbnail(path.toStdWString(), px, cachedOnly);
}

QImage FileIconProvider::largeImage(const QString& path, int px)
{
    thread_local ComApartment com;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(::SHCreateItemFromParsingName(reinterpret_cast<const wchar_t*>(path.utf16()), nullptr, IID_PPV_ARGS(&factory))))
        return {};
    HBITMAP bitmap = nullptr;
    // Neither THUMBNAILONLY nor ICONONLY: the thumbnail, else the icon drawn that large.
    if (FAILED(factory->GetImage({px, px}, SIIGBF_BIGGERSIZEOK, &bitmap)) || !bitmap)
        return {};
    QImage image = fromShellBitmap(bitmap);
    ::DeleteObject(bitmap);
    if (image.width() > px || image.height() > px)
        image = image.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

QString FileIconProvider::thumbnailUrl(const QString& path)
{
    return u"image://fileicon/thumb/"_s + encode(path);
}

QString FileIconProvider::appIconUrl(const QString& appId, bool packaged, bool dark)
{
    const QString url = u"image://fileicon/app/"_s + encode(appId);
    if (!packaged)
        return url;
    return url + (dark ? u"/dark"_s : u"/light"_s); // its logo has a variant per theme
}

QString FileIconProvider::placeIconUrl(const QString& icon)
{
    return u"image://fileicon/place/"_s + encode(icon);
}

QString FileIconProvider::webIconUrl(const QString& homeUrl, bool dark)
{
    // A picture its user chose (WebShortcut::iconRef), named after its content:
    // another picture is another address, never an old one cached.
    if (homeUrl.startsWith(u"custom:"))
        return u"image://fileicon/custom/"_s + encode(homeUrl.mid(7)) + (dark ? u"/dark"_s : u"/light"_s);
    if (const QString site = siteIconKey(homeUrl); !site.isEmpty()) {
        if (siteicons::has(site))
            return u"image://fileicon/site/"_s + encode(site) + (dark ? u"/dark"_s : u"/light"_s);
        siteicons::fetch({homeUrl}); // its rows get the new address once it is here
    }
    // One icon per scheme: every https page shows the browser's.
    QString scheme = QUrl(homeUrl, QUrl::TolerantMode).scheme();
    if (scheme.isEmpty())
        scheme = u"https"_s;
    return u"image://fileicon/web/"_s + encode(scheme);
}

QImage FileIconProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    thread_local ComApartment com; // SHGetFileInfo requires COM on the calling thread

    if (id.startsWith(u"thumb/")) { // large and seen one at a time: not cached (Windows keeps them)
        const int px = requestedSize.width() > 0 ? std::clamp(requestedSize.width(), 16, 1024) : 256;
        const QImage image = shellThumbnail(decode(QStringView(id).mid(6)).toStdWString(), px);
        *size = image.size();
        return image;
    }
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
    if (id.startsWith(u"command/")) {
        image = commandImage(id.mid(8).split(u'/'), px);
    } else if (id == u"folder") {
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
    } else if (id.startsWith(u"place/")) {
        image = placeIcon(decode(QStringView(id).mid(6)), px);
        if (image.isNull()) // Control Panel's own
            image = shellItemIcon(L"shell:::{26EE0668-A00A-44D7-9371-BEB064C98683}", px);
    } else if (id.startsWith(u"web/")) {
        image = schemeIcon(decode(QStringView(id).mid(4)), px);
    } else if (id.startsWith(u"site/")) {
        // "site/<key>/<dark|light>"
        const QStringList parts = id.mid(5).split(u'/');
        const QImage kept = siteicons::load(decode(parts.value(0)));
        image = kept.isNull() ? schemeIcon(u"https"_s, px) : siteicon::drawn(kept, px, parts.value(1) == u"dark");
    } else if (id.startsWith(u"custom/")) {
        // "custom/<name>/<dark|light>"
        const QStringList parts = id.mid(7).split(u'/');
        const QImage kept = siteicons::loadCustom(decode(parts.value(0)));
        image = kept.isNull() ? schemeIcon(u"https"_s, px) : siteicon::drawn(kept, px, parts.value(1) == u"dark");
    } else if (id.startsWith(u"plugin/") || id.startsWith(u"glyph/") || id.startsWith(u"color/") || id.startsWith(u"pdata/")) {
        image = pluginImage(id, px);
        if (image.isNull()) // a picture that is not there, a colour that is none
            image = glyphIcon(u"U+EA86"_s, px, id.endsWith(u"/dark"));
    }
    if (image.isNull())
        image = shellIcon(L"file", FILE_ATTRIBUTE_NORMAL, true, px);

    *size = image.size();
    shell::prepareMenuHooks(); // the shell has loaded its icon handlers into the process by now
    QMutexLocker lock(&m_mutex);
    m_cache.insert(key, new QImage(image));
    return image;
}

} // namespace ws
