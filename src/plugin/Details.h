#pragma once

// GitHub #202: a plugin's row in the content pane (内容详情) and the preview.
// A row that opens a plain file (a content provider's always does) shows
// that file, as the file's own row would.

#include "SearchTypes.h"

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

} // namespace ws::plugin
