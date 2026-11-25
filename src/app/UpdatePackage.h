#pragma once

#include "Release.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace ws::update {

// How this copy was installed, which decides how an update is put in place
// (Updater): Setup again in the same mode (the Inno Setup uninstall key's
// hive, for this very folder), or the portable zip unpacked over it. Scoop:
// installed by Scoop (core/ScoopInstall.h), which owns the files and the
// versions: updated by `scoop update`, never by swapping files in place.
// None: none of these (a build run from its folder, a copy installed
// elsewhere), which is sent to the download page instead.
enum class Kind { None, AllUsers, CurrentUser, Portable, Scoop };
Kind installKind(const QString& appDir, bool portable);

// The programs a silent Setup found holding Win顺's files, as its log names
// them in its last "Update blocked by: A、B" line (installer/WinShun.iss,
// GitHub #300); empty when the log has none. For the notice of an update
// that did not happen. Cut to a length a notice can show.
QString setupBlockedBy(const QByteArray& log);

// A copy made by Setup (for all users or this one) is installed.
bool setupInstalled();

// WinShunUpdate.exe is at work: it holds the mutex Local\WinShun.Updating
// from its start to its end (each version's helper since 0.16).
bool helperRunning();

// Whether the copy that started an update ran as administrator (GitHub #363):
// Setup starts the new version with the user's own rights (runasoriginaluser,
// #21), so a copy raised by hand ("以管理员身份重新启动 Win顺") came back
// without them. Kept in update.ini in the data folder as the update starts;
// the new version raises itself once more after it (App::elevateOnceUpdated).
void rememberElevation(const QString& dataDir, bool elevated);
bool elevatedBeforeUpdate(const QString& dataDir);

// What that copy does about it, asked once a second (GitHub #400). It waits
// for the helper (it watches this copy come up, and would start another one
// while this one is between its end and its elevated start), at most
// kHelperGiven from its start. Then, with no window of ours open, it
// restarts as administrator; while one is open it waits for it to be put
// away (and kSettled more: an Enter that hid the launcher is still pasting
// or opening), but no longer than kWindowsGiven after the helper ended:
// someone at work in Win顺 is asked (a notification) rather than kept
// waiting, unaware, with normal rights.
enum class ElevationStep : std::uint8_t { Wait, Restart, Ask, Done };
struct ElevationWait {
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kHelperGiven {600};
    static constexpr std::chrono::seconds kWindowsGiven {60};
    static constexpr std::chrono::seconds kSettled {2};

    Clock::time_point started; // the copy's start
    std::optional<Clock::time_point> helperEnded;
    std::optional<Clock::time_point> windowSeen; // last time one was open, after the helper ended

    // `wanted`: administrator before the update (or 管理员直读) and not now,
    // and the user may be. Done: not wanted, or the helper still at work
    // after kHelperGiven (it is left as it is).
    ElevationStep next(Clock::time_point now, bool helper, bool wanted, bool windowOpen);
};

// Something can be created in `folder` (a portable copy in Program Files,
// on a read-only share: not). Tried with a file that is gone at once.
bool folderWritable(const QString& folder);

// The fixed file version of an executable ("0.15.0.0"); empty when it has
// none or it could not be read (`error`: Windows' reason then, else 0).
QString fileVersion(const QString& path, unsigned long* error = nullptr);

// A file just written (a download, an unpacked program) is read by the
// security software for a moment, which can keep it from being opened
// exclusively, renamed or replaced: these errors are waited out.
bool transientHold(unsigned long error);
// The errors that say something on this computer took the file away or
// kept us from it (deleted or quarantined, reported as infected, still held
// after the wait): security software, as good as always, not a bad package.
bool blockedOnThisComputer(unsigned long error);

// How long a hold on a file just written is waited out: an antivirus that
// unpacks an installer to look inside, or asks its cloud, takes seconds.
inline constexpr std::chrono::milliseconds kHoldWait = std::chrono::seconds(30);

// A downloaded package, held open for reading with writing and deleting
// refused (by anyone, us included) from its check until it has been used:
// the bytes hashed are the bytes run or unpacked.
class HeldFile {
public:
    HeldFile() = default;
    ~HeldFile() { close(); }
    HeldFile(const HeldFile&) = delete;
    HeldFile& operator=(const HeldFile&) = delete;

    // Opens `path`, closing what was open. While something else has it open
    // for writing or deleting (transientHold), tried again until `wait` has
    // passed or `cancel` is set; error() then says why it is not open.
    bool open(const QString& path, std::chrono::milliseconds wait = kHoldWait,
        const std::atomic<bool>* cancel = nullptr);
    void close();
    bool isOpen() const { return m_handle != nullptr; }
    unsigned long error() const { return m_error; }
    qint64 size() const;
    // Lowercase hex SHA-256 of the whole file, and, with `bytes`, the file
    // itself (at most `maxBytes`); empty when it could not be read.
    QByteArray sha256(QByteArray* bytes = nullptr, qint64 maxBytes = 0) const;

private:
    void* m_handle = nullptr;
    unsigned long m_error = 0;
};

// Downloads `asset` to `target`, from each of `urls` in turn until one
// gives exactly its bytes: streamed into "<target>.partial", hashed on the
// way, then its size and SHA-256 compared with the asset's, and only then
// renamed to `target`, a hold on either waited out (kHoldWait). A source
// that fails (no answer, not 2xx, wrong size or digest, a mirror slower
// than ~16 KB/s after half a minute) is deleted and the next tried; the
// mirrors are third parties, GitHub comes last.
// `progress` hears the bytes of the current source; false cancels.
// Blocking: on a worker thread.
struct DownloadResult {
    bool ok = false;
    bool cancelled = false;
    // Not the sources: the file could not be written or renamed here (held
    // or removed by security software, a full disk), the same for each.
    bool blocked = false;
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
// Empty on success, else why (the stage is removed again); `writeFailed`
// set when the package was fine but a folder or file could not be written
// (security software refusing a new program file, a full disk).
QString unpackPortable(const QByteArray& zip, const QString& stage, bool* writeFailed = nullptr);

// Whether `name` (an entry's, '/' or '\' separated) is safe to write under
// a folder. Exposed for the tests.
bool safeEntryName(const QString& name);

} // namespace ws::update
