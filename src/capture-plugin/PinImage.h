#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>

#include <optional>

namespace ws::captureplugin::pinimage {

struct Placement {
    QPoint cursor;
    QRect work;
    int dpi = 96; // the monitor's: copied words are drawn at its size (renderText)
};

struct Prepared {
    QImage image;
    QPoint origin; // the whole picture's top left, its middle at the cursor
    QPoint anchor; // the cursor: the pin starts on its monitor (PinManager::pinFitted)
};

struct LastCapture {
    QString id;
    quint32 clipboardSequence = 0;
};

// Text: no picture, but words (GitHub #388), pinned as renderText draws them.
enum class ClipboardStatus { Image, LastCapture, Text, Empty, Busy, Unreadable };

struct ClipboardPicture {
    ClipboardStatus status = ClipboardStatus::Empty;
    QImage image;
    QString text; // Text: what was copied, at most kPinTextChars
};

// The most of copied words a pin keeps to copy again (UTF-16 units); the
// card shows fewer (renderText).
inline constexpr qsizetype kPinTextChars = 256 * 1024;

// How copied words look pinned: the 界面风格's floating panel.
struct TextLook {
    QColor background; // made opaque over a grey of the theme's
    QColor text;
    QColor subtext; // the … where the words go on past the card
    bool dark = false;
    QString font; // the family of Win顺's words; empty: the application's
};

// Copied words as a picture to pin (GitHub #388): on a card of the panel's
// colour, wrapped at a reading width, in pixels of a monitor of `dpi` (so
// they are sharp at 100% there). Lines keep their breaks and indents; text
// indented as code is drawn in a monospaced font. The card shows the first
// 2,000 characters and 50 lines, an … below when there is more. Null for
// words that are only spaces.
QImage renderText(const QString& text, int dpi, const TextLook& look);

// Snapshot on invocation; decoding must not move a pin to wherever the mouse
// happens to be when a large picture finishes loading. Coordinates are physical.
std::optional<Placement> cursorPlacement();
// Its own pixels, the whole picture's middle at the cursor: the pin fits a
// picture larger than the screen (PinManager::pinFitted), it is not stored
// fitted.
Prepared prepare(QImage image, const Placement& placement);

// Worker-thread operations: copy clipboard bytes/paths under a short lock,
// release it before decoding, then use the same WIC reader as file pinning.
ClipboardPicture readClipboard(const LastCapture& last);
QImage readFile(const QString& path);

} // namespace ws::captureplugin::pinimage
