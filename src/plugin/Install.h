#pragma once

// Installing and removing plugins (the plugin design, §5.1, §5.2): a folder,
// or a .zip / .wsplugin (unpacked with miniz into a temporary folder), is
// looked at first -- its manifest read and checked, the words of any error
// kept for the settings page -- and put into plugins\<id> once the user has
// answered what there is to ask (an older or newer version there already).

#include "Manifest.h"

#include <QString>

#include <optional>

namespace ws::plugin::install {

inline constexpr qint64 kMaxArchiveBytes = 256LL << 20; // of a zip, and of what it unpacks to

// What was given, looked at.
struct Candidate {
    QString source; // the folder or archive, as given
    QString dir; // where its files are now: the folder itself, or a temporary one (archive)
    bool archive = false; // `dir` is temporary: place() moves it, discard() removes it
    std::optional<Manifest> manifest; // nothing when `error` says why it cannot be installed
    QString error;
};

// Unpacks a .zip / .wsplugin held in memory into `dir` (made here): the
// plugin.json at its root or in its one folder, the files landing in `dir`
// either way; nothing outside it. Empty on success, else why.
QString unpackArchive(const QByteArray& bytes, const QString& dir);

// Reads `path` (a folder, or a .zip / .wsplugin). The candidate's `error`
// says why it is no plugin; nothing at all when `path` is not there.
// An archive is unpacked into a new folder under `unpackParent` (empty: the
// system's temporary folder).
std::optional<Candidate> inspect(const QString& path, const QString& unpackParent = {});
// A folder Win顺 unpacked itself earlier (a staged update): read like
// inspect() does, but `archive` is set, so place() moves it in and
// discard() removes it. Nothing when `dir` is no folder.
std::optional<Candidate> adopt(const QString& dir);
// What stands at `<userDir>\<id>` that place() will not put the plugin over
// (looked at before the user is asked): a link (a plugin in development), or
// a folder that is no installed plugin -- no plugin.json in it: an uninstall
// whose program held its files, the user's own files (GitHub #253).
enum class Obstacle { None, Link, Foreign };
Obstacle obstacle(const QString& userDir, const QString& id);
// A name beside `<userDir>\<id>` to rename such a folder to, not yet taken:
// `<id>.old-<date>-<time>`. No installed plugin is ever read from it.
QString asideName(const QString& userDir, const QString& id);
// Puts the candidate into `<userDir>\<id>`, replacing what is there (the
// caller has asked the user). The folder it went to, or empty with `error`.
// A target that is a link (junction, symlink: a plugin in development) is
// not replaced. A Foreign folder is left alone, unless `asideForeignTo`
// names a folder (asideName()) in `userDir` to rename it to first: nothing
// of it is deleted.
QString place(const Candidate& candidate, const QString& userDir, QString* error, const QString& asideForeignTo = {});
void discard(Candidate& candidate); // the temporary folder of an archive, if any

// Removes `<userDir>\<id>` and, when `data`, `<dataRoot>\<id>.data`. Why it
// could not be, or empty. All or nothing: a folder whose files are held open
// (its program still running) stays whole, to be removed again later.
QString remove(const QString& userDir, const QString& dataRoot, const QString& id, bool data);

// The permissions `manifest` asks for that `confirmed` (the names the user
// said yes to) does not have: what to ask about again.
QStringList unconfirmed(const Manifest& manifest, const QStringList& confirmed);

} // namespace ws::plugin::install
