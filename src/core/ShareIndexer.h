#pragma once

#include "ChangeWatcher.h"
#include "Crawler.h"
#include "FileIndex.h"
#include "ShareLister.h"
#include "ShareRetry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ws {

struct ShareConfig {
    std::wstring path; // "\\host\share", or a folder in it (unc::normalize)
    std::wstring name; // shown instead of the path; empty: the path
    bool content = false; // its files' contents indexed too (RemoteContent; GitHub #114)

    bool operator==(const ShareConfig&) const = default;
};

// Keeps network shares in the file index, each as a root of its own named
// by its path ("\\192.168.1.10\docs"), searched like the local disks.
//
// Every share has a thread of its own, so a server that hangs holds up no
// other. It checks first that the server answers at all (share::reachable),
// reads the share in full the first time, then verifies it every hour or so
// and in between follows it live (ReadDirectoryChangesW: a server that
// supports it tells us what changes). Live is not enough alone: Samba tells
// only of changes made over the network, not of those made on the NAS
// itself (its sync and download apps), and with "change notify = no" of
// none, while the watch seems to work. A big share is verified less often:
// a pass takes at most a sixtieth of the time. A server that goes away keeps its
// items in the index, shown as offline, and is tried again now and then and
// whenever the network changes (share::Retry: the probe every few minutes,
// Windows itself every quarter of an hour, as the probe can be wrong).
//
// A pass only removes what is gone once it has read the whole share: a
// server dropping out halfway costs nothing. Nor does a share that comes
// back nearly empty (a NAS whose disks are not mounted yet): it is believed
// only when a pass half an hour later finds the same, or the user says so.
//
// When a share was last read in full is the write time of its root entry,
// so it is saved with the snapshot.
class ShareIndexer {
public:
    enum class State : std::uint8_t {
        Waiting, // for the index, or its turn
        Building, // read for the first time
        Verifying, // read again, in full
        Online, // read, and followed or verified now and then
        Problem, // see Status::problem
        Suspect, // a pass found much less than the index has: not believed yet
    };
    struct Status {
        std::wstring path;
        std::wstring name;
        State state = State::Waiting;
        share::Problem problem = share::Problem::None;
        unsigned long error = 0;
        std::size_t items = 0; // in the index; while building, read so far
        FileTime lastVerified = 0; // 0: never read in full
        std::size_t missing = 0; // Suspect: what the last pass did not find
        bool watching = false; // changes come in live
        // GitHub #116: asked to check now (verifyNow and the like), from the
        // moment of asking until the answer is there (read, or why not), and
        // at least RetryTimes::shown: a quick answer is seen to be one.
        bool checking = false;
        FileTime lastAttempt = 0; // when it was last tried and could not be reached; 0: not yet

        bool operator==(const Status&) const = default;
    };
    struct Options {
        std::vector<ShareConfig> shares;
        CrawlRules rules;
        std::vector<std::wstring> excludedFiles; // share::Lister::Options
        std::chrono::minutes verifyEvery {std::chrono::hours(1)}; // followed live; 0: on start and when asked only
        std::chrono::minutes verifyEveryUnwatched {std::chrono::hours(1)}; // the server does not tell
        int threads = 16; // folders read at a time when building; half that when verifying
        bool watch = true; // follow changes live where the server tells of them; else verifyEveryUnwatched
        share::RetryTimes retry; // a server that cannot be reached, tried again (taken when a share starts)
        // WebDAV tells of nothing either (GitHub #114): its server is asked
        // this often whether it is still there, and when the launcher opens
        // (lookedAt) when not asked for lookedAtAfter. Nothing is listed.
        // Offline after two misses in a row (the second asked a little
        // later); back after that alone, it is not read again unless a pass
        // is due.
        std::chrono::milliseconds webDavPoll {std::chrono::minutes(5)};
        std::chrono::milliseconds lookedAtAfter {std::chrono::minutes(1)};

