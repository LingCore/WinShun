#pragma once

#include <windows.h>

#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The file managers Win顺 works with: File Explorer, Total Commander,
// Directory Opus, XYplorer, Pulse and OneCommander. They are asked which
// folders their windows show (for Open and Save dialogs: Ctrl+G, the bar
// beside them, going there by themselves), and told to open a folder or to
// show where an item is (Settings::fileManager, through shell::open and shell::reveal).
// Any other one (Kind::Custom) only opens folders and shows items, by
// command lines of the user's own (setCustomCommands): it cannot be asked.
//
// Total Commander answers window messages (WM_USER+50) at once. Directory
// Opus answers through its dopusrt.exe, started with the user's normal
// rights: started elevated, as Win顺 is, it waits for an answer that never
// comes (docs/pitfalls.md). XYplorer runs a script sent by WM_COPYDATA and
// answers by WM_COPYDATA to a window of ours, while the message is still
// being sent (GitHub #23). Pulse cannot be asked (GitHub #41): its session
// file says what its windows show (pathtext::pulseWindows), saved a few
// seconds late, which the title of a lone window gives away. OneCommander
// exposes CurrentPathGet/CurrentPathSet through UI Automation's ValuePattern.
namespace ws::filemanager {

enum class Kind {
    Explorer,
    TotalCommander,
    DirectoryOpus,
    XYplorer,
    Pulse,
    OneCommander,
    Custom,
};

// The names Settings::fileManager knows them by.
inline constexpr std::pair<Kind, const char*> kSettingNames[] {
    {Kind::Explorer, "explorer"},
    {Kind::TotalCommander, "totalcmd"},
    {Kind::DirectoryOpus, "dopus"},
    {Kind::XYplorer, "xyplorer"},
    {Kind::Pulse, "pulse"},
    {Kind::OneCommander, "onecommander"},
    {Kind::Custom, "custom"},
};

// The command lines for Kind::Custom (Settings::fileManagerOpenCommand and
// fileManagerRevealCommand; pathtext::customCommand says how they are read).
// No reveal command: an item's folder is opened instead.
void setCustomCommands(const QString& open, const QString& reveal);
// The program such a command line names, found as Windows finds it (on the
// PATH when it has no folder); empty when there is none.
QString customProgram(const QString& commandLine);

// Which one's main window this is (a top-level window); nullopt for any other.
std::optional<Kind> kindOf(HWND window);

struct Folder {
    std::wstring path;
    Kind shownIn;
};

// The folders their windows show: each window's folder in front (the tab on
// top, the panel or pane with the focus), front to back, then the others
// (tabs behind, the other panel). Folders on disk only: no Home, This PC,
// searches, FTP sites or archives. Needs COM; asks the other programs
// (Directory Opus only while one of its windows is open: asking starts it),
// and may wait for Pulse to save what it shows (3.5 s at most), so not on
// the GUI thread.
std::vector<Folder> openFolders();

// An Explorer tab by its window (ShellTabWindowClass: one per tab, Windows
// 10 has one; the top-level window should it have none): the folder on disk
// it shows (empty for Home, This PC, a search and the like), and taking it
// to another folder there, as going there in it would (its Back button
// comes back). Need COM.
// False when the tab is not among the shell's windows (they can miss one).
std::wstring explorerFolder(HWND tab);
// The exact Opus file-display tab; not whichever pane became active since
// its first key. Uses the existing bounded, unelevated dopusrt query.
std::wstring opusFolder(HWND tab);
bool goToInOpusView(HWND root, HWND tab, const std::wstring& folder, const std::wstring& select = {});
bool goTo(HWND tab, const std::wstring& folder);
// goTo, for a tab whose window was just handed the front with the keyboard
// on its files (the launcher opened by typing there, GitHub #39): first
// waits until it is in front with that focus again (half a second at most),
// then, once there, until the tab shows the folder (two seconds at most), so
// that the folder asked for next is the new one; the new folder's files get
// the keyboard. Waits on Explorer: not on the GUI thread. `select`: an item
// of the folder to select there (Ctrl+Enter, GitHub #60), once the view has
// it; the folder shown already is not navigated to again.
bool goToKeepingFocus(HWND tab, const std::wstring& folder, const std::wstring& select = {});

// The program that takes the commands: TOTALCMD64.EXE (TOTALCMD.EXE),
// dopusrt.exe, XYplorer.exe, pulse.exe, OneCommander.exe, the custom open
// command's; empty when it is not installed. A running copy (including a
// portable one) is preferred to the installed copy. Explorer: always empty.
std::wstring program(Kind kind);
bool installed(Kind kind); // Explorer and Custom always: Custom is to be set up

// With the user's normal rights, in a new tab (Directory Opus, XYplorer and
// Pulse: the tab that shows it already, if any; Pulse may open a new window
// instead, as it chooses for any folder from outside), starting the program
// if it does not run. False when it is not installed or did not start. Not
// for Explorer: that is shell::open's and shell::reveal's own.
bool openFolder(Kind kind, const std::wstring& folder);
// Directory Opus's lister used last, in its tab in front, goes to `folder`
// (its Back button comes back), as the folder menu's double click there
// asks. False when Opus is not installed.
bool goToInOpus(const std::wstring& folder);
// The folder shown in front in `window`, one of theirs (kindOf): the tab on
// top in File Explorer and Directory Opus, the panel with the focus in Total
// Commander, the tab with the focus in XYplorer, the pane with the focus in
// the tab on top in Pulse or OneCommander. Empty for a virtual location
// (Home, This PC, an archive), or another program's window. Needs COM, asks the
// programs: not on the GUI thread. For the folder menu's hotkey (GitHub #25).
std::wstring frontFolder(HWND window);
// The window of that program used last goes to `folder` where its user
// looks: Directory Opus's tab in front (goToInOpus), Total Commander's
// panel with the focus (its current tab), XYplorer's and Pulse's tab
// showing it already or a new one (openFolder; Pulse may take a new window
// for it), OneCommander's current pane. Not for Explorer (goTo takes its tab)
// or Custom (openFolder).
// False when it is not installed.
bool goToInFront(Kind kind, const std::wstring& folder);
// Directory Opus's Preferences window.
bool openOpusPreferences();
// Directory Opus's settings file (prefs.oxc); empty when there is none.
std::wstring opusPrefsFile();
bool opusRunning(); // with or without a window open
// Changes Directory Opus's settings file: `edit` gets what it holds and
// gives what it is to hold (nullopt: leave it). Opus reads the file only
// when it starts, and writes it when its Preferences are confirmed: one
// that runs is closed first and started again after (its windows come
// back, to the front: waited for, 3 s at most; one that ran in the tray
// only stays so). Takes seconds: not on the GUI thread.
enum class PrefsEdit {
    Done,
    OpusStayed, // Opus would not close (it asked about a copy still going, say)
    Failed, // the file could not be read or written
};
PrefsEdit editOpusPrefs(const std::function<std::optional<QByteArray>(const QByteArray&)>& edit);
// Each item's folder, with the item selected (in Total Commander, which has
// one cursor, and in Pulse, the first item of each folder; Pulse selects
// files only: a folder's folder opens as it is). OneCommander selects one
// file per folder through its public interface; folders' parents are opened
// without selection (the public setter navigates into a folder).
bool reveal(Kind kind, const std::vector<std::wstring>& paths);

} // namespace ws::filemanager
