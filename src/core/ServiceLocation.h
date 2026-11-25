#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

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

// What is wrong with one file or folder, so the settings can say it and how
// to put it right instead of "restore the default permissions" (GitHub #426).
struct Weakness {
    enum class Kind {
        None, // protected
        Owner, // owned by an account that is not an administrator's
        Rights, // an account that is not an administrator's may change it
        NoAcl, // no DACL at all: everyone, everything
        Unreadable, // its security cannot be read (or it is not what was expected)
    };
    Kind kind = Kind::None;
    std::wstring path; // the file or folder at fault
    std::wstring sid; // Owner: its owner; Rights: who may change it ("S-1-5-11")
    DWORD mask = 0; // Rights: the access that entry grants
    DWORD error = 0; // Unreadable: the Win32 error
    explicit operator bool() const noexcept { return kind != Kind::None; }
};

// The first thing wrong with this owner and DACL, judged as
// writableByNonAdmin does (its `path` left empty). Pure.
Weakness weakness(PSID owner, PACL dacl, Role role);

// The same for the file or folder at `path`, read from the disk.
Weakness weaknessOf(const std::wstring& path, Role role);

// The account a string SID names, without its machine or domain
// ("Authenticated Users", "fan"); the SID itself where Windows knows no name
// for it.
std::wstring accountName(const std::wstring& sid);

// The commands (for an administrator's Command Prompt, in order) that put
// right what `weak` names and nothing else: a non-administrator owner gives
// the folder to the Administrators group; an account that may change it keeps
// reading and running only. Not recursive: the folder itself, and what
// inherits from it. Empty where there is no safe general command.
std::vector<std::wstring> repairCommands(const Weakness& weak);

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

// Checks every directory strictly above `path` as an Ancestor, without
// requiring `path` itself to exist yet (used before creating a protected folder
// and by the UI to diagnose why no protected location was usable).
bool ancestorsProtected(const std::wstring& path, std::wstring* why = nullptr);

// The fixed protected folders a portable --install may copy the service into,
// in preference order: %ProgramFiles%\WinShun Service, %ProgramData%\WinShun
// Service, %ProgramFiles(x86)%\WinShun Service (all via SHGetKnownFolderPath,
// never spoofable environment variables). Deliberately not under the
// installer's {app}: uninstalling the Program Files copy must not take a
// portable service's folder with it.
std::vector<std::wstring> protectedServiceDirs();

// The primary protected service folder (the first entry of protectedServiceDirs),
// or empty if none can be found.
std::wstring protectedServiceDir();

// True when `path`'s parent directory is one of protectedServiceDirs().
bool inProtectedServiceDir(const std::wstring& path);

// Read-only check: true if at least one candidate in `candidates` either exists
// and passes folderProtected(), or does not exist yet while its parent exists
// and every ancestor passes ancestorsProtected(). Otherwise writes the first
// thing wrong, the first candidate's, to `firstWeak` (if given) and returns
// false: putting that one right is enough.
bool anyProtectedDirUsable(const std::vector<std::wstring>& candidates, Weakness* firstWeak = nullptr);

// Ensures `dir` exists and only administrators / SYSTEM can write to it:
// creates it (Administrators as owner, Administrators & SYSTEM full control,
// Users read/execute, nothing inherited) when it is absent, or, when it
// already exists, verifies it with folderProtected and refuses an unsafe folder
// — a folder someone else made is never trusted. Needs administrator rights
// to create under Program Files / ProgramData. On failure `error`, if given,
// says why.
bool ensureProtectedDir(const std::wstring& dir, std::wstring* error);

// Tries `candidates` in order with ensureProtectedDir(), placing the first
// usable directory into `chosen`. If none are usable, combines their errors
// into `error` and returns false.
bool ensureAnyProtectedDir(
    const std::vector<std::wstring>& candidates, std::wstring& chosen, std::wstring* error);

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
// GetModuleFileNameW give them, compared without regard to case. The
// protected clipboard display starts that very WinShun.exe with UIAccess, so
// only such a client gets it. Pure: no file is opened.
bool isWinShunBeside(std::wstring_view clientImage, std::wstring_view serviceImage) noexcept;

// Whether the service navigates elevated file dialogs (GitHub #104) for
// `clientImage`: Win顺's own UI, the WinShun.exe beside the service, or the
// one `installedFrom` names. That is the copy whose --install (run by an
// administrator: the installer, or a portable copy's settings) had to put
// the service in protectedServiceDir because its own folder is not protected
// (an all-users install to D:\Softwares, a portable copy); --install records
// it on the service's command line, which only administrators can change.
// Empty `installedFrom`: beside only. Compared as full paths, without regard
// to case. Pure.
bool servesDialogNav(std::wstring_view clientImage, std::wstring_view serviceImage, std::wstring_view installedFrom) noexcept;

} // namespace ws::svcloc
