#pragma once

// The tools' work off the GUI thread (ToolHost::aside): a few threads of
// their own, and the manners of the reads made there.

#include <QString>
#include <QStringList>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace ws {
class WorkerPool;
}

namespace ws::mcp {

class Visibility;

// At most a few jobs at once (a call's access checks, sizes and reads: one
// run of ws may make 50 000 of them), the rest waiting their turn in order.
// A job stopped while it waits never runs. Each is destroyed on the thread
// that ran it, so what it holds (a search's results) is freed there, not on
// the GUI thread. Going, it stops every job first, then joins its threads.
class AsideWorkers {
public:
    using Job = std::function<void(const std::stop_token& stop)>;

    explicit AsideWorkers(int threads);
    ~AsideWorkers();
    AsideWorkers(const AsideWorkers&) = delete;
    AsideWorkers& operator=(const AsideWorkers&) = delete;

    // Queues `job`; its stop source stops it, waiting or running.
    std::stop_source post(Job job);

private:
    struct Item {
        std::stop_source stop {std::nostopstate};
        Job job;
    };
    void work(std::size_t slot);

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Item> m_queue;
    std::vector<std::stop_source> m_running; // each thread's job now, to be stopped when going
    bool m_quitting = false;
    std::vector<std::jthread> m_threads; // last: started once the rest is there
};

// How a job reads (at() before each path it goes to the disk for): from a
// hard disk while the user is at the computer, its thread works in
// background mode, as the content index reads then (Win顺's quiet mode), so
// that the user's own reads come first; elsewhere at its normal priority.
// Made on the job's thread, and ended with it.
class QuietIo {
public:
    QuietIo(); // the user away or not: asked once (win32::userAway)
    explicit QuietIo(bool userAway);
    ~QuietIo();
    QuietIo(const QuietIo&) = delete;
    QuietIo& operator=(const QuietIo&) = delete;

    void at(const QString& path);
    bool background() const noexcept { return m_background; }
    // A path on one of this computer's fixed drives that does not seek (a
    // solid state drive): reads there may go many at once without taking
    // the disk from anyone. Not a hard disk, nor a share (the network's pace).
    bool solid(const QString& path);

private:
    enum class Disk : std::uint8_t { Other, Seeks, Solid };
    Disk diskOf(const QString& path); // Other: not a local drive letter's path, or not a fixed drive

    bool m_away;
    bool m_background = false;
    std::map<char16_t, Disk> m_disks; // drive letter (upper case) -> what it is
};

// What a tool's job aside goes by: whether to stop, what its caller may see
// (the checks go to the disk each at that disk's pace: QuietIo), and how it
// reads what else it reads (quiet.at first).
struct AsideContext {
    std::stop_token stop;
    Visibility& view;
    QuietIo& quiet;
    // Where checks of many folders go at once (seeAhead); null: one at a time.
    WorkerPool* pool = nullptr;

    bool stopped() const noexcept { return stop.stop_requested(); }
    bool sees(const QString& path);
    bool canList(const QString& folder);
    bool canRead(const QString& file);
    bool canReadAttributes(const QString& path);
    // The folders of `paths` checked ahead of sees(), many at once (on
    // `pool`): those on a solid state drive (QuietIo::solid), when enough
    // of them are new to be worth it. The rest are checked by sees() one at
    // a time as before, a hard disk's quietly. The same decisions either way.
    void seeAhead(const QStringList& paths);
};

} // namespace ws::mcp
