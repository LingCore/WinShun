#include "Interrupt.h"

#include "Output.h"
#include "Result.h"

#include <atomic>
#include <mutex>

namespace ws::cli::interrupt {

namespace {

std::atomic<bool> g_requested {false};
HANDLE g_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); // set on Ctrl+C
HANDLE g_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); // set when the main thread leaves
std::mutex g_mutex; // g_action
std::function<void()> g_action;

void runAction()
{
    const std::lock_guard lock(g_mutex);
    if (g_action)
        g_action();
}

BOOL WINAPI onControl(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        break;
    default:
        return FALSE;
    }
    if (g_requested.exchange(true))
        return TRUE; // a second press while the first is handled
    ::SetEvent(g_event);
    // Two seconds for the main thread to tidy up and leave, ending a read
    // that may begin meanwhile.
    for (int i = 0; i < 100; ++i) {
        runAction();
        if (::WaitForSingleObject(g_done, 20) == WAIT_OBJECT_0)
            return TRUE;
    }
    ConsoleModes::restoreNow();
    ::ExitProcess(static_cast<UINT>(Exit::Interrupted));
}

} // namespace

void install()
{
    ::SetConsoleCtrlHandler(onControl, TRUE);
}

bool requested() noexcept
{
    return g_requested.load();
}

bool sleep(DWORD milliseconds)
{
    if (requested())
        return false;
    return ::WaitForSingleObject(g_event, milliseconds) != WAIT_OBJECT_0;
}

void setAction(std::function<void()> action)
{
    const std::lock_guard lock(g_mutex);
    g_action = std::move(action);
}

void clearAction()
{
    const std::lock_guard lock(g_mutex);
    g_action = nullptr;
}

void finished() noexcept
{
    ::SetEvent(g_done);
}

} // namespace ws::cli::interrupt
