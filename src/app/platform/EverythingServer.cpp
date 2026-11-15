#include "EverythingServer.h"

#include "FileIndex.h"
#include "WorkerPool.h"

#include <QDebug>
#include <QElapsedTimer>

#include <chrono>
#include <span>

using namespace std::chrono_literals;

namespace ws {

namespace {

// How long a program is given to take its answer before it counts as hung.
constexpr UINT kReplyTimeoutMs = 10'000;
// With Everything's own window up: looked for again this often.
constexpr DWORD kLookAgainMs = 30'000;
// Answers larger than this are not made (WM_COPYDATA maps them into the
// asker): the rows are cut, the totals stay.
constexpr qsizetype kMostBytes = 512 * 1024 * 1024;

HWND windowOf(std::uint32_t handle) noexcept
{
    return reinterpret_cast<HWND>(static_cast<LONG_PTR>(static_cast<LONG>(handle))); // handles are sign-extended
}

} // namespace

EverythingServer::EverythingServer(IndexSource index, std::wstring windowClass, Observer observer)
    : m_index(std::move(index))
    , m_observer(std::move(observer))
    , m_class(std::move(windowClass))
    , m_searcher([this](std::stop_token stop) { search(stop); })
    , m_server([this](std::stop_token stop) { serve(stop); })
{
}

EverythingServer::~EverythingServer()
{
    m_server.request_stop();
    if (const DWORD thread = m_serveThread.load())
        ::PostThreadMessageW(thread, WM_NULL, 0, 0); // out of its wait
    m_server.join();
    m_searcher.request_stop();
    m_wake.notify_all();
}

LRESULT CALLBACK EverythingServer::proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<EverythingServer*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    if (!self)
        return ::DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_USER: { // EVERYTHING_WM_IPC
        const auto index = self->m_index();
        return everything::answerCommand(static_cast<std::uint32_t>(wParam), static_cast<std::uint32_t>(lParam),
            index && index->liveCount() > 0);
    }
    case WM_COPYDATA: {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!data || !data->lpData)
            return FALSE;
        auto query = everything::readQuery(static_cast<std::uint32_t>(data->dwData),
            std::span(static_cast<const std::byte*>(data->lpData), data->cbData));
        if (!query || !::IsWindow(windowOf(query->replyWindow)))
            return FALSE; // a command line for Everything, run counts: not ours to do
        self->enqueue(std::move(*query));
        return TRUE;
    }
    default:
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

bool EverythingServer::makeWindow()
{
    if (::FindWindowW(m_class.c_str(), nullptr))
        return false; // Everything itself (or another Win顺): it answers
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc {sizeof wc};
    wc.lpfnWndProc = &EverythingServer::proc;
    wc.hInstance = instance;
    wc.lpszClassName = m_class.c_str();
    ::RegisterClassExW(&wc); // again after a window of ours went: already there
    // Top-level and never shown, as Everything's is: programs look for it
    // with FindWindow, which does not see message-only windows.
    const HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, m_class.c_str(), L"", WS_POPUP, 0, 0, 0, 0,
        nullptr, nullptr, instance, this);
    if (!window) {
        qWarning() << "Everything queries: no window, error" << ::GetLastError();
        return false;
    }
    // Asked by programs of lower rights too when Win顺 runs as administrator.
    ::ChangeWindowMessageFilterEx(window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(window, WM_USER, MSGFLT_ALLOW, nullptr);
    m_window = window;
    qInfo() << "Everything queries: answering";
    return true;
}

void EverythingServer::serve(std::stop_token stop)
{
    m_serveThread = ::GetCurrentThreadId();
    MSG message;
    ::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's queue, for the wake-up
    bool told = false;
    while (!stop.stop_requested()) {
        if (!m_window.load() && !makeWindow() && !std::exchange(told, true))
            qInfo() << "Everything queries: Everything itself is running and answers";
        const DWORD wait = m_window.load() ? INFINITE : kLookAgainMs;
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT);
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
    if (const HWND window = m_window.exchange(nullptr))
        ::DestroyWindow(window);
    ::UnregisterClassW(m_class.c_str(), ::GetModuleHandleW(nullptr));
}

void EverythingServer::enqueue(everything::Query query)
{
    {
        const std::scoped_lock lock(m_mutex);
        const std::uint64_t turn = ++m_turn;
        m_turns[query.replyWindow] = turn;
        // What the same program asked before and has not been searched yet
        // is not waited for any more.
        std::erase_if(m_jobs, [&](const Job& job) { return job.query.replyWindow == query.replyWindow; });
        m_jobs.push_back({std::move(query), turn});
    }
    m_wake.notify_one();
}

bool EverythingServer::superseded(const Job& job)
{
    const std::scoped_lock lock(m_mutex);
    const auto latest = m_turns.find(job.query.replyWindow);
    return latest != m_turns.end() && latest->second != job.turn;
}

void EverythingServer::search(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            if (!m_wake.wait(lock, stop, [this] { return !m_jobs.empty(); }))
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        const auto index = m_index();
        if (!index)
            continue;
        QElapsedTimer clock;
        clock.start();
        everything::Rows rows;
        {
            // Threads for the time of the search only: nothing is kept
            // running for a switch most leave idle.
            WorkerPool pool(WorkerPool::defaultThreadCount());
            rows = everything::find(*index, pool, job.query, [&] { return stop.stop_requested() || superseded(job); });
        }
        if (stop.stop_requested() || superseded(job))
            continue;
        if (m_observer)
            m_observer(job.query, rows);
        QByteArray bytes = everything::encode(job.query, rows);
        while (bytes.size() > kMostBytes && !rows.rows.empty()) {
            rows.rows.resize(rows.rows.size() / 2);
            bytes = everything::encode(job.query, rows);
        }
        const qint64 searched = clock.elapsed();
        COPYDATASTRUCT data {};
        data.dwData = job.query.replyMessage;
        data.cbData = static_cast<DWORD>(bytes.size());
        data.lpData = bytes.data();
        DWORD_PTR taken = 0;
        const LRESULT sent = ::SendMessageTimeoutW(windowOf(job.query.replyWindow), WM_COPYDATA,
            reinterpret_cast<WPARAM>(m_window.load()), reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK,
            kReplyTimeoutMs, &taken);
        // Never the words asked for: they are the user's.
        qInfo().noquote() << QStringLiteral("Everything queries: %1 of %2 rows, %3 KB, searched %4 ms, sent %5 ms%6")
                                 .arg(rows.rows.size())
                                 .arg(rows.totalItems)
                                 .arg(bytes.size() / 1024)
                                 .arg(searched)
                                 .arg(clock.elapsed() - searched)
                                 .arg(sent ? QString() : QStringLiteral(" (not taken, error %1)").arg(::GetLastError()));
        const std::scoped_lock lock(m_mutex);
        if (const auto latest = m_turns.find(job.query.replyWindow); latest != m_turns.end() && latest->second == job.turn)
            m_turns.erase(latest);
    }
}

} // namespace ws
