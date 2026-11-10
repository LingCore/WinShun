#include "WorkerPool.h"

#include <algorithm>

namespace ws {

namespace {
thread_local WorkerPool::Waits t_waits;
}

WorkerPool::Waits WorkerPool::waits() noexcept
{
    return t_waits;
}

// A search goes over every name, the more threads the sooner, up to 16:
// on 3.3 million items a one-letter query took 17 ms with 8 threads and 11
// with 16 (24: 8); the threads sleep between searches.
int WorkerPool::defaultThreadCount()
{
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 16);
}

WorkerPool::WorkerPool(int threads)
{
    const int extra = std::max(0, threads - 1); // the caller works too
    m_threads.reserve(static_cast<std::size_t>(extra));
    for (int i = 0; i < extra; ++i)
        m_threads.emplace_back([this] { work(); });
}

WorkerPool::~WorkerPool()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    m_threads.clear(); // joins
}

void WorkerPool::drain()
{
    const auto& fn = *m_fn;
    for (std::size_t i = m_next.fetch_add(1); i < m_count; i = m_next.fetch_add(1))
        fn(i);
}

void WorkerPool::work()
{
    std::size_t seen = 0;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stopping || m_generation != seen; });
            if (m_stopping)
                return;
            seen = m_generation;
        }
        drain();
        {
            std::lock_guard lock(m_mutex);
            ++m_finished;
        }
        m_done.notify_all();
    }
}

void WorkerPool::parallelFor(std::size_t count, const std::function<void(std::size_t)>& fn)
{
    if (count == 0)
        return;
    const auto asked = std::chrono::steady_clock::now();
    std::lock_guard call(m_callMutex);
    t_waits.forTurn += std::chrono::steady_clock::now() - asked;
    if (m_threads.empty() || count == 1) {
        for (std::size_t i = 0; i < count; ++i)
            fn(i);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_fn = &fn;
        m_count = count;
        m_next.store(0);
        m_finished = 0;
        ++m_generation;
    }
    m_wake.notify_all();
    drain();
    // Every worker checks in once per round, so none can still be touching
    // `fn` when we return.
    const auto drained = std::chrono::steady_clock::now();
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [&] { return m_finished == m_threads.size(); });
    m_fn = nullptr;
    t_waits.forThreads += std::chrono::steady_clock::now() - drained;
}

} // namespace ws
