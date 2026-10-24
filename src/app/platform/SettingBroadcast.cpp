#include "SettingBroadcast.h"

#include <QDebug>

#include <windows.h>

#include <atomic>
#include <iterator>
#include <system_error>
#include <thread>
#include <vector>

namespace ws::settingbroadcast {

namespace {

constexpr const wchar_t* kAreaNames[] = {L"ImmersiveColorSet", L"ShellState", L"Environment"};
constexpr UINT kOwnTimeoutMs = 200; // one of Win顺's windows
constexpr UINT kBroadcastTimeoutMs = 1000; // each window, HWND_BROADCAST

// Shared with the detached thread, which may outlive everything else: plain
// atomics (nothing to destroy), no Qt objects.
constinit std::atomic<unsigned> g_areas {0}; // Area bits not sent yet
constinit std::atomic<unsigned> g_requests {0}; // sends asked for that the thread has not taken up
constinit std::atomic<DWORD> g_toldDirectly {0}; // the thread whose windows send() tells itself (the GUI thread)

struct OwnWindow {
    HWND window;
    DWORD thread;
};

// Win顺's top-level windows (those that broadcasts reach), with their threads.
std::vector<OwnWindow> ownWindows()
{
    struct Search {
        DWORD process = ::GetCurrentProcessId();
        std::vector<OwnWindow> found;
    } search;
    ::EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(data);
            DWORD process = 0;
            const DWORD thread = ::GetWindowThreadProcessId(window, &process);
            if (process == s.process)
                s.found.push_back({window, thread});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return std::move(search.found);
}

void tell(HWND window, const wchar_t* area, UINT timeoutMs)
{
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(window, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(area), SMTO_ABORTIFHUNG | SMTO_NORMAL,
        timeoutMs, &result);
}

void sendElsewhere(unsigned areas)
{
    for (unsigned i = 0; i < std::size(kAreaNames); ++i) {
        if (!(areas & (1u << i)))
            continue;
        const DWORD told = g_toldDirectly.load();
        for (const OwnWindow& own : ownWindows()) {
            if (own.thread != told)
                tell(own.window, kAreaNames[i], kOwnTimeoutMs);
        }
        tell(HWND_BROADCAST, kAreaNames[i], kBroadcastTimeoutMs);
    }
}

// The sending thread: takes up every request, sends what is due, and ends
// only once no request came in meanwhile (one that comes after that starts
// a new thread: it finds the count back at 0).
void run()
{
    unsigned taken = 0;
    do {
        taken = g_requests.load();
        if (const unsigned areas = g_areas.exchange(0); areas != 0)
            sendElsewhere(areas);
    } while (g_requests.fetch_sub(taken) != taken);
}

} // namespace

void send(Area area)
{
    const auto index = static_cast<unsigned>(area);
    // The calling thread's windows: their window procedures are called
    // directly, no waiting on anyone.
    const DWORD thread = ::GetCurrentThreadId();
    bool toldHere = false;
    for (const OwnWindow& own : ownWindows()) {
        if (own.thread == thread) {
            tell(own.window, kAreaNames[index], kOwnTimeoutMs);
            toldHere = true;
        }
    }
    if (toldHere)
        g_toldDirectly.store(thread);
    g_areas.fetch_or(1u << index);
    if (g_requests.fetch_add(1) != 0)
        return; // the thread under way sends it (again) when done
    try {
        std::thread(run).detach();
    } catch (const std::system_error& e) {
        g_requests.fetch_sub(1);
        qWarning() << "Setting broadcast: no thread:" << e.what();
    }
}

bool busy() noexcept
{
    return g_requests.load() != 0;
}

} // namespace ws::settingbroadcast
