#include "ComWorker.h"

#include <objbase.h>

#include <utility>

namespace ws {

ComWorker::ComWorker()
    : m_thread([this](std::stop_token stop) { run(stop); })
{
}

ComWorker::~ComWorker() = default; // m_thread: stop, then join

void ComWorker::post(std::function<void()> task)
{
    {
        std::lock_guard lock(m_mutex);
        m_tasks.push_back(std::move(task));
    }
    m_wake.notify_one();
}

void ComWorker::run(std::stop_token stop)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, stop, [this] { return !m_tasks.empty(); });
            if (stop.stop_requested())
                break;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        task();
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
}

} // namespace ws
