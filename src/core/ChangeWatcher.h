#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace qf {

struct FsChange {
    enum class Kind : std::uint8_t { Added, Removed, RenamedFrom, RenamedTo, Overflow };
    Kind kind;
    std::wstring path; // full path; the volume root ("C:") for Overflow
};

// Watches whole volumes for name changes with ReadDirectoryChangesW on one
// background thread. No administrator rights required.
class ChangeWatcher {
public:
    using Handler = std::function<void(std::vector<FsChange>&&)>;

    // The handler runs on the watcher thread.
    explicit ChangeWatcher(Handler handler);
    ~ChangeWatcher();

    ChangeWatcher(const ChangeWatcher&) = delete;
    ChangeWatcher& operator=(const ChangeWatcher&) = delete;

    // Watches these volumes ("C:", "D:", ...) from now on. Returns once the
    // others are no longer watched (their handles closed, so they can be
    // ejected) and the new ones are, unless they cannot be opened. Never call
    // it while holding a lock the handler takes: it waits for the handler.
    void setRoots(std::vector<std::wstring> roots);
    std::vector<std::wstring> roots() const; // those being watched

private:
    void run(std::stop_token stop);

    Handler m_handler;
    mutable std::mutex m_mutex;
    std::condition_variable m_applied;
    std::vector<std::wstring> m_wanted; // guarded by m_mutex, like the three below
    std::vector<std::wstring> m_watching;
    std::uint64_t m_requested = 0;
    std::uint64_t m_done = 0;
    void* m_wake = nullptr; // event: m_wanted changed
    std::jthread m_thread;
};

} // namespace qf
