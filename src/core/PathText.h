#pragma once

#include "SearchTypes.h"

#include <QString>
#include <QStringList>

// Paths in text that other programs show or hand over, or that the user
// types: what was copied, what a file dialog's controls say, a path being
// typed into the search bar under file dialogs (and what it names on disk).
namespace ws::pathtext {

// A path copied as text: the first line, maybe in quotes (Explorer's "Copy
// as path"), maybe a file:/// URL or with %VARIABLES%. An absolute path on a
// drive ("C:\…") or a share ("\\server\share\…"), with backslashes and
// without a trailing one (but "C:\"); else empty.
QString pathFromText(const QString& text);

// A path being typed, split where the name being typed starts:
// "D:\Pro" -> {"D:\", "Pro"}, "D:\Projects\" -> {"D:\Projects", ""},
// "D:" -> {"D:\", ""}. Taken as pathFromText() takes text (quotes, file:
// URLs, %VARIABLES%). An empty `folder`: the text is no path, or the name
// has characters no name can have ("D:\*.pdf").
struct TypedPath {
    QString folder;
    QString name;
};
TypedPath splitTyped(const QString& text);

// What `typed.folder` holds that `typed.name` matches, as the search box
// matches names (anywhere in the name, by pinyin too; everything when no
// name is typed): folders first, then the best matches, then in Explorer's
// order (numbers by their value). With no name typed, the folder itself
// comes first. Hidden entries are left out, as Explorer leaves them out; at
// most `max`. Reads the disk: not on the GUI thread.
SearchResults listTyped(const TypedPath& typed, bool foldersOnly, int max);

// The extensions a file dialog's file type names, in lower case: "Text
// files (*.txt;*.log)" -> {"txt", "log"}. Empty when it takes every file
// ("*.*") or names no plain extension.
QStringList filterExtensions(const QString& fileType);

// The folder an address bar's caption names: "Address: C:\Windows" ->
// "C:\Windows". Empty for a place that is no folder on disk ("Address:
// This PC").
QString folderFromAddress(const QString& caption);

// Whether two paths name the same folder: case and a trailing backslash aside.
bool sameFolder(const QString& a, const QString& b);

} // namespace ws::pathtext
