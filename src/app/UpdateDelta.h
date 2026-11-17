#pragma once

#include "Release.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace ws::update {

class HeldFile;

// Delta updates: instead of the whole package, the files that changed since
// the version installed here, as zstd patches against the old files
// ("--patch-from"), and a small Setup that installs from a folder.
//
// The release (scripts/release.ps1) builds one delta package for each of a
// few earlier versions, WinShun-<to>-x64-delta-from-<from>.zip, and names
// them in one line at the end of its release notes, which lcstp.com passes
// on as it passes on the full package's SHA-256 (from GitHub):
//
//   <!-- winshun-delta 1 0.34.1:WinShun-0.34.2-x64-delta-from-0.34.1.zip:2954315:<sha256> ... -->
//
// so the package is as trustworthy as the full one, from any mirror. Its
// files sit beside the full package's (same folder on GitHub and mirrors).
//
// The package (a zip):
//   manifest.json  {"schema":1,"from":..,"to":..,"files":[..],"setup":{..}}
//                  each file {"path","size","sha256","how", "base"?, "data"?}:
//                  how "same"  : the old file at that path, unchanged
//                      "patch" : "data" patched against the old file, whose
//                                SHA-256 is "base"
//                      "new"   : "data" alone (zstd)
//   setup.exe      the delta Setup (installer/WinShun.iss /DDeltaPayload):
//                  the full Setup's script, its files taken from the folder
//                  "payload" beside it
//   d/<n>          the zstd frames
//
// Every file built is checked against its size and SHA-256 before it is
// used; anything that does not fit (a file changed or missing here, a bad
// package) and Updater downloads the full package instead.
namespace delta {

inline constexpr int kSchema = 1;
inline constexpr auto kSetupEntry = "setup.exe";
inline constexpr auto kManifestEntry = "manifest.json";

// The delta package for an update from `current`, as the release notes in
// `info` name it, with the full package's sources (`full`: its URL and
// mirrors, the file name swapped). Nothing when the notes name none for
// `current`, or the line is not as it should be.
std::optional<release::Asset> offer(const release::Info& info, const release::Asset& full, const QString& current);

// The hidden line for the release notes (scripts/release.ps1 via WinShunDelta).
struct Offered {
    QString from;
    QString name;
    qint64 size = 0;
    QString sha256;
};
QString notesLine(const QList<Offered>& offered);
QList<Offered> parseNotesLine(const QString& notes);

struct ApplyOptions {
    // Where the package's setup.exe goes; empty: not wanted (portable).
    QString setupTarget;
    // Files the copy here does not have and does not need (installed for
    // the user alone: no WinShunService.exe, as Setup leaves it out).
    QStringList skip;
    // When set: every file written (and the Setup) is held against changes
    // and checked again through that handle, until Setup has ended.
    std::vector<std::unique_ptr<HeldFile>>* hold = nullptr;
    const std::atomic<bool>* cancel = nullptr;
};
// Builds the new version's files in `out` (which must not exist yet) from
// the files in `base` (the program folder) and the package's bytes. Empty
// on success, else why; `out` (and the Setup) are removed again then.
QString apply(const QByteArray& package, const QString& base, const QString& out, const ApplyOptions& options = {});

// For the release tool and the tests (UpdateDeltaMake.cpp, which links the
// zstd compressor; Win顺 itself only applies).
struct MadeStats {
    int same = 0;
    int patched = 0;
    int added = 0;
    qint64 bytes = 0; // the package's size
};
// The package for `from` -> `to`: `oldDir` and `newDir` are the two
// versions' program folders ("data" left out), `setup` the delta Setup
// (empty: none). Empty on success, else why.
QString make(const QString& oldDir, const QString& newDir, const QString& from, const QString& to, const QString& setup,
    const QString& outZip, MadeStats* stats = nullptr);

} // namespace delta
} // namespace ws::update
