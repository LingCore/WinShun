#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace qf {

// A small fixed set of threads for CPU-bound parallel loops (the name scan).
// Unlike std::execution::par, which grows the system thread pool to one
// thread per core, this keeps the thread count bounded and predictable.
class WorkerPool {
public:
    explicit WorkerPool(int threads);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int size() const noexcept { return static_cast<int>(m_threads.size()) + 1; }

    // Calls fn(i) for every i in [0, count), spread over the pool and the
    // calling thread, and returns when all calls finished. One loop at a time.
    void parallelFor(std::size_t count, const std::function<void(std::size_t)>& fn);

    static int defaultThreadCount();

private:
    void work();
    void drain();

    std::vector<std::jthread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    const std::function<void(std::size_t)>* m_fn = nullptr;
    std::size_t m_count = 0;
    std::atomic<std::size_t> m_next {0};
    std::size_t m_generation = 0;
    std::size_t m_finished = 0; // workers done with the current round
    bool m_stopping = false;
    std::mutex m_callMutex; // serialises parallelFor() callers
};

} // namespace qf
