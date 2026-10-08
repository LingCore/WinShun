#pragma once

#include <QCache>
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
    // An installed app (AppCatalog). Packaged apps' logos differ by theme.
    static QString appIconUrl(const QString& appId, bool packaged, bool dark);
    // A place in Windows (SystemCatalog), by its icon: one in a module
    // ("%SystemRoot%\System32\netcenter.dll,-1"), or that of a file or shell path.
    static QString placeIconUrl(const QString& icon);

private:
    QMutex m_mutex;
    QCache<QString, QImage> m_cache {256};
};

} // namespace ws
