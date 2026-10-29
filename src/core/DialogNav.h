#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

// Elevated-safe navigation of another program's Open / Save / folder-picker
// dialog, for GitHub #104. Win顺's UI runs with the user's normal rights
// (#21); UIPI blocks the window messages and input it would send to an
// elevated dialog, and it cannot open an elevated process for memory
// (measured: OpenProcess(VM_WRITE) -> ERROR_ACCESS_DENIED). So the index
// service launches a small uiAccess + high-integrity helper in the user's
// session (service/InputListener already does this for double Ctrl) and that
// helper drives the dialog here.
//
// Nothing in this module reads or writes another process's memory, injects
// code, presses OK / Open / Save, or types a file name: only window messages
// the system marshals (WM_SETTEXT, a keyboard Enter in the address bar, an
// Up / Down arrow pair to the folder tree) and UI Automation (ExpandCollapse /
// SelectionItem on the folder tree) -- exactly the operations a uiAccess
// accessibility tool is meant to perform. That keeps it off the
// elevation-of-privilege path (docs/architecture.md 权限分离).
//
// This is a deliberately elevated-only variant of platform/FileDialog.cpp's
// navigation; keep the classification and the address-bar / Inno paths in
// step with it. It is Qt-free so the service executable can link it.
namespace ws::dialognav {

enum class Kind {
    None, // not one we navigate from the helper
    Open, // IFileOpenDialog / GetOpenFileName without a hook
    Save,
    Folder, // a folder picker (FOS_PICKFOLDERS), or Inno Setup's destination picker
    Legacy, // the Windows XP style (no address bar): left to the copy-only fallback
    Browse, // SHBrowseForFolder's tree ("浏览文件夹"), old and new style
};

// What `dialog` is, by its window structure alone (no WPS Office / Kingsoft:
// that stays copy-only when elevated). Quick; any thread.
Kind classify(HWND dialog);

// True for an existing local directory. The helper runs as the user, so this
// is checked with the user's own identity.
bool isExistingDirectory(const wchar_t* folder);

// The name a folder tree shows for `folder` when its desktop.ini gives it a
// localized one ("用户" for C:\Users, "下载" for Downloads); empty when it has
// none. Without the shell namespace (so no handler a registry entry names is
// loaded into the helper): SHGetLocalizedName gives a module and a string id,
// and the string is read from that module mapped as data (LoadLibraryExW
// AS_DATAFILE | AS_IMAGE_RESOURCE: no code of it runs), only when no
// non-administrator can change the module (svcloc::pathProtected: System32,
// Program Files). Names a namespace extension gives a folder of its own are
// not covered: the tree walk then keeps the copy fallback.
std::wstring localizedName(const std::wstring& folder);

// The longest folder a navigation request may name: the classic MAX_PATH
// (the helper is not long-path aware, so it could not check a longer one).
inline constexpr std::size_t kMaxFolderChars = MAX_PATH - 1;

// A string-only check of a requested folder, made by the service before it
// builds the helper's command line (it never touches the path itself: that
// would be as LocalSystem) and again by the helper. True for an absolute
// drive path ("C:\", "D:\a b") or UNC path ("\\server\share\x") of at most
// kMaxFolderChars, with no control characters, none of " < > | ? * / and no
// ':' but the drive's, no "." / ".." or empty segments (one trailing '\' is
// fine), and no \\.\ device path.
bool isValidFolderRequest(std::wstring_view folder);

// `argument` quoted for a CreateProcess command line so that
// CommandLineToArgvW (and the C runtime's argv) give it back unchanged: the
// backslashes before a quote, the closing one included, are doubled ("C:\"
// becomes "C:\\"), an embedded quote is escaped.
std::wstring quoteArgument(std::wstring_view argument);

// Takes `dialog` to `folder` (an existing directory), elevated-safe as above.
// `folder` must pass isValidFolderRequest, and each route checks that it is
// still a directory right before it acts (the address bar gets it with a
// trailing backslash, so it can only resolve a folder).
// False when it did not go: an unsupported kind (Legacy / None), a folder that
// is gone, a tree segment the UI Automation walk could not match (a hidden
// folder, say), or a new-style tree whose selection did not take. The caller
// keeps the copy-and-paste fallback then.
bool navigateToFolder(HWND dialog, const std::wstring& folder);

} // namespace ws::dialognav
