#pragma once

#include <cstdint>
#include <functional>
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

    // roots: "C:", "D:", ... The handler runs on the watcher thread.
    ChangeWatcher(std::vector<std::wstring> roots, Handler handler);
    ~ChangeWatcher();

    ChangeWatcher(const ChangeWatcher&) = delete;
    ChangeWatcher& operator=(const ChangeWatcher&) = delete;

    const std::vector<std::wstring>& roots() const noexcept { return m_roots; }

private:
    void run(std::stop_token stop);

    std::vector<std::wstring> m_roots;
    Handler m_handler;
    std::jthread m_thread;
};

} // namespace qf
