#pragma once

#include <windows.h>

#include <string>
#include <vector>

// The Open and Save dialogs of Windows in other programs, driven from outside
// with window messages alone, as a user would drive them: nothing is loaded
// into the other program and no keys are synthesised (see DialogJump). The
// one exception: SHBrowseForFolder's dialog takes a folder only as a string
// in its own memory (BFFM_SETSELECTION), so the path is written there.
//
// And WPS Office's own (Kingsoft's "KcfdFileDialog"): a Qt window whose
// controls are no windows, so only UI Automation sees them. Asked on a worker
// thread with COM only (probe(), goTo()…): they wait on the program.
namespace ws::filedialog {

enum class Kind {
    None, // not a file dialog
    Open, // Windows Vista and later: IFileOpenDialog, GetOpenFileName without a hook
    Save,
    Folder, // a folder picker (FOS_PICKFOLDERS), or Inno Setup's destination folder picker
    Legacy, // the Windows XP style, still shown for GetOpenFileName with a hook or template
    Browse, // "浏览文件夹": SHBrowseForFolder's tree (VBA macros, Shell.Application.BrowseForFolder)
};

// A WPS Office dialog is one once probe() has said so.
Kind kind(HWND hwnd);

// Could the window be a WPS Office file dialog: a Qt window of one of its
// programs, owned by another. Quick, for any thread; probe() tells.
bool mayBeKingsoft(HWND hwnd);
bool probed(HWND hwnd); // probe() has an answer kept for it
// Asks the window through UI Automation and keeps the answer for kind(). On
// a worker thread: it waits on the program.
Kind probe(HWND hwnd);

// The folder the dialog shows, as its address bar names it; empty for a
// place that is no folder on disk (This PC, a library), in the Windows XP
// style and SHBrowseForFolder's, which have no address bar, and in WPS Office's, whose crumbs UI
// Automation does not tell right.
std::wstring currentFolder(HWND dialog);

// The file type chosen in an Open dialog, as its list shows it: "Text
// documents (*.txt)". Empty for the other kinds, and for WPS Office's, whose
// list tells the type it started with.
std::wstring fileType(HWND dialog);

// Waits (up to a second) until the dialog is in front and has the keyboard
// focus on one of its controls again. Just brought to the front, it has not
// yet, and goTo() would find no focus to give back after the move.
bool waitForFront(HWND dialog);

// Waits (up to a second and a half) until a dialog that has just come up
// shows where it is: its address bar is empty until then.
bool waitForLocation(HWND dialog);

// Takes the dialog to `folder`; false if it did not go there. Waits for the
// dialog (up to a few seconds): not on its thread.
bool goTo(HWND dialog, const std::wstring& folder);

// Puts `name` in the file name box and the keyboard focus on it.
bool setFileName(HWND dialog, const std::wstring& name);

// Presses Open (or Save): for the name in the file name box. Does not wait
// for the program, which may take a while with the file.
bool accept(HWND dialog);

} // namespace ws::filedialog
