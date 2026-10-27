#pragma once

#include "Release.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace ws::update {

// How this copy was installed, which decides how an update is put in place
// (Updater): Setup again in the same mode (the Inno Setup uninstall key's
// hive, for this very folder), or the portable zip unpacked over it. None:
// neither (a build run from its folder, a copy installed elsewhere), which
// is sent to the download page instead.
enum class Kind { None, AllUsers, CurrentUser, Portable };
Kind installKind(const QString& appDir, bool portable);

// Something can be created in `folder` (a portable copy in Program Files,
// on a read-only share: not). Tried with a file that is gone at once.
bool folderWritable(const QString& folder);

// The fixed file version of an executable ("0.15.0.0"); empty when it has none.
QString fileVersion(const QString& path);

// A downloaded package, held open for reading with writing and deleting
// refused (by anyone, us included) from its check until it has been used:
// the bytes hashed are the bytes run or unpacked.
class HeldFile {
public:
    HeldFile() = default;
    ~HeldFile() { close(); }
    HeldFile(const HeldFile&) = delete;
    HeldFile& operator=(const HeldFile&) = delete;

    bool open(const QString& path);
    void close();
    bool isOpen() const { return m_handle != nullptr; }
    qint64 size() const;
    // Lowercase hex SHA-256 of the whole file, and, with `bytes`, the file
    // itself (at most `maxBytes`); empty when it could not be read.
    QByteArray sha256(QByteArray* bytes = nullptr, qint64 maxBytes = 0) const;

private:
    void* m_handle = nullptr;
};

// Downloads `asset` to `target`, from each of `urls` in turn until one
// gives exactly its bytes: streamed into "<target>.partial", hashed on the
// way, then its size and SHA-256 compared with the asset's, and only then
// renamed to `target`. A source that fails (no answer, not 2xx, wrong size
// or digest, a mirror slower than ~16 KB/s after half a minute) is deleted
// and the next tried; the mirrors are third parties, GitHub comes last.
// `progress` hears the bytes of the current source; false cancels.
// Blocking: on a worker thread.
struct DownloadResult {
    bool ok = false;
    bool cancelled = false;
    QStringList failures; // "<url>: why", one per source tried
};
using DownloadProgress = std::function<bool(qint64 received, qint64 total)>;
DownloadResult download(const release::Asset& asset, const QStringList& urls, const QString& target,
    const DownloadProgress& progress, int timeoutMs = 30000);

// Unpacks a portable zip (its bytes) into `stage`, which must not exist yet.
// Every entry's name is checked before anything is written: no "..", no
// absolute path or drive, no ':' (streams), no device name or a part
// ending in a dot or space (Windows would write it elsewhere), no symbolic
// link, no encryption, names unique without regard to case; and limits on
// the count and sizes. The single folder the files sit in (release.ps1
// zips "WinShun\") is dropped, and its "data" left out: the user's stays.
// Empty on success, else why (the stage is removed again).
QString unpackPortable(const QByteArray& zip, const QString& stage);

// Whether `name` (an entry's, '/' or '\' separated) is safe to write under
// a folder. Exposed for the tests.
bool safeEntryName(const QString& name);

} // namespace ws::update
