#include "Selection.h"

#include <utility>

namespace gestures {

SelectionReader::SelectionReader(Read read)
    : m_read(std::move(read))
    , m_thread([this] { run(); })
{
}

SelectionReader::~SelectionReader()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_one();
    if (m_thread.joinable())
        m_thread.join();
}

void SelectionReader::ask(HWND root, std::function<void(std::wstring)> answer)
{
    {
        std::lock_guard lock(m_mutex);
        m_root = root;
        m_answer = std::move(answer);
    }
    m_wake.notify_one();
}

void SelectionReader::run()
{
    for (;;) {
        HWND root = nullptr;
        std::function<void(std::wstring)> answer;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_quit || m_answer; });
            if (m_quit)
                break;
            root = m_root;
            answer = std::exchange(m_answer, nullptr);
        }
        answer(m_read ? m_read(root) : std::wstring());
    }
}

} // namespace gestures
