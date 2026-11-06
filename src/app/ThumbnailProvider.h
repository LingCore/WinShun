#pragma once

#include <QQuickAsyncImageProvider>

#include <cstdint>

namespace ws {

// Serves the result rows' thumbnails to QML as "image://thumbnail/<key>":
// what Explorer shows for a picture, a video, a PDF, instead of the type's
// icon.
//
// Not through FileIconProvider: Qt loads every image of a plain provider on
// one thread per engine, so one slow thumbnail (a video's handler, a large
// photo, a codec loaded the first time) would hold up every icon behind it.
// Here the reader thread only hands the work to a few threads of our own and
// goes on. Each first asks Windows' thumbnail cache (instant), then has the
// thumbnail made. The newest request goes first, and one Qt cancels (its row
// scrolled away or reused) is dropped before it starts, so a fast scroll
// through hundreds of pictures does not queue them all.
//
// A file with nothing to show (no handler, an error, a cloud placeholder,
// a network drive) gives an empty image, not an error: the row's Image is
// Ready with no size and keeps the icon, without a warning per row. Pictures
// made are kept (by bytes) and so are misses, by path, size and time
// written, so typing the same words again costs nothing.
class ThumbnailProvider : public QQuickAsyncImageProvider {
public:
    ThumbnailProvider();

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    // A file's thumbnail address, or "" for one not worth asking about:
    // a type that seldom has one (only pictures, videos, PDF and the like
    // do), a network path (thumbnails read the file). `modified` (seconds
    // since 1970, SearchResult::modified) is part of it: a file written
    // since gets a new address, never the old picture.
    static QString url(const QString& path, std::uint32_t modified);
    static bool hasThumbnailType(const QString& path); // by its extension
    // The thumbnails kept for the lists (at most 48 MB), in bytes; any thread.
    static qint64 cachedBytes();
};

} // namespace ws
