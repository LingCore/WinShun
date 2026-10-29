#pragma once

#include "ClipStore.h"

#include <vector>

namespace ws::clipfiles {

enum class Error { None, NotImages, WriteFailed };
struct Export {
    QStringList paths;
    Error error = Error::None;
};

// Copies every selected image outside the history, in selection order. A failed
// batch returns no paths: never silently drag/paste just part of the selection.
Export images(
    const ClipStore& store, const std::vector<const Clip*>& clips, const QString& folder, const QString& name);

// 保存为文件 (a forum request, 2026-10-29): the selected entries as files of
// their own in a folder the user names. Planned on the GUI thread (the store
// is read there), written on any.
struct Planned {
    QString base; // its name without the extension
    QString extension; // ".txt"; empty for a folder
    QByteArray bytes; // words: the file as written
    QString source; // a picture's original, or a copied file or folder
    bool original = false; // a file or folder of a Files entry: dragged out as it is, no copy
};
struct Plan {
    std::vector<Planned> files;
    int missing = 0; // pictures whose file is gone
};
// Words in the kind of file their text looks like (粘贴为文件's rules,
// core/PasteFile.h), a link as an Internet shortcut (.url), a picture as it
// was copied (PNG, GIF, JPEG), files and folders copied as they are. With
// `merged`, the words of all of them in one file, `separator` between them.
// Names in English with `english`.
Plan plan(const ClipStore& store, const std::vector<const Clip*>& clips, bool english, bool merged = false,
    const QString& separator = {});
struct Saved {
    QStringList paths; // made, in the plan's order
    int missing = 0; // files and folders of the entries no longer there
    int failed = 0;
};
// Each of the plan's files made in `folder` under the first free name, " (2)",
// " (3)"... after one taken, as Explorer numbers them; nothing is replaced.
// The folder is made when it is not there yet.
// Copies files: not on the GUI thread.
Saved save(const Plan& plan, const QString& folder);

// The file dialog's quoted list, including full paths for files in different
// folders. Not command-line escaping: backslashes, including UNC, stay literal.
QString fileNames(const QStringList& paths);

} // namespace ws::clipfiles
