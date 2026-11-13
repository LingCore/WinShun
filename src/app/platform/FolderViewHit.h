#pragma once

#include <windows.h>

#include <string>

// Where a double click landed, for the folder menu (GitHub #22): on the blank
// space of a File Explorer folder view or of the desktop, or elsewhere. And
// which window the menu's hotkey (GitHub #25) found in front.
namespace ws::folderview {

enum class Surface {
    None,
    Explorer, // a folder view in a File Explorer window
    Desktop, // the desktop's icons; also its bare canvas for the folder menu
    Opus, // a Directory Opus file display: blank space told by Opus itself (FolderMenu::requested), or its lister in
          // front
    // The hotkey only (FolderMenu::hotkeyPressed):
    TotalCommander, // its window in front: the panel with the focus
    XYplorer, // its window in front: the tab with the focus
    Pulse, // its window in front: the pane with the focus in the tab on top (GitHub #41)
    OneCommander, // its public CurrentPathGet/CurrentPathSet automation elements
    Dialog, // an Open or Save dialog in front: chosen, it goes there
    Other, // any other window: chosen, the file manager opens it
};

struct Hit {
    Surface surface = Surface::None;
    // The window clicked on: DirectUIHWND (File Explorer's items view), or
    // SysListView32 (the desktop's; File Explorer's own when ExplorerPatcher
    // or a Windhawk mod brings the classic list view back, GitHub #45).
    // classifyMenu also accepts the desktop's host / folder-view background.
    HWND view = nullptr;
    HWND tab = nullptr; // the Explorer tab (ShellTabWindowClass: one per tab on Windows 11, the only one on Windows 10)
    HWND root = nullptr; // the top-level window
    POINT pt {}; // physical pixels: where the menu goes
};

// Which surface `window` (under the pointer, at `pt`) belongs to, by window
// classes alone: quick, sends no messages, for any thread. Everything else
// (the title bar, the navigation pane, the address bar, a name being edited,
// another file manager, whatever covers the desktop) is None. File
// Explorer's classic list view (ExplorerPatcher, Windhawk; GitHub #45)
// counts only with `classicList`: the folder menu has been tried there; the
// keys of type to search and the rest have not.
Hit classify(HWND window, POINT pt, bool classicList = false);

// The folder menu also accepts the bare desktop canvas when its icons are
// hidden. Kept separate so typing/paste focus still requires an items view.
Hit classifyMenu(HWND window, POINT pt);

// Explorer can give the desktop click and foreground to different shell
// hosts (Progman/WorkerW). Never accepts a different application's window.
bool sameDesktop(HWND first, HWND second);

// Whether the top-level window `root` is a File Explorer folder window (the
// tab on top is the Hit's) or the desktop, by window classes alone; None
// for anything else. For the hotkey, which has no point: `pt` is left zero.
Hit classifyWindow(HWND root);

// Opus typing: exactly its file-display window, never an Edit/FAYT/path
// field nested under it. Class queries only: safe on the keyboard hook.
bool isOpusLister(HWND root);
bool isOpusFileDisplay(HWND root, HWND focus);
// The file display of the lister `root` that a key typed now belongs to: the
// one with the keyboard focus, with no menu open over it; null otherwise.
// Opus draws its menus itself (dopus.button.display.popup), so the thread is
// in no menu mode and the focus stays on the files: what tells is that the
// menu holds the mouse (GitHub #311). GetGUIThreadInfo only, which sends the
// other thread nothing: safe on the keyboard hook.
HWND opusTypingView(HWND root);
// The bounds of the lister's file displays shown, together (both sides of a
// dual display); empty for none. Screen pixels (GitHub #82).
RECT opusFileLists(HWND root);
// The file list of the File Explorer tab `tab` (a Hit's) in its window
// `root`: its folder view shown (SHELLDLL_DefView: the items, without the
// navigation, details and preview panes; the largest, should two be there),
// less the scroll bars it shows (a box put in its corner keeps clear of
// them as of its edges, GitHub #234); none, that tab; nor that, the window's
// client area. Screen pixels; empty only for no window at all. Windows 10's
// one tab and the tabs of Windows 11 alike, the hidden ones never (GitHub
// #203). Class names only: any thread.
RECT explorerFileList(HWND root, HWND tab);

// What isBlank went by, for the diagnostics journal (GitHub #45): class
// names, roles and results only, never an item's name.
struct BlankTrace {
    const char* by = ""; // "uia", "msaa", or "desktop-background"
    std::wstring uiaClass; // UI Automation's element at the point; empty: not asked, or none
    long uiaControlType = 0;
    long uiaResult = 0; // ElementFromPoint's HRESULT
    bool uiaOtherProcess = false; // the element is another program's (something covers the view now)
    std::wstring msaa; // the accessibility walk: "LIST(33) | DISP LIST(33) | SELF LIST(33)"
};

// Whether the point is on blank space there: no item or its cells, no group
// header, column header or scroll bar. File Explorer's items view is asked
// through UI Automation (its element's class name, Windows 10 and 11 alike),
// the classic lists (the desktop's) through Microsoft Active Accessibility
// (accHitTest); each takes one to a few milliseconds and waits on Explorer:
// on a COM thread of its own, never the GUI's. The bare desktop canvas is
// rechecked with WindowFromPoint, without an icons accessibility walk.
// Unsure is not blank.
bool isBlank(const Hit& hit, BlankTrace* trace = nullptr);

// The accessibility walk isBlank makes for a classic list, run anywhere for
// its trace alone (the journal's, when UI Automation decided).
std::wstring accessibleTrace(const Hit& hit);

// The window classes from `window` up to its top-level window:
// "DirectUIHWND<SHELLDLL_DefView<…<CabinetWClass". Class names only: any thread.
std::wstring classChain(HWND window);

// Whether `window` is a shell folder view's window (SHELLDLL_DefView's
// child), whatever it is and wherever: a double click there that classify()
// turned down is worth telling. Class names only: any thread.
bool inShellView(HWND window);

} // namespace ws::folderview
