#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace ws::saved {

// What a click on "截图已保存" shows (GitHub #99). Seconds, hours or days
// later the file may be gone (moved, deleted, the drive removed): then its
// folder, if that is still there, else nothing.
enum class Show {
    File, // the folder, with the file selected
    Folder, // the folder only
    Nothing,
};

struct Target {
    Show show = Show::Nothing;
    QString path; // the file or the folder to hand to the file manager
};

inline Target target(const QString& file)
{
    if (file.isEmpty())
        return {};
    const QFileInfo info(file);
    if (info.isFile())
        return {Show::File, file};
    const QString folder = info.absolutePath();
    if (QFileInfo(folder).isDir())
        return {Show::Folder, folder};
    return {};
}

} // namespace ws::saved
