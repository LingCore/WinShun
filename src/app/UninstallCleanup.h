#pragma once

namespace ws::uninstall {

// `WinShun.exe --uninstall-cleanup`: takes back what THIS copy registered
// with Windows, before its files are deleted (Scoop's pre_uninstall hook runs
// it, bucket/winshun.json; the Inno Setup uninstaller does the same in its
// own code). Only this copy's: an autostart task or Run value, Explorer's
// right-click verbs, the ws folder on the user's PATH, Directory Opus's
// double click, Win+V / Win+S given back to Windows (when no other copy is
// installed or starts at logon), and the index service when this copy
// installed it (UAC asks). A Scoop copy's version folder and "current" count
// as one copy (core/ScoopInstall.h). The user's data is not touched.
//
// First ends this copy if it runs (the plugin host and the other programs
// from its folder too): without that Scoop cannot remove the folder and the
// registrations would be written again. When something of it will not end,
// nothing is changed. Writes what it did to standard output (UTF-8), or the
// parent's console. 0 on success, 3 when something of this copy still runs,
// else 1 if any step failed.
int cleanup();

} // namespace ws::uninstall
