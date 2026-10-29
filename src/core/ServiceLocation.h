#pragma once

#include <windows.h>

#include <string>
#include <string_view>

// Where the index service's executable must live before it is registered as a
// LocalSystem service (a local privilege-escalation fix). A portable copy of
// Win顺 sits in a user-writable folder (Downloads, Desktop, D:\tools...), so
// any program running with the user's normal rights could replace
// WinShunService.exe there and have it run as SYSTEM at the next service
// start. So `WinShunService.exe --install`, when it finds itself in such a
// place, copies itself into a folder only administrators / SYSTEM can write
// and registers that copy instead (src/service/main.cpp).
//
// Plain Win32 with no Qt, like the rest of the service; compiled into both the
// service and wscore (the settings read the registered path and judge it the
// same way, so no change to the pipe protocol is needed).
namespace ws::svcloc {

// What counts as "a non-administrator can tamper with this" depends on what
// the object is:
//  - Object:   the executable itself (or the folder being judged). Any right
//              that lets a non-admin write to, add to, delete or re-permission
//              it.
//  - Parent:   the folder the executable is in. The Ancestor rights, and also
//              adding a file or a folder to it: Windows looks for a program's
//              DLLs in the program's own folder before System32 (wtsapi32.dll,
//              say, is not a KnownDLL), so a DLL planted beside a SYSTEM
//              service would run as SYSTEM.
//  - Ancestor: a directory further up. Only the rights that let a non-admin
//              swap the child below it (delete a child, rename or re-permission
//              the directory, take its ownership). Merely being allowed to
//              create a *sibling* does not count — the default DACLs on C:\
//              and %ProgramData% grant exactly that to ordinary users, and
//              treating it as unsafe would flag every path, including the
//              official Program Files install.
enum class Role { Object, Parent, Ancestor };

// True when a principal that is not SYSTEM, the Administrators group or
// TrustedInstaller could modify this object (Role::Object), plant a file in
// it (Role::Parent) or replace the child inside it (Role::Parent and
// Role::Ancestor), judging only from its owner and DACL.
// A non-admin owner is always unsafe (an owner can rewrite the DACL); a NULL
// DACL (everyone, everything) is unsafe; DENY aces are ignored (they only
// take rights away, and an attacker would not add one); INHERIT_ONLY aces are
// skipped (they do not apply to this object). Pure: unit-tested with
// hand-built security descriptors.
bool writableByNonAdmin(PSID owner, PACL dacl, Role role) noexcept;

// Reads the owner and DACL of the executable `path` (as an Object), of its
// folder (as its Parent) and of every directory above it up to the volume root
// (as Ancestors); false if any is unsafe, or if any one of them cannot be read
// (unknown is treated as unsafe). `why`, if given, gets the first path that
// failed. A read-only check: it opens nothing for writing.
bool pathProtected(const std::wstring& path, std::wstring* why = nullptr);

// The same for a folder whose contents this code decides (the protected
// service folder, the log folder): `dir` as an Object — so no one else may add
// to it — and every directory above it as an Ancestor.
bool folderProtected(const std::wstring& dir, std::wstring* why = nullptr);

// The fixed protected folder a portable --install copies the service into:
// %ProgramFiles%\WinShun Service (SHGetKnownFolderPath, not the %ProgramFiles%
// environment variable, which a caller could spoof). Deliberately not under
// the installer's {app}: uninstalling the Program Files copy must not take a
// portable service's folder with it. Empty if Program Files cannot be found.
std::wstring protectedServiceDir();

// Ensures `dir` exists and only administrators / SYSTEM can write to it:
// creates it (Administrators as owner, Administrators & SYSTEM full control,
// Users read/execute, nothing inherited) when it is absent, or, when it
// already exists, verifies it with folderProtected and refuses an unsafe folder
// — a folder someone else made is never trusted. Needs administrator rights
// to create under Program Files. On failure `error`, if given, says why.
bool ensureProtectedDir(const std::wstring& dir, std::wstring* error);

// Copies `sourceExe` into `dir` as `name`, atomically and verified: reads the
// source once into memory, writes a CREATE_NEW temp file inside `dir` (which
// must already be protected, so nothing can pre-plant it), replaces the target
// with MoveFileEx(REPLACE_EXISTING), then checks the result is not writable by
// a non-admin and that its bytes equal what was read. The caller must stop the
// service first if the target is the running executable. On success `dest`,
// if given, gets the final path. On failure `error`, if given, says why.
bool copyExeInto(const std::wstring& dir, const std::wstring& name, const std::wstring& sourceExe,
    std::wstring* dest, std::wstring* error);

// For the service's log folder (%ProgramData%\WinShun), a lighter touch than
// ensureProtectedDir: ProgramData lets ordinary users add folders, so a user
// could pre-create WinShun\ (and own it) and plant a junction on the log file,
// turning a SYSTEM write into an arbitrary-file write. These do not refuse to
// log (that would slow the user's searches for no gain), they make it safe.

// Creates `dir`, succeeding if it already exists. When this process is
// elevated or SYSTEM (the index service, or --install) it gives a new folder a
// DACL only administrators and SYSTEM can write to (users may read); otherwise
// (an ordinary-user test writing into its own temp) it creates a plain folder,
// since that is not a privilege boundary. Does not change an existing folder's
// security — secureLogFolder does that. Cheap enough to call before each line.
bool createProtectedDir(const std::wstring& dir);

// Best-effort, run once (at --install with full rights, and at service start):
// makes `dir` writable only by administrators / SYSTEM even if it was there
// already and weak (an older version let it inherit ProgramData's user-writable
// ACL, or someone pre-created it). Sets Administrators as owner and a protected
// DACL when the process may; does only what it can. Callers still open the log
// files with FILE_FLAG_OPEN_REPARSE_POINT as a second line of defence.
void secureLogFolder(const std::wstring& dir);

// `path`'s parent directory equals `dir` (case-insensitively): used to tell
// whether a registered ImagePath is a copy this code placed in the protected
// folder, so --uninstall removes it (and the folder) but leaves the installer's
// {app} copy alone.
bool parentIs(const std::wstring& path, const std::wstring& dir);

// True when `clientImage` is a WinShun.exe in the very folder of
// `serviceImage`: both full paths as QueryFullProcessImageNameW and
// GetModuleFileNameW give them, compared without regard to case. The service
// offers its session helpers (the UIAccess double-tap listener, the elevated
// file-dialog navigator of GitHub #104) only to such a client, Win顺's own UI
// from the same install; a portable or development copy elsewhere is refused
// them. Pure: no file is opened.
bool isWinShunBeside(std::wstring_view clientImage, std::wstring_view serviceImage) noexcept;

} // namespace ws::svcloc
