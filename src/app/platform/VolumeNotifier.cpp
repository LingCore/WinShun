#include "VolumeNotifier.h"

#include <dbt.h>
// Defines (not only declares) the GUID_IO_VOLUME_* events.
#include <initguid.h>
#include <ioevent.h>

#include <algorithm>

namespace ws {

VolumeNotifier::VolumeNotifier(HWND window, Callbacks callbacks)
    : m_window(window)
    , m_callbacks(std::move(callbacks))
{
}

VolumeNotifier::~VolumeNotifier()
{
    for (Volume& v : m_volumes)
        unwatch(v);
}

bool VolumeNotifier::watch(Volume& v)
{
    // No access rights: the handle only tells Windows which volume we mean.
    const std::wstring path = L"\\\\.\\" + v.root;
    v.handle = ::CreateFileW(
        path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (v.handle == INVALID_HANDLE_VALUE)
        return false;
    DEV_BROADCAST_HANDLE filter {};
    filter.dbch_size = sizeof filter;
    filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
    filter.dbch_handle = v.handle;
    v.notification = ::RegisterDeviceNotificationW(m_window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!v.notification) {
        ::CloseHandle(v.handle);
        v.handle = INVALID_HANDLE_VALUE;
        return false;
    }
    v.released = false;
    return true;
}

void VolumeNotifier::unwatch(Volume& v)
{
    if (v.handle != INVALID_HANDLE_VALUE)
        ::CloseHandle(v.handle);
    v.handle = INVALID_HANDLE_VALUE;
    if (v.notification)
        ::UnregisterDeviceNotification(v.notification);
    v.notification = nullptr;
}

// Our handles close; the registration stays, to hear how it ends.
void VolumeNotifier::release(Volume& v)
{
    if (!v.released) {
        v.released = true;
        if (m_callbacks.releaseRequested)
            m_callbacks.releaseRequested(v.root);
    }
    if (v.handle != INVALID_HANDLE_VALUE)
        ::CloseHandle(v.handle);
    v.handle = INVALID_HANDLE_VALUE;
}

void VolumeNotifier::track(const std::vector<std::wstring>& roots)
{
    const auto wanted = [&](const std::wstring& root) {
        return std::any_of(roots.begin(), roots.end(), [&](const std::wstring& r) {
            return ::CompareStringOrdinal(r.c_str(), -1, root.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    };
    // One being removed stays registered: its end is still to come.
    std::erase_if(m_volumes, [&](Volume& v) {
        if (wanted(v.root) || v.released)
            return false;
        unwatch(v);
        return true;
    });
    for (const std::wstring& root : roots) {
        const bool known = std::any_of(m_volumes.begin(), m_volumes.end(), [&](const Volume& v) {
            return ::CompareStringOrdinal(v.root.c_str(), -1, root.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        Volume v {root};
        if (!known && watch(v))
            m_volumes.push_back(std::move(v));
    }
}

LRESULT VolumeNotifier::handle(WPARAM event, LPARAM data)
{
    const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(data);
    if ((event == DBT_DEVICEARRIVAL || event == DBT_DEVICEREMOVECOMPLETE) && header
        && header->dbch_devicetype == DBT_DEVTYP_VOLUME) {
        if (m_callbacks.volumesChanged)
            m_callbacks.volumesChanged(); // a drive letter came or went
        return TRUE;
    }
    if (!header || header->dbch_devicetype != DBT_DEVTYP_HANDLE)
        return TRUE;
    const auto* change = reinterpret_cast<const DEV_BROADCAST_HANDLE*>(data);
    const auto it = std::find_if(m_volumes.begin(), m_volumes.end(),
        [&](const Volume& v) { return v.notification == change->dbch_hdevnotify; });
    if (it == m_volumes.end())
        return TRUE;
    Volume& v = *it;
    const std::wstring root = v.root;

    // A volume that stays after all: register it anew (the old registration
    // belongs to the handle closed meanwhile) and carry on. If that fails,
    // the next track() tries again.
    const auto resume = [&] {
        unwatch(v);
        if (!watch(v))
            m_volumes.erase(it);
        if (m_callbacks.released)
            m_callbacks.released(root);
    };
    switch (event) {
    case DBT_DEVICEQUERYREMOVE:
        release(v);
        return TRUE; // granted: nothing of ours holds it open any more
    case DBT_DEVICEQUERYREMOVEFAILED:
        resume(); // someone else refused
        return TRUE;
    case DBT_DEVICEREMOVEPENDING:
    case DBT_DEVICEREMOVECOMPLETE:
        unwatch(v);
        m_volumes.erase(it);
        if (m_callbacks.released)
            m_callbacks.released(root);
        if (m_callbacks.volumesChanged)
            m_callbacks.volumesChanged();
        return TRUE;
    case DBT_CUSTOMEVENT:
        // Format and chkdsk lock the volume first: that needs our handles closed too.
        if (change->dbch_eventguid == GUID_IO_VOLUME_LOCK) {
            release(v);
        } else if (v.released
            && (change->dbch_eventguid == GUID_IO_VOLUME_UNLOCK || change->dbch_eventguid == GUID_IO_VOLUME_LOCK_FAILED)) {
            resume();
        }
        return TRUE;
    default:
        return TRUE;
    }
}

} // namespace ws
