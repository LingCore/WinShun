#pragma once

// GitHub #202: a plugin's row in the content pane (内容详情) and the preview.
// A row that opens a plain file (a content provider's always does) shows
// that file, as the file's own row would. A row that is no file (a mail)
// and says "details": true is asked about: Win顺 sends the resident plugin
// `details` {item, text}, and it answers with a file of its own data folder
// or with text, which then shows as a file's would.

#include "SearchTypes.h"

#include <QJsonValue>
#include <QString>
#include <QStringView>

namespace ws::plugin {

// A path that names a plain file by its text alone (no disk: asked as the
// keys move, a share may be offline): absolute, on a drive (C:\…) or a share
// (\\server\share\…); not a device or \\?\ path, no alternate data stream,
// no "." or ".." step, no reserved device name (CON, NUL, COM1 …), not a
// folder's trailing backslash. "/" counts as "\".
bool plainFilePath(QStringView path);

// The file a plugin's row opens, for the content pane and the preview: its
// `open.path` with "\" for "/" when that is a plainFilePath with no
// arguments or working folder (a program started so is not a file to show);
// else empty.
QString rowFile(const PluginRow& row);

// The row asks for `details` in the content pane and the preview: its item
// said "details": true, and its plugin is resident (toRow keeps it only then).
inline bool asksDetails(const PluginRow& row)
{
    return row.item.value(QStringLiteral("details")).toBool();
}

// The most of a `details` answer's text that is shown (characters): about
// 2 MB in memory, as much as clipboard_copy takes.
inline constexpr qsizetype kMaxDetailsText = 1'000'000;

// Where `path` really is when that is a plain file on a local drive inside
// `folder` (both followed through junctions and links to where they lead):
// that path, else empty. Not a folder, not a link itself, not one with other
// hard links (a name in the folder for a file elsewhere). Opens the file and
// the folder to see.
QString fileInFolder(const QString& path, const QString& folder);

// A plugin's answer to `details`: {"file": …} (fileInFolder its data
// folder) or {"text": …} (cut to kMaxDetailsText); "file" first when both
// are given and it is good.
struct DetailsAnswer {
    enum class Kind { None, File, Text };
    Kind kind = Kind::None;
    QString file;
    QString text;
    bool cut = false; // text: longer than kMaxDetailsText
};
DetailsAnswer parseDetails(const QJsonValue& result, const QString& dataDir);

} // namespace ws::plugin
