#pragma once

// The `elevated` permission (a forum request: a plugin that drives an
// elevated file manager): "插件跟着主程序走". When Win顺 itself runs as
// administrator (管理员直读, a portable copy run as administrator, UAC off),
// a plugin that declares `elevated` and that the user allowed runs as
// administrator too, through a second runner started with Win顺's own
// token (Host); when Win顺 is not elevated it runs as every plugin does.
// Never a UAC prompt of its own. The aim: this adds no new way for a program
// running with the user's normal rights to get a process run as
// administrator without the user's own click.
//
// The catch: a plugin's folder (%LOCALAPPDATA%\WinShun\plugins\<id>, a
// developer's folder) is the user's to write, so any program running with
// the user's normal rights could change its exe, its manifest (runtime.env,
// args) or put a DLL beside it, and have it run as administrator the next
// time an elevated Win顺 (often started so at logon) starts it. So an
// elevated plugin never runs from that folder, nor with anything read from
// it. It runs from a copy the user confirmed: made by the elevated Win顺
// (the card's 确认; for a plugin of the library, from its package checked
// against the signed list), under %ProgramFiles%\WinShun Elevated Plugins\
// <user SID> (Program Files: ordinary users cannot make a folder there
// first), which only administrators and SYSTEM may change or even read,
// with a record of each file's SHA-256 beside it. Its manifest is the copy's,
// and must be the one the catalog has. Before an elevated start the plugin's
// folder is compared with the record: the same files, the copy runs as
// administrator; anything else (never confirmed, changed since, or the
// protected folder not what it must be), it runs from its own folder with
// the user's normal rights, and its card says why. The comparison only
// decides whether the confirmed copy is still the plugin as installed: a
// wrong answer either way runs confirmed bytes elevated or the user's bytes
// with normal rights, never the user's bytes elevated.
//
// Every folder of the store is opened without following a link, must not
// be one, must be owned by administrators (or SYSTEM) and let no one else
// change it; nothing found otherwise is repaired or adopted. The folders
// down to the copy stay open (no deleting or renaming them) from the check
// until the plugin's process has started: a Hold.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace ws::plugin::elevation {

// Where the confirmed copies live.
struct Store {
    QString root; // per user; empty: nothing confirmed, nothing kept
    // The root and the folder above it must be ones only administrators can
    // change, owned by them, and are made so when missing (only then).
    // False in tests, which keep their store in a temporary folder (links
    // are refused there too).
    bool requireProtected = true;
};
// %ProgramFiles%\WinShun Elevated Plugins\<the user's SID> (the known folder,
// not the environment variable, which a caller could set).
Store defaultStore();

// Win顺's own token is elevated.
bool processElevated();

// The store's folders down to a confirmed copy, held open so that none can
// be deleted, renamed or swapped for a link while it is held.
class Hold;

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
    std::shared_ptr<Hold> hold; // Match, when asked for: the folders held
};
// The plugin `id` in `pluginDir` against what was confirmed. Files are
// hashed once while their size and time stay as they were (a cache of this
// process's; see above for why a forged time cannot run anything not
// confirmed). `hold`: Match keeps the folders open in `Result::hold`.
Result check(const Store& store, const QString& id, const QString& pluginDir, bool hold = false);

// Confirms the files in `pluginDir` now: copies them into the store, each
// file read once and its SHA-256 taken of the very bytes written. Links
// (junctions, symbolic links, hard links) are refused, the folder itself
// one too. False with `error`. Blocking (hashes and copies): off the GUI
// thread.
bool confirmFolder(const Store& store, const QString& id, const QString& pluginDir, QString* error);
// Confirms a plugin of the library from its package (already checked
// against the signed list): its files are what is kept, and they must be
// the ones in `pluginDir`, else nothing is confirmed (`error`).
bool confirmPackage(const Store& store, const QString& id, const QString& pluginDir, const QByteArray& package, QString* error);
// An update of a plugin confirmed before (a record exists): the package's
// files, unpacked from these bytes (already checked against the signed
// list) straight into the store, become what is confirmed too, so the
// update keeps running elevated once it is put in place. Nothing done (and
// true) when the plugin was never confirmed; an older version than the one
// confirmed is refused.
bool keepConfirmed(const Store& store, const QString& id, const QString& pluginDir, const QByteArray& package, QString* error);
bool hasRecord(const Store& store, const QString& id, const QString& pluginDir);
// Uninstalled: its copies go (only an elevated Win顺 can; else sweep()
// does it the next time Win顺 runs elevated).
void forget(const Store& store, const QString& id, const QString& pluginDir);
// The copies of plugins no longer installed (`installed` says which are),
// removed. How many.
int sweep(const Store& store, const std::function<bool(const QString& id, const QString& pluginDir)>& installed);
// A temporary folder of the plugin's own in the store (TEMP and TMP of its
// elevated process); empty when there is none to be had.
QString tempDir(const Store& store, const QString& id, const QString& pluginDir);

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
// tests and the record. Empty with `error` for a link.
struct File {
    QString path; // '/' separated
    qint64 size = 0;
    QByteArray sha256; // hex
    friend bool operator==(const File&, const File&) = default;
};
QList<File> listFiles(const QString& dir, QString* error);

} // namespace ws::plugin::elevation
