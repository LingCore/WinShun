#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace ws {

// Lets go of volumes that are about to be removed or locked. Before a drive
// is ejected ("Safely remove") or locked (format, chkdsk), Windows asks the
// programs that registered for it; a handle still open makes the request fail
// with "this device is in use". This registers every indexed volume, reports
// those requests, and reports drives that arrive or go.
class VolumeNotifier {
public:
    struct Callbacks {
        // Close every handle on the volume ("E:") before returning.
        std::function<void(const std::wstring& root)> releaseRequested;
        // The removal or lock is over: it failed, or the volume is gone.
        std::function<void(const std::wstring& root)> released;
        std::function<void()> volumesChanged; // a drive arrived or went
    };

    // `window` receives WM_DEVICECHANGE and passes it to handle().
    VolumeNotifier(HWND window, Callbacks callbacks);
    ~VolumeNotifier();

    VolumeNotifier(const VolumeNotifier&) = delete;
    VolumeNotifier& operator=(const VolumeNotifier&) = delete;

    void track(const std::vector<std::wstring>& roots); // the indexed volumes
    LRESULT handle(WPARAM event, LPARAM data); // WM_DEVICECHANGE

private:
    struct Volume {
        std::wstring root;
        HANDLE handle = INVALID_HANDLE_VALUE; // names the volume only; closed when asked to let go
        HDEVNOTIFY notification = nullptr;
        bool released = false;
    };

    bool watch(Volume& v);
    void unwatch(Volume& v);
    void release(Volume& v);

    HWND m_window;
    Callbacks m_callbacks;
    std::vector<Volume> m_volumes;
};

} // namespace ws
