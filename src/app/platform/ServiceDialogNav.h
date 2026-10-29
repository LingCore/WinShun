#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace ws {

// Asks the index service to navigate a higher-integrity (elevated) file dialog
// to a folder, for the DialogBar / DialogJump (GitHub #104). Win顺's UI runs
// with normal rights and UIPI blocks what it would send such a dialog; the
// service launches a uiAccess helper that drives it. A missing or older
// service (no kCapabilityDialogNav) leaves the copy-and-paste fallback.
//
// All pipe I/O runs off the GUI thread. A background thread keeps `reach`
// current (the service may come up after login, or be upgraded) and runs the
// navigation requests in order. Callbacks fire on that thread; the owner
// marshals them to the GUI thread.
class ServiceDialogNav {
public:
    // What the service offers this copy of Win顺, for the bar's hint too.
    enum class Reach {
        NoService, // none running (or of another major version)
        NoNavigator, // one runs but cannot navigate (older, or cannot start helpers)
        OtherCopy, // it can, but for the WinShun.exe beside it only: this is a portable or other copy
        Available,
    };
    using ReachChanged = std::function<void(Reach reach)>;
    using Done = std::function<void(bool ok)>;

    explicit ServiceDialogNav(ReachChanged reachChanged);
    ~ServiceDialogNav();
    ServiceDialogNav(const ServiceDialogNav&) = delete;
    ServiceDialogNav& operator=(const ServiceDialogNav&) = delete;

    Reach reach() const { return m_reach.load(std::memory_order_relaxed); }
    bool available() const { return reach() == Reach::Available; }
    void refresh(); // re-probe soon (e.g. an elevated dialog just came up)
    // Navigate `dialog` to `folder`; `done(ok)` is called on the worker thread.
    void navigate(HWND dialog, std::wstring folder, Done done);

private:
    struct Request {
        HWND dialog;
        std::wstring folder;
        Done done;
    };
    void run();
    void setReach(Reach now); // on the worker thread; tells the owner of each change
    Reach probe(); // connect + handshake
    bool send(const Request& request); // one connection: handshake + OpenDialogNav

    ReachChanged m_reachChanged;
    std::atomic<Reach> m_reach {Reach::NoService};
    Reach m_told = Reach::NoService; // what m_reachChanged said last (worker thread only)
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<Request> m_queue;
    bool m_stop = false;
    bool m_refresh = false;
    HANDLE m_stopEvent = nullptr; // cancels an in-flight pipe read at shutdown
    std::thread m_thread;
};

} // namespace ws
