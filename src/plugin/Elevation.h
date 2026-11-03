#pragma once

// The `elevated` permission (a forum request: a plugin that drives an
// elevated file manager): "插件跟着主程序走". When Win顺 itself runs as
// administrator (管理员直读, a portable copy run as administrator, UAC off),
// a plugin that declares `elevated` and that the user allowed runs as
// administrator too, through a second runner started with Win顺's own
// token (Host); when Win顺 is not elevated it runs as every plugin does.
// Never a UAC prompt of its own.
//
// The catch: a plugin's folder (%LOCALAPPDATA%\WinShun\plugins\<id>, a
// developer's folder) is the user's to write, so any program running with
// the user's normal rights could change its exe or put a DLL beside it,
// and have it run as administrator the next time an elevated Win顺 (often
// started so at logon) starts it. So an elevated plugin never runs from
// that folder. It runs from a copy the user confirmed: made by the elevated
// Win顺 (the card's 确认, the permission dialog said yes to while elevated,
// or an update of a confirmed plugin whose package matched the signed
// list), under %ProgramData%\WinShun\elevated-plugins\<user SID>, which
// only administrators and SYSTEM may change (svcloc's checks), with a
// record of each file's SHA-256 beside it. Before an elevated start the
// plugin's folder is compared with the record: the same files, the copy
// runs as administrator; anything else (never confirmed, changed since, or
// the protected folder not what it must be), it runs from its own folder
// with the user's normal rights, and its card says why. What runs as
// administrator is the protected copy itself, so nothing done to the
// user's folder between the check and the start (or after it: a DLL put
// beside the exe) reaches it. The comparison only decides whether that copy
// is still the plugin as installed.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace ws::plugin::elevation {

// Where the confirmed copies live.
struct Store {
    QString root; // per user; empty: nothing confirmed, nothing kept
    // The root and every folder above it must be ones only administrators
    // can change (svcloc::folderProtected), and are made so when missing.
    // False in tests, which keep their store in a temporary folder.
    bool requireProtected = true;
};
// %ProgramData%\WinShun\elevated-plugins\<the user's SID> (the known folder,
// not the environment variable, which a caller could set).
Store defaultStore();

// Win顺's own token is elevated.
bool processElevated();

enum class Check {
    Match, // the folder holds what was confirmed: `dir` is the copy to start
    NoRecord, // never confirmed (or the record is gone)
    Changed, // confirmed, but the folder holds other files now
    Unavailable, // the store is not safe or cannot be read: `detail` says why
};
struct Result {
    Check check = Check::NoRecord;
    QString dir; // Match: the protected copy
    QString version; // the version confirmed (the newest), for the card
    QString detail;
};
// The plugin `id` in `pluginDir` against what was confirmed. Files are
// hashed once while their size and time stay as they were (a cache of this
// process's: nothing outside it can change it).
Result check(const Store& store, const QString& id, const QString& pluginDir);

// Confirms the files in `pluginDir` now: copies them into the store, each
// file read once and its SHA-256 taken of the very bytes written. Links
// (junctions, symbolic links) inside the folder are refused; the folder
// itself may be one (a plugin in development). False with `error`.
bool confirmFolder(const Store& store, const QString& id, const QString& pluginDir, QString* error);
// An update of a plugin confirmed before (a record exists): the package's
// files, unpacked from these bytes (already checked against the signed
// list) straight into the store, become what is confirmed too, so the
// update keeps running elevated once it is put in place. Nothing done (and
// true) when the plugin was never confirmed.
bool keepConfirmed(const Store& store, const QString& id, const QString& pluginDir, const QByteArray& package, QString* error);
bool hasRecord(const Store& store, const QString& id, const QString& pluginDir);
void forget(const Store& store, const QString& id, const QString& pluginDir); // uninstalled

// What a plugin's start comes to.
enum class Verdict {
    NotAsked, // it does not declare `elevated`
    HostNotElevated, // Win顺 is not elevated: as every plugin
    Elevated, // from the protected copy, as administrator
    Unconfirmed, // with normal rights until the user confirms
    Changed, // with normal rights: its files changed since they confirmed
    Unavailable, // with normal rights: the store is not safe
};
Verdict decide(bool declares, bool hostElevated, Check check);
QString verdictName(Verdict verdict); // "elevated", "unconfirmed" … ("" for the first two)

// The relative paths and SHA-256 (hex) of a folder's files, sorted, for the
// tests and the record. Empty with `error` for a link inside.
struct File {
    QString path; // '/' separated
    qint64 size = 0;
    QByteArray sha256; // hex
    friend bool operator==(const File&, const File&) = default;
};
QList<File> listFiles(const QString& dir, QString* error);

} // namespace ws::plugin::elevation
