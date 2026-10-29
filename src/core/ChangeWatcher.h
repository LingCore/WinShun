#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace ws {

struct FsChange {
    // Modified: a file written to (its size or time), from a watcher asked
    // for writes only; a folder's own too, when a name in it changed.
    enum class Kind : std::uint8_t { Added, Removed, RenamedFrom, RenamedTo, Overflow, Modified };
    Kind kind;
    std::wstring path; // full path; the root watched ("C:", or a folder) for Overflow
    // GitHub #52: a RenamedTo that only ends a move, of a folder above chosen
    // ones (its entry holds no more than the way to them): when the move
    // cannot be made, nothing comes in its place (applyChanges), let alone
    // walked; the chosen folders' walk sets them right.
    bool moveOnly = false;
};

// Watches whole volumes for name changes with ReadDirectoryChangesW on one
// background thread. No administrator rights required.
class ChangeWatcher {
public:
    using Handler = std::function<void(std::vector<FsChange>&&)>;

    // GitHub #52: folders in a root to watch in its place when the root
    // itself cannot be opened (this account may not list it). Each such watch
    // holds its folder open, which keeps the folders above it from being
    // renamed or moved (docs/pitfalls.md), so only then.
    struct Fallback {
        std::wstring root;
        std::vector<std::wstring> folders;
        bool operator==(const Fallback&) const = default;
    };

    // One wait takes at most 64 handles, two of them the thread's own.
    static constexpr std::size_t kMaxWatches = 62;

    // What a watcher tells of: names (added, removed, renamed), or files
    // written to as well (Modified, once a read): for shares, whose files are
    // read for their contents and which keep no change journal (GitHub #114).
    // Over SMB they come within a second or two.
    enum class Tell : std::uint8_t { Names, Writes };

    // The handler runs on the watcher thread.
    explicit ChangeWatcher(Handler handler, Tell tell = Tell::Names);
    ~ChangeWatcher();

    ChangeWatcher(const ChangeWatcher&) = delete;
    ChangeWatcher& operator=(const ChangeWatcher&) = delete;

    // Watches these volumes ("C:", "D:", ...) from now on (or folders, or
    // shares). Returns once the others are no longer watched (their handles
    // closed, so they can be ejected) and the new ones are, unless they
    // cannot be opened; those `instead` names for such a root are watched
    // then. At most kMaxWatches in all: the roots first, in order, then the
    // folders in their place. What stays watched is not opened again (no
    // events lost). Never call it while holding a lock the handler takes: it
    // waits for the handler.
    void setRoots(std::vector<std::wstring> roots, std::vector<Fallback> instead = {});
    std::vector<std::wstring> roots() const; // those being watched, folders in a root's place too

    // How many bytes a watch on `root` collects between two reads: 1 MB for a
    // local volume's root, where changes come thick and fast (unpacking an
    // archive, a build) and losing them costs a walk; 64 KB for the rest:
    // over the network ReadDirectoryChangesW takes no more (shares).
    static std::size_t bufferBytes(const std::wstring& root);
    // GitHub #52: the bytes a watch goes on with once a read of `bytes` came
    // back (`completed`, or failed with `error`): a big buffer whose read
    // failed, and not because events were lost (ERROR_NOTIFY_ENUM_DIR), is
    // taken down to 64 KB. Some file systems take 1 MB and then fail every
    // read: each would lose the root's events and have it walked again.
    static std::size_t bufferAfter(std::size_t bytes, bool completed, unsigned long error);

private:
    void run(std::stop_token stop);

    Handler m_handler;
    const Tell m_tell;
    mutable std::mutex m_mutex;
    std::condition_variable m_applied;
    std::vector<std::wstring> m_wanted; // guarded by m_mutex, like the four below
    std::vector<Fallback> m_instead;
    std::vector<std::wstring> m_watching;
    std::uint64_t m_requested = 0;
    std::uint64_t m_done = 0;
    void* m_wake = nullptr; // event: m_wanted changed
    std::jthread m_thread;
};

} // namespace ws
