#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace ws {

// A thread of its own with COM (a single-threaded apartment, as the shell
// wants), for work that waits on other programs: asking Explorer, driving a
// file dialog. The tasks run one at a time, in the order they came.
//
// The owner's members can be used from the tasks: the destructor lets the
// running task finish and drops the others before the owner's members go.
class ComWorker {
public:
    ComWorker();
    ~ComWorker();

    ComWorker(const ComWorker&) = delete;
    ComWorker& operator=(const ComWorker&) = delete;

    void post(std::function<void()> task);

private:
    void run(std::stop_token stop);

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<std::function<void()>> m_tasks;
    std::jthread m_thread; // last: started once the members above exist
};

} // namespace ws
