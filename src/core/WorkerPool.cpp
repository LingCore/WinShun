#include "WorkerPool.h"

#include <algorithm>

namespace qf {

int WorkerPool::defaultThreadCount()
{
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
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
    std::lock_guard call(m_callMutex);
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
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [&] { return m_finished == m_threads.size(); });
    m_fn = nullptr;
}

} // namespace qf
