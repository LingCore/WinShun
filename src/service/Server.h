#pragma once

#include "VolumeProtocol.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ws::service {

// One client connection, served on a thread of its own: the handshake, then
// one operation (VolumeProtocol.h). While the operation holds a volume open
// (its MFT or change journal), the connection is bound to that volume, so
// that the volume can be let go of from outside (Server::closeVolume): the
// thread sees `abort` set, closes what it holds and sets `released`.
// What a connection holds a volume for: a journal (any number per process),
// an MFT read or a bulk read (one each per process and volume).
enum class Bind : std::uint8_t { Journal, Mft, Bulk };

struct Connection {
    volproto::Channel channel;
    std::uint32_t id = 0;
    DWORD pid = 0; // the client process, as the pipe reports it
    std::uint16_t minor = 0; // the protocol minor the client speaks (its Hello)
    std::wstring root; // "C:" while bound
    bool bound = false; // holds (or is about to hold) a handle on `root`; under Server's mutex
    Bind kind = Bind::Journal; // while bound
    HANDLE abort = nullptr; // manual-reset: let go of the volume (or stop altogether)
    HANDLE released = nullptr; // manual-reset: nothing held on the volume any more
    void* watch = nullptr; // DeviceWatch's token while bound
    HANDLE token = nullptr; // the client's identification token, while a bulk read checks access with it

    ~Connection() // the events live as long as anyone waiting on them holds the connection
    {
        if (abort)
            ::CloseHandle(abort);
        if (released)
            ::CloseHandle(released);
        if (token)
            ::CloseHandle(token);
    }
};

// Lets go of volumes that Windows is about to take away (an eject, a lock
// by format or chkdsk) even when the client does not ask: each volume a
// connection holds is registered for device notifications with a handle of
// no access rights. Notifications arrive through the service's control
// handler (DEVICE_NOTIFY_SERVICE_HANDLE) or, in --console mode, a message
// window of its own.
class DeviceWatch {
public:
    using Release = std::function<void(const std::wstring& root)>;
    // `recipient`: the SERVICE_STATUS_HANDLE with DEVICE_NOTIFY_SERVICE_HANDLE, or
    // nothing with 0 (a message window on a thread of this object's is used).
    DeviceWatch(HANDLE recipient, DWORD recipientFlags, Release release);
    ~DeviceWatch();
    DeviceWatch(const DeviceWatch&) = delete;
    DeviceWatch& operator=(const DeviceWatch&) = delete;

    void* watch(const std::wstring& root); // a token, or null
    void unwatch(void* token);
    // From the control handler or the window: a device event. Returns TRUE
    // (allow) for the query-remove and lock events it handles.
    LRESULT onEvent(DWORD eventType, const void* data);

private:
    struct Entry {
        void* token; // HDEVNOTIFY
        HANDLE handle;
        std::wstring root;
    };
    void closeEntry(Entry& e);

    HANDLE m_recipient;
    DWORD m_flags;
    Release m_release;
    std::mutex m_mutex;
    std::vector<Entry> m_entries;
    std::thread m_windowThread; // console mode
    HWND m_window = nullptr;
    HANDLE m_windowReady = nullptr;
};

class Server {
public:
    struct Options {
        std::wstring pipeName = volproto::kPipeName;
        unsigned maxPerProcess = 16; // connections one client process may hold
        DWORD helloTimeoutMs = 10'000; // for the handshake and the request after it
        DWORD writeTimeoutMs = 10'000; // a client that does not read for this long is dropped
        bool bulkReads = true; // --console --no-bulk: as a service without them (tests)
        bool keyListener = false; // LocalSystem with the session-launch privileges
        std::wstring installedFrom; // --installed-from: the WinShun.exe also given dialog navigation
    };
    explicit Server(Options options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    const Options& options() const noexcept { return m_options; }
    void setDeviceWatch(DeviceWatch* watch) noexcept { m_watch = watch; }

    bool start(DWORD* error); // the first pipe instance and the accept thread
    void stop(); // ends every connection, waits for their threads (a while at most)
    unsigned connectionCount() const;

    // Binds `conn` to `root` for `kind`: false when the process already holds
    // the volume for an MFT or bulk read of that kind (one at a time per process).
    bool bind(Connection& conn, std::wstring root, Bind kind);
    void unbind(Connection& conn);
    // Has every connection bound to `root` (of process `pid`, or of all with
    // 0) let go of it, and waits up to `waitMs` for them to say so. Returns
    // how many were told.
    unsigned closeVolume(std::wstring_view root, DWORD pid, DWORD waitMs);

private:
    HANDLE createInstance(bool first, DWORD* error);
    void acceptLoop();
    void serve(std::shared_ptr<Connection> conn);
    bool handshake(Connection& conn);

    Options m_options;
    DeviceWatch* m_watch = nullptr;
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    HANDLE m_listening = INVALID_HANDLE_VALUE;
    HANDLE m_stopEvent = nullptr;
    std::atomic<bool> m_running {false};
    std::thread m_acceptor;
    mutable std::mutex m_mutex;
    std::condition_variable m_finished;
    std::vector<std::shared_ptr<Connection>> m_connections;
    std::uint32_t m_nextId = 1;
};

// The operations (Volumes.cpp): each serves one request on `conn` and returns
// when it is answered, the client gone, or the volume let go of.
void serveOpenMft(Server& server, Connection& conn, std::span<const std::byte> payload);
void serveQueryJournal(Server& server, Connection& conn, std::span<const std::byte> payload);
void serveFollowJournal(Server& server, Connection& conn, std::span<const std::byte> payload);
void serveCloseVolume(Server& server, Connection& conn, std::span<const std::byte> payload);
void serveOpenBulkRead(Server& server, Connection& conn, std::span<const std::byte> payload);
// A reply with nothing in it, or an error.
bool reply(Connection& conn, volproto::Type type, DWORD timeoutMs);
bool replyError(Connection& conn, DWORD code, const wchar_t* message, DWORD timeoutMs);

} // namespace ws::service
