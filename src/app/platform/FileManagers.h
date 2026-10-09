#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

// The file managers Win顺 works with: File Explorer, Total Commander and
// Directory Opus. They are asked which folders their windows show (for the
// Open and Save dialogs: Ctrl+G, the bar beside them, going there by
// themselves), and told to open a folder or to show where an item is
// (Settings::fileManager, through shell::open and shell::reveal).
//
// Total Commander answers window messages (WM_USER+50) at once. Directory
// Opus answers through its dopusrt.exe, started with the user's normal
// rights: started elevated, as Win顺 is, it waits for an answer that never
// comes (docs/pitfalls.md).
namespace ws::filemanager {

enum class Kind {
    Explorer,
    TotalCommander,
    DirectoryOpus,
};

// The names Settings::fileManager knows them by.
inline constexpr std::pair<Kind, const char*> kSettingNames[] {
    {Kind::Explorer, "explorer"},
    {Kind::TotalCommander, "totalcmd"},
    {Kind::DirectoryOpus, "dopus"},
};

// Which one's main window this is (a top-level window); nullopt for any other.
std::optional<Kind> kindOf(HWND window);

struct Folder {
    std::wstring path;
    Kind shownIn;
};

// The folders their windows show: each window's folder in front (the tab on
// top, the panel with the focus), front to back, then the others (tabs
// behind, the other panel). Folders on disk only: no Home, This PC,
// searches, FTP sites or archives. Needs COM; asks the other programs
// (Directory Opus only while one of its windows is open: asking starts it),
// so not on the GUI thread.
std::vector<Folder> openFolders();

// The program that takes the commands: TOTALCMD64.EXE (TOTALCMD.EXE),
// dopusrt.exe; empty when it is not installed. A Total Commander that runs
// (maybe a portable one) before the one installed. Explorer: always empty.
std::wstring program(Kind kind);
bool installed(Kind kind); // Explorer always

// With the user's normal rights, in a new tab (Directory Opus: the tab that
// shows it already, if any), starting the program if it does not run. False
// when it is not installed or did not start. Not for Explorer: that is
// shell::open's and shell::reveal's own.
bool openFolder(Kind kind, const std::wstring& folder);
// Each item's folder, with the item selected (in Total Commander, which has
// one cursor, the first item of each folder).
bool reveal(Kind kind, const std::vector<std::wstring>& paths);

} // namespace ws::filemanager
