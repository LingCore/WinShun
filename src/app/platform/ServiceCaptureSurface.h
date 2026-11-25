#pragma once

#include <QString>

#include <windows.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace ws {

// Holds at most one active OpenCaptureSurface lease against WinShunService.exe
// on behalf of winshun.capture. All pipe I/O runs on a background thread so
// the GUI thread never blocks; closing signals the stop event immediately so
// the service drops the Job and ends the Band-2 UIAccess mirror at once.
class ServiceCaptureSurface {
public:
    struct Result {
        bool ok = false;
        DWORD childPid = 0;
        QString reason; // "no-service", "not-offered", "busy", "failed"
        DWORD error = ERROR_SUCCESS;
    };

    ServiceCaptureSurface();
    ~ServiceCaptureSurface();
    ServiceCaptureSurface(const ServiceCaptureSurface&) = delete;
    ServiceCaptureSurface& operator=(const ServiceCaptureSurface&) = delete;

    // Ends any earlier lease, then opens a new one off the GUI thread and
    // invokes `done` once from the worker thread.
    void open(DWORD pluginPid, HWND fullScreenHwnd, std::wstring pipeName, std::function<void(Result)> done);
    // Signals the active lease (if any) to end immediately. Safe on the GUI thread.
    void close();

private:
    struct Lease;
    std::mutex m_mutex;
    std::unique_ptr<Lease> m_active;
};

} // namespace ws
