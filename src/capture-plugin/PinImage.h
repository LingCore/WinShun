#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>

#include <optional>

namespace ws::captureplugin::pinimage {

struct Placement {
    QPoint cursor;
    QRect work;
};

struct Prepared {
    QImage image;
    QPoint origin;
};

struct LastCapture {
    QString id;
    quint32 clipboardSequence = 0;
};

enum class ClipboardStatus { Image, LastCapture, Empty, Busy, Unreadable };

struct ClipboardPicture {
    ClipboardStatus status = ClipboardStatus::Empty;
    QImage image;
};

// Snapshot on invocation; decoding must not move a pin to wherever the mouse
// happens to be when a large picture finishes loading. Coordinates are physical.
std::optional<Placement> cursorPlacement();
// Its own pixels, starting where the whole picture's middle is at the cursor:
// the pin fits a picture larger than the screen, it is not stored fitted.
Prepared prepare(QImage image, const Placement& placement);

// Worker-thread operations: copy clipboard bytes/paths under a short lock,
// release it before decoding, then use the same WIC reader as file pinning.
ClipboardPicture readClipboard(const LastCapture& last);
QImage readFile(const QString& path);

} // namespace ws::captureplugin::pinimage