        bool operator==(const Options&) const = default;
    };
    struct Source {
        std::function<std::shared_ptr<FileIndex>()> index;
        std::function<bool()> ready; // the index is loaded or built: shares can go in
        // For tests, in place of the network; empty: the real thing.
        std::function<bool(const std::wstring& path, bool fresh)> reachable; // share::reachable, for the server of `path`
        std::function<unsigned long(const std::wstring& path)> openRoot; // share::openRoot
    };
    struct Callbacks {
        std::function<void()> changed; // statuses() differ; any thread
        std::function<void()> dirty; // the index changed: save it in a while
        std::function<void(const std::wstring& path, bool reading)> reading;
        // Names, sizes or times under the share at `path` (ShareConfig::path)
        // came in: a pass, changes the server told of. Its files' contents
        // may want reading (RemoteContent); any thread.
        std::function<void(const std::wstring& path)> filesChanged;
    };

    ShareIndexer(Source source, Options options, Callbacks callbacks);
    ~ShareIndexer();
    ShareIndexer(const ShareIndexer&) = delete;
    ShareIndexer& operator=(const ShareIndexer&) = delete;

    void start();
    void requestStop(); // the threads stop where they are, waits for nothing (stop() waits)
    // Waits for the threads; idempotent. Not for Windows still trying a
    // server (tryWindows): up to a minute for one gone away, and the next
    // Win顺 cannot start meanwhile. Let go of, that try leaves the share be.
    void stop();
    void setOptions(Options options);

    void verifyNow(const std::wstring& path); // also: try again after signing in
    void verifyAll();
    // The launcher could not get to a file on the share (GitHub #116):
    // whether its server is still there, asked now (the probe, then Windows
    // before it is called offline); nothing is read unless it came back.
    void checkReach(const std::wstring& path);
    void acceptMissing(const std::wstring& path); // Suspect: they are gone indeed
    void networkChanged(); // tries offline servers again, a few seconds later
    // The launcher opened (GitHub #114): WebDAV servers not asked lately
    // (Options::lookedAtAfter) are asked now, on their shares' threads. Quick.
    void lookedAt();

    std::vector<Status> statuses() const;
    // Whether `path` is on a share that cannot be reached now. Quick.
    bool offline(std::wstring_view path) const;

    // Roots of the index that are shares ("\\..."): kept when configured,
    // dropped otherwise. For a freshly loaded index; caller holds the write lock.
    static std::size_t dropUnknownRoots(FileIndex& index, const std::vector<ShareConfig>& shares);
    // The share root entry for `path`, made when missing. Write lock held.
    static EntryId ensureRoot(FileIndex& index, const std::wstring& path);

private:
    struct Share;
    using Clock = std::chrono::steady_clock;

    void launch(Share& share);
    void run(std::stop_token stop, Share& share);
    bool wait(std::stop_token stop, Share& share, Clock::duration timeout);
    std::optional<unsigned long> tryWindows(std::stop_token stop, Share& share, Clock::duration wait);
    bool pass(std::stop_token stop, Share& share, bool building);
    void applyPending(std::stop_token stop, Share& share);
    void setStatus(Share& share, const std::function<void(Status&)>& change);
    void ask(const std::function<bool(Share&)>& which);
    void updateWatches();
    void watchLoop(std::stop_token stop);
    void onChanges(std::vector<FsChange>&& changes);
    void retire(std::unique_ptr<Share> share);
    Options options() const;

    Source m_source;
    Callbacks m_callbacks;
    mutable std::mutex m_mutex; // guards the members below, and each Share's state
    Options m_options;
    std::vector<std::unique_ptr<Share>> m_shares;
    std::vector<std::unique_ptr<Share>> m_retired; // removed, their threads ending
    Clock::time_point m_networkChangedAt {};
    bool m_started = false;
    bool m_stopped = false;
    bool m_joined = false; // stop() has run
    bool m_watchDirty = false; // the roots to watch may differ: watchLoop sets them
    std::condition_variable_any m_watchCv; // with m_mutex

    // Sets the watches (watchLoop). Dropping one waits for its server to let
    // it go, a minute when the server hung: no share thread waits for that.
    std::jthread m_watchThread;
    // The watch thread's, and stop()'s once it is joined: names alone, and
    // for the shares whose contents are indexed (ShareConfig::content)
    // writes too, so that a file edited there is read again soon. Not for
    // the others: every write on a busy share would come over the network
    // for nothing (GitHub #114).
    std::unique_ptr<ChangeWatcher> m_watcher;
    std::unique_ptr<ChangeWatcher> m_writeWatcher;
};

} // namespace ws
