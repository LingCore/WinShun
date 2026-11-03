#pragma once

#include <QQuickAsyncImageProvider>

namespace ws {

// Serves the clipboard rows' pictures to QML as "image://clipthumb/<key>":
// a picture in the history, or a picture file copied (WeChat), scaled to its
// row's slot.
//
// Plain file URLs went to Qt's one image thread per engine, decoded whole
// every time a row came into view, and dropped again (Image.cache false): a
// list scrolled back showed grey tiles while each picture was decoded anew,
// one after the other. Here decoding runs on a few threads of our own, the
// newest request first, and one Qt cancels (its row scrolled away or reused)
// is dropped before it starts. The scaled pictures are kept by bytes, so
// scrolling back finds them at once. Scaled as Qt scales a file for
// sourceSize (the same size, the same filter), so a row looks as it did.
class ClipThumbnailProvider : public QQuickAsyncImageProvider {
public:
    ClipThumbnailProvider();

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    // The address of a picture file's thumbnail. `stamp` tells versions of
    // a file apart (its time written; 0 for one never written again): a
    // file written since gets a new address, never the old picture.
    static QString url(const QString& path, qint64 stamp = 0);
};

} // namespace ws
