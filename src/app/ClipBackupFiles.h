#pragma once

#include <QDateTime>
#include <QString>

// A backup of the clipboard history: one zip the user names,
// "WinShun-clipboard-2026-11-25-194700.zip". Inside, "clipboard.db" (the
// entries, their notes, groups and shortcuts), "images/…" (the pictures),
// "projects/…" (annotated pictures' editable forms) and "backup.json" (when,
// which version, how many entries).
//
// Its own file, made when asked for: the settings' backups (BackupFiles.h)
// leave the history out, which is private, large, and changes with every
// copy. Restoring replaces the history there is with the backup's, whole
// (ids, order, groups, pictures as they were): unpacked beside it, put in
// its place by the next start, before anything opens it (applyStaged).
//
// Qt Core, Qt Sql and miniz only; any thread.
namespace ws::clipbackup {

struct Summary {
    QString problem; // empty: fine
    int entries = 0;
    int pictures = 0;
};

// "WinShun-clipboard-2026-11-25-194700.zip"
QString fileName(const QDateTime& when);

// The history in `clipFolder` (clipboard.db, images\, projects\) into the zip
// at `zipPath`, replaced only once the whole of it is written. The database
// goes in as of one moment (a copy SQLite makes), whatever is copied
// meanwhile; a picture dropped meanwhile is left out.
Summary pack(const QString& clipFolder, const QString& zipPath, const QString& version, const QDateTime& when);

// Unpacks the backup at `zipPath` into `folder` (made here, anything in it
// removed first) as a history folder. Only the names a backup has are taken;
// anything else in it, a damaged entry, or a database that does not read
// fails the whole backup.
Summary unpack(const QString& zipPath, const QString& folder);

// Puts an unpacked backup (`staged`) in the place of `clipFolder`, which is
// removed once the new one is there; put back if that fails, `staged` kept.
// Empty on success, else why.
QString apply(const QString& staged, const QString& clipFolder);

// Where a backup to restore waits for the next start, beside the history in
// the data folder; and the history's own folder.
QString stagedFolder(const QString& dataFolder);
QString historyFolder(const QString& dataFolder);

// At start, before the history is opened: a backup waiting is put in place.
enum class Start { Nothing, Restored, Failed };
struct Started {
    Start what = Start::Nothing;
    QString problem; // Failed: why
};
Started applyStaged(const QString& dataFolder);
// What applyStaged() did in this process, to tell the user once.
Started started();

} // namespace ws::clipbackup
