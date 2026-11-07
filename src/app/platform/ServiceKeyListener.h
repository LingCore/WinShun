#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ws {

// Keeps a service-owned UIAccess listener alive for this window. A missing
// or older service leaves KeyListener's ordinary Raw Input listener working.
// All pipe I/O and retries run off the GUI thread; destruction cancels them.
class ServiceKeyListener {
public:
    using Changed = std::function<void(DWORD pid)>; // zero: disconnected
    // `keys`: the UI's plain hotkeys for the listener to hold back over
    // administrators' windows (plainkeys::relayed, GitHub #243).
    ServiceKeyListener(HWND window, Changed changed, std::vector<std::uint32_t> keys = {});
    ~ServiceKeyListener();
    ServiceKeyListener(const ServiceKeyListener&) = delete;
    ServiceKeyListener& operator=(const ServiceKeyListener&) = delete;

    // Other keys from now on: the listener is started anew with them (a
    // service that relays none, or keeps that for Win顺's own UI, is left be).
    void setKeys(std::vector<std::uint32_t> keys);
    // The listener runs and holds the keys back for this process.
    bool relaying() const noexcept { return m_relaying.load(); }

private:
    void run(HWND window, const Changed& changed);
    std::vector<std::uint32_t> keys() const;
    HANDLE m_stop = nullptr;
    HANDLE m_rekey = nullptr; // set by setKeys
    mutable std::mutex m_keysMutex;
    std::vector<std::uint32_t> m_keys; // guarded by m_keysMutex
    std::atomic<bool> m_relaying {false};
    std::thread m_thread;
};

} // namespace ws
