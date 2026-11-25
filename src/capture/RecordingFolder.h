#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace ws::capture {

// Where recordings go while no folder is chosen: WinShun in the user's Videos.
// On its own here so the settings can name it without the recorder.
inline QString defaultRecordingFolder()
{
    return QDir::toNativeSeparators(
        QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath(QStringLiteral("WinShun")));
}

} // namespace ws::capture
