#pragma once

#include <QByteArray>
#include <QFile>
#include <QFontDatabase>
#include <QString>

namespace ws {

// A font file added to the application's fonts by its bytes: for a file
// FreeType cannot open by its path (it opens through the ANSI C runtime, so
// not in a Chinese-named folder, nor in the program's resources). The bytes
// are the file's own pages, mapped: in memory only where a glyph was read
// from, and Windows' to take back at no cost. Read into the heap instead, the
// interface font was 8.5 MB in a block of 16 for as long as the program ran.
// Returns QFontDatabase's id; -1 if the file cannot be read.
inline int addFontFromFile(const QString& path)
{
    auto* const file = new QFile(path);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        return -1;
    }
    if (const uchar* const mapped = file->map(0, file->size())) {
        // The file stays open and mapped as long as the font is: until the process ends.
        return QFontDatabase::addApplicationFontFromData(QByteArray::fromRawData(reinterpret_cast<const char*>(mapped), file->size()));
    }
    const QByteArray bytes = file->readAll(); // cannot be mapped (a compressed resource): a copy after all
    delete file;
    return QFontDatabase::addApplicationFontFromData(bytes);
}

} // namespace ws
