#pragma once

// The command line ws (src/cli, docs/design-cli.md) as the settings page sees
// it: where ws.exe is, whether its folder is on the PATH, whether another
// program called ws comes first, and putting the folder on the user's PATH.
// The PATH is read from the registry, not from this program's environment
// (which is as old as this program's start); the system list first, then the
// user's, the order Windows joins them in.

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <windows.h>

#include <functional>
#include <string>

namespace ws::cli {

// <the folder of this program>\bin, native separators: ws.exe lives alone in
// it, and it is the only folder that goes on the PATH (the install folder
// holds Qt's DLLs, which other programs would pick up from the PATH).
QString binFolder();
// ws.exe in `bin`; empty when it is not there.
QString wsExe(const QString& bin);

// ---- a PATH as text: entries between ';', as the registry keeps them ----

// The entries of a PATH, as Windows reads it: between ';', but a ';' inside
// double quotes belongs to the entry ("C:\a;b\bin" is one). Empty entries and
// quotes stay as written, so that joining the list with ';' gives `path` back.
QStringList splitPath(const QString& path);
// How `folder` is written on a PATH: native separators, in quotes when its name holds a ';'.
QString entryOf(const QString& folder);

// An entry made comparable: unquoted, %VAR% expanded, native separators, no
// closing '\', a short name (PROGRA~1) in its long form. Only a name with a
// '~' in it is asked of the file system: GetLongPathName on a disconnected
// share or drive stalls for many seconds, and this runs for every entry.
QString normalized(const QString& entry);
bool sameFolder(const QString& a, const QString& b);
bool hasEntry(const QString& path, const QString& folder);
// `path` with `folder` at the end, unless an entry names it already. Every
// other character stays as it was (quotes, %VAR%, empty entries).
QString withEntry(const QString& path, const QString& folder);
// Whether an entry reads exactly `entry` (character for character, not
// another spelling of the same folder).
bool hasExactEntry(const QString& path, const QString& entry);
// `path` without its last entry that reads exactly `entry` (with one ';'); the
// rest stays as it was. Another spelling of the folder is the user's, not ours.
QString withoutExactEntry(const QString& path, const QString& entry);

// ---- the registry ----

// Where the lists are. Replaced in the tests, which must not touch the real ones.
struct Locations {
    HKEY systemRoot = HKEY_LOCAL_MACHINE;
    std::wstring systemKey = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    HKEY userRoot = HKEY_CURRENT_USER;
    std::wstring userKey = L"Environment";
    // The folder WinShun itself put on the user's PATH is noted here (value
    // "CliPathEntry"): an entry the installer wrote, or the user, is not WinShun's to take out.
    HKEY recordRoot = HKEY_CURRENT_USER;
    std::wstring recordKey = L"Software\\WinShun";
};

// A value as stored. Read raw: a PATH is REG_EXPAND_SZ, and expanding it on
// the way in would write the expanded text back out (QSettings does).
//
// Only ERROR_FILE_NOT_FOUND means "not there" (present false, error 0). Any
// other failure (a value of another type: ERROR_UNSUPPORTED_TYPE; no access;
// one that kept growing: ERROR_MORE_DATA) is `error`, and the value is neither
// there nor absent: whoever writes must not take it for an empty PATH.
struct RawValue {
    QString text;
    DWORD type = REG_EXPAND_SZ;
    bool present = false;
    DWORD error = 0;
};
// `betweenReads` runs between asking the size and reading (the tests grow the
// value there); the read starts over when the value outgrew its buffer.
RawValue readValue(HKEY root, const std::wstring& key, const wchar_t* name, const std::function<void()>& betweenReads = {});
// Writes the value with `type` (a key that is not there is made); the Win32 error, 0 when it worked.
DWORD writeValue(HKEY root, const std::wstring& key, const wchar_t* name, const QString& text, DWORD type);
// Deletes the value; true when it is gone (or was not there).
bool deleteValue(HKEY root, const std::wstring& key, const wchar_t* name);
// Deletes the key when nothing is in it (no values, no subkeys); true when it is gone.
bool deleteKeyIfEmpty(HKEY root, const std::wstring& key);

// What the analysis needs, read from the registry (readSources) or made up (tests).
struct Sources {
    QString systemPath; // raw
    QString userPath; // raw
    QStringList pathExt; // ".COM", ".EXE" ...: what a name without an extension may be completed with
    // The entry exactly as WinShun wrote it on the user's PATH (entryOf), if it did.
    QString recorded;
    DWORD pathError = 0; // a PATH that could not be read (RawValue::error), the first: its text above is empty
};
Sources readSources(const Locations& where = {});

struct Status {
    enum class Where { None, System, User };
    QString bin; // native
    QString exe; // ws.exe in it; empty when it is not there
    Where where = Where::None; // which list names bin (system first)
    bool removable = false; // the user's list names bin and WinShun put it there
    QString shadow; // another program called ws that comes first on the PATH (with ours not on it yet: any); its path
    DWORD pathError = 0; // a PATH list could not be read (Sources::pathError): what is said of the PATH may be incomplete

    bool onPath() const noexcept { return where != Where::None; }
};
// `exists`: whether a file is there (the disk, off the GUI thread: a PATH
// entry may be a drive that is gone, or a share).
Status analyse(const QString& bin, const Sources& sources, const std::function<bool(const QString&)>& exists);
Status probe(const Locations& where = {}); // the registry and the disk, for a worker thread
QVariantMap toVariant(const Status& status); // for QML: {bin, exe, exeFound, onPath, where ("system" | "user" | ""), removable, shadow, pathError}

// ---- putting bin on the user's PATH, and taking it off again ----

// Appends bin to the user's PATH. No change, and nothing noted as WinShun's,
// when the system's list or the user's names it already (the installer's, or
// the user's own). The entry is noted first (a failed note stops everything),
// then the PATH is read again just before it is written, and written only if
// no one changed it meanwhile; a copy of WinShun moved elsewhere takes its old
// entry along (the noted one, if it is still there). A PATH that cannot be
// read as text (another registry type, no access) is left exactly as it is,
// and its error comes back. The Win32 error; 0 when it worked.
// Telling the other programs (WM_SETTINGCHANGE "Environment") is up to the caller.
DWORD addToUserPath(const QString& bin, const Locations& where = {});
// Takes out the entry WinShun put there (the noted one, character for
// character), nothing else. ERROR_NOT_FOUND when it did not put `bin` there; a
// PATH that cannot be read comes back as its error, the note untouched.
DWORD removeFromUserPath(const QString& bin, const Locations& where = {});

} // namespace ws::cli
