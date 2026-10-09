#pragma once

#include <windows.h>

#include <string>
#include <vector>

// The Open and Save dialogs of Windows in other programs, driven from outside
// with window messages alone, as a user would drive them: nothing is loaded
// into the other program and no keys are synthesised (see DialogJump).
namespace ws::filedialog {

enum class Kind {
    None, // not a file dialog
    Open, // Windows Vista and later: IFileOpenDialog, GetOpenFileName without a hook
    Save,
    Folder, // a folder picker (FOS_PICKFOLDERS)
    Legacy, // the Windows XP style, still shown for GetOpenFileName with a hook or template
};

Kind kind(HWND hwnd);

// The folders open in File Explorer: each window's tab on top, front to back,
// then the tabs behind them. Folders on disk only (no Home, This PC or
// searches). Needs COM; asks Explorer, so not on the GUI thread.
std::vector<std::wstring> explorerFolders();

// The folder the dialog shows, as its address bar names it; empty for a
// place that is no folder on disk (This PC, a library) and in the Windows XP
// style, which has no address bar.
std::wstring currentFolder(HWND dialog);

// The file type chosen in an Open dialog, as its list shows it: "Text
// documents (*.txt)". Empty for the other kinds.
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
