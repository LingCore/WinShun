#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

namespace ws {

// Serves Windows shell icons to QML as "image://fileicon/<key>".
//
// Icons are looked up by extension (one cached image for every .pdf) without
// touching the disk; only types whose icon is per-file (.exe, .lnk, ...) are
// looked up by path. Installed apps get the icon the Start menu shows: a
// program's icon, or a packaged app's logo file for the theme (AppLogo.h).
// Every icon is made at exactly the requested size, from the picture drawn
// for the nearest size: never scaled up when a bigger one exists. Loading
// happens on Qt's image-loader thread.
class FileIconProvider : public QQuickImageProvider {
public:
    FileIconProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

    static QString iconUrl(const QString& path, bool isDir);
    // A custom command's embedded picture, else its program (resolved only
    // on the image-loader thread); a command glyph if it cannot be found.
    static QString commandIconUrl(const QString& command, const QString& directory, const QString& icon, bool dark);
    // The icon from a file explicitly chosen by the user, without fallback.
    // Called off the GUI thread; null if Windows cannot read it.
    static QImage fileIcon(const QString& path, int px);
    // What Explorer shows as the file's thumbnail (pictures, videos, PDFs
    // and other documents with a thumbnail handler); no image if it has none.
    static QString thumbnailUrl(const QString& path);
    // The same picture, at most `px` pixels on its longer side, on the
    // calling thread (which it joins to COM); null if there is none.
    // `cachedOnly`: only from Windows' thumbnail cache, which is instant.
    static QImage thumbnail(const QString& path, int px, bool cachedOnly);
    // The thumbnail, else the icon drawn as large (a folder's shows what is
    // in it): at most `px` pixels on its longer side, on the calling thread.
    static QImage largeImage(const QString& path, int px);
    // An installed app (AppCatalog). Packaged apps' logos differ by theme.
    static QString appIconUrl(const QString& appId, bool packaged, bool dark);
    // A place in Windows (SystemCatalog), by its icon: one in a module
    // ("%SystemRoot%\System32\netcenter.dll,-1"), or that of a file or shell path.
    static QString placeIconUrl(const QString& icon);
    // A web shortcut by its home page: its site's own icon once it is kept
    // (siteicons; asking for it starts fetching it), until then and for
    // other schemes the icon of the program that opens it, the default
    // browser for https. "custom:<name>" (WebShortcut::iconRef): the picture
    // its user chose. `dark`: on a plate when it would not show (siteicon::drawn).
    static QString webIconUrl(const QString& homeUrl, bool dark);
    // A plugin row's icon (PluginRow::icon, the plugin design §4.5):
    // "file:C:\…", "ext:pdf", "app:<AUMID>" go to the icons above;
    // "glyph:U+E8E5" and "color:#ff8800" are drawn; "data:image/…;base64,…"
    // is decoded (64 KB at most); anything else is a picture in the
    // plugin's folder ("image://fileicon/plugin/<id>/<file>", with a
    // ".dark" variant for the dark theme when it has one). A path that
    // leaves the folder is not an icon.
    static QString pluginIconUrl(const QString& pluginId, const QString& icon, bool dark);
    // Where each plugin's folder is (plugin::Catalog), for "plugin/" icons.
    static void setPluginFolders(QHash<QString, QString> folders);

private:
    QImage pluginImage(const QString& id, int px);

    // Bound pixels as well as entries: 256 icons at 256 px would hold
    // 64 MiB. The minimum cost below keeps the ordinary list's 256 slots.
    static constexpr qsizetype kCacheBytes = 8 * 1024 * 1024;
    static constexpr qsizetype kCacheIcons = 256;
    QMutex m_mutex;
    QCache<QString, QImage> m_cache {kCacheBytes};
};

} // namespace ws
