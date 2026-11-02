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

// Reads `path` (a folder, or a .zip / .wsplugin). The candidate's `error`
// says why it is no plugin; nothing at all when `path` is not there.
// An archive is unpacked into a new folder under `unpackParent` (empty: the
// system's temporary folder).
std::optional<Candidate> inspect(const QString& path, const QString& unpackParent = {});
// A folder Win顺 unpacked itself earlier (a staged update): read like
// inspect() does, but `archive` is set, so place() moves it in and
// discard() removes it. Nothing when `dir` is no folder.
std::optional<Candidate> adopt(const QString& dir);
// Puts the candidate into `<userDir>\<id>`, replacing what is there (the
// caller has asked the user). The folder it went to, or empty with `error`.
// A target that is a link (junction, symlink: a plugin in development) is
// not replaced.
QString place(const Candidate& candidate, const QString& userDir, QString* error);
void discard(Candidate& candidate); // the temporary folder of an archive, if any

// Removes `<userDir>\<id>` and, when `data`, `<dataRoot>\<id>.data`. Why it
// could not be, or empty.
QString remove(const QString& userDir, const QString& dataRoot, const QString& id, bool data);

// The permissions `manifest` asks for that `confirmed` (the names the user
// said yes to) does not have: what to ask about again.
QStringList unconfirmed(const Manifest& manifest, const QStringList& confirmed);

} // namespace ws::plugin::install
