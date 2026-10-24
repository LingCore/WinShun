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

// An entry made comparable: unquoted, %VAR% expanded, native separators, no
// closing '\', a short name (PROGRA~1) in its long form.
QString normalized(const QString& entry);
bool sameFolder(const QString& a, const QString& b);
bool hasEntry(const QString& path, const QString& folder);
// `path` with `folder` at the end, unless an entry names it already. Every
// other character stays as it was (quotes, %VAR%, empty entries).
QString withEntry(const QString& path, const QString& folder);
// `path` without the entries that name `folder` (each with one ';'); the
// rest stays as it was.
QString withoutEntry(const QString& path, const QString& folder);

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
struct RawValue {
    QString text;
    DWORD type = REG_EXPAND_SZ;
    bool present = false;
};
RawValue readValue(HKEY root, const std::wstring& key, const wchar_t* name);
// Writes the value with `type` (a key that is not there is made); the Win32 error, 0 when it worked.
DWORD writeValue(HKEY root, const std::wstring& key, const wchar_t* name, const QString& text, DWORD type);

// What the analysis needs, read from the registry (readSources) or made up (tests).
struct Sources {
    QString systemPath; // raw
    QString userPath; // raw
    QStringList pathExt; // ".COM", ".EXE" ...: what a name without an extension may be completed with
    QString recorded; // the folder WinShun put on the user's PATH, if it did
};
Sources readSources(const Locations& where = {});

struct Status {
    enum class Where { None, System, User };
    QString bin; // native
    QString exe; // ws.exe in it; empty when it is not there
    Where where = Where::None; // which list names bin (system first)
    bool removable = false; // the user's list names bin and WinShun put it there
    QString shadow; // another program called ws that comes first on the PATH (with ours not on it yet: any); its path

    bool onPath() const noexcept { return where != Where::None; }
};
// `exists`: whether a file is there (the disk, off the GUI thread: a PATH
// entry may be a drive that is gone, or a share).
Status analyse(const QString& bin, const Sources& sources, const std::function<bool(const QString&)>& exists);
Status probe(const Locations& where = {}); // the registry and the disk, for a worker thread
QVariantMap toVariant(const Status& status); // for QML: {bin, exe, exeFound, onPath, where ("system" | "user" | ""), removable, shadow}

// ---- putting bin on the user's PATH, and taking it off again ----

// Appends bin to the user's PATH (no change when it names it already, and
// then it is not noted as WinShun's). The Win32 error; 0 when it worked.
// Telling the other programs (WM_SETTINGCHANGE "Environment") is up to the caller.
DWORD addToUserPath(const QString& bin, const Locations& where = {});
// Takes it out again, if WinShun put it there. ERROR_NOT_FOUND when it did not.
DWORD removeFromUserPath(const QString& bin, const Locations& where = {});

} // namespace ws::cli
