#pragma once

#include <windows.h>

#include <string>
#include <string_view>

// Windows of other programs that ask for a folder in a text box of their own:
// the folder written there is the answer, and the window's OK takes it. They
// are no Windows file dialog and have no address bar; going to a folder is
// writing it into that box, as the user would type it, and nothing is ever
// confirmed for the user (no Enter, no OK).
//
//  - Inno Setup's destination folder picker (TSelectFolderForm).
//  - A dialog with a folder tree and one box holding a path, OK and Cancel:
//    Bandizip's "选择解压路径" and windows built the same way. Known by what
//    it is made of, in any program (measured on Bandizip 7.36: the box is a
//    combo box, read on OK; told of the edit with CBN_EDITCHANGE, the tree
//    follows).
//  - 7-Zip's "Extract" (7zG.exe, 7zFM.exe), by the control IDs of its source
//    (CPP/7zip/UI/GUI/ExtractDialogRes.h). Its OK reads the list's selected
//    entry before the text, so the selection is cleared first, as its own
//    "..." button does.
//  - WinRAR's "Extraction path and options", as Listary's open plugin for it
//    drives it (Listary.FileAppPlugin.WinRAR): the path box on the first
//    page, then its "Display" button, which takes the tree there.
//
// Evidence and what was run for real: docs/dialog-compatibility.md.
//
// Window messages only, no Qt: shared by the UI (platform/FileDialog) and the
// service's helper for elevated windows (DialogNav).
namespace ws::pathbox {

struct Box {
    HWND edit = nullptr; // holds the folder
    HWND combo = nullptr; // the combo box the edit is part of, if it is
    HWND show = nullptr; // a button of the program that takes its own view to the folder in the box
    explicit operator bool() const { return edit != nullptr; }
};

// The box of `dialog`, if it is one of these windows. The window made of a
// tree and a box is taken only while the box holds a path ("C:\x",
// "\\server\share"), unless `known`: the caller has seen it hold one before
// (the user may be typing in it now).
Box find(HWND dialog, bool known = false);

std::wstring folder(const Box& box);

// Writes `folder` into the box as it is and tells the program of the edit;
// false if the box did not take it. Never confirms.
bool setFolder(const Box& box, const std::wstring& folder);

// "C:\", "D:\a b", "\\server\share\x": an absolute path by its shape alone.
bool looksLikePath(std::wstring_view text);

} // namespace ws::pathbox
