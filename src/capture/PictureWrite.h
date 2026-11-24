#pragma once

#include <QImage>
#include <QString>
#include <QStringView>

namespace ws::capture {

// Picture file extensions that Windows Imaging Component (and Qt PNG) can write
// back reliably without destroying animation frames (.gif) or multi-icon containers (.ico).
bool isWritableImageExtension(QStringView path);

// Whether `path` is a user-owned, writable-format image file eligible for direct
// save-back (#427): rejects directories, unsupported formats, and WinShun's own
// internal cache/data directories.
bool isWritableSourceFile(const QString& path, const QString& internalDataDir = {});

// Encodes `image` in the format matching `targetPath`'s extension (PNG, JPEG,
// BMP, TIFF; unknown extensions fall back to PNG) and replaces `targetPath`
// atomically via a same-directory temp file so the original file is never left
// truncated or corrupted on failure. Returns an empty string on success, or a
// user-facing error message.
QString writeImageFile(const QImage& image, const QString& targetPath);

// Shortened display name for menus/pills (e.g. "very-long-name…tail.png").
QString shortFileName(const QString& path, qsizetype maxChars = 28);

} // namespace ws::capture
