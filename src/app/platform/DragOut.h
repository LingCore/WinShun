#pragma once

#include <QStringList>

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

// Dragging search results out of Win顺: into a chat (WeChat), a folder in
// Explorer, a mail, any program that takes files. Win顺 runs elevated and the
// programs it drops into do not; that works: OLE calls down into the target,
// and the target's calls back into the data object are let through (measured
// 2026-10-11, OLE targets and WM_DROPFILES ones alike).
namespace ws::dragout {

// What a drop target gets: the shell's own data object for the items, as
// Explorer hands them out (their ID lists, CF_HDROP, file names), so whatever
// takes files from Explorer takes them from here. Items of different folders
// may go together, as a search's results do. It prefers a copy (Preferred
// DropEffect). Null when none of the paths names an item.
Microsoft::WRL::ComPtr<IDataObject> dataObject(const QStringList& paths);

// The files by name only (CF_HDROP, preferring a copy), as Explorer's paste,
// chats and Office read them: no text and no URL, which some programs (WPS)
// would paste instead of the file, and no shell lookup, which can hang on a
// network path that went away. For the clipboard (shell::copyFiles).
Microsoft::WRL::ComPtr<IDataObject> fileNamesObject(const QStringList& paths);

enum class Outcome { Dropped, Cancelled, Failed };

// Drags the items until the left button is let go: a modal loop on the
// calling thread, which pumps its messages meanwhile (the GUI thread, with
// the button down). Copy or link, never move: dropped on a folder of the same
// disk, Explorer would otherwise move the user's file away. With the shell's
// drag image, the items' thumbnails and their count. Set allowLink to false
// for temporary exports: a shortcut would break when the export is removed.
Outcome drag(const QStringList& paths, HWND source, bool allowLink = true);

} // namespace ws::dragout
