#include "Server.h"
#include "InputListener.h"

#include "Log.h"
#include "Win32Util.h"

#include <dbt.h>
// Defines (not only declares) the GUID_IO_VOLUME_* events.
#include <initguid.h>
#include <ioevent.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>

namespace ws::service {

using namespace volproto;

namespace {

// Everything for the system and administrators; reading and writing for
// interactive users (a logged-on user, on the console or over RDP): not
// services, not scheduled tasks, not AppContainers (Store apps, browser
// sandboxes). Protected: nothing inherited. The label lets medium-integrity
// processes in (the pipe is made by a system process, so it would be high)
// and keeps low ones out.
constexpr wchar_t kSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;FRFW;;;IU)S:(ML;;NW;;;ME)";
constexpr DWORD kOutBufferBytes = 1u << 20; // an MftBlock at a time, without blocking the writer
constexpr DWORD kInBufferBytes = 64u << 10;
constexpr DWORD kStopWaitMs = 15'000;

} // namespace

// ---- DeviceWatch ---------------------------------------------------------------

namespace {

constexpr wchar_t kWindowClass[] = L"WinShunService.DeviceWatch";

LRESULT CALLBACK watchWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DEVICECHANGE) {
        auto* watch = reinterpret_cast<DeviceWatch*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (watch)
            return watch->onEvent(static_cast<DWORD>(wParam), reinterpret_cast<const void*>(lParam));
        return TRUE;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

DeviceWatch::DeviceWatch(HANDLE recipient, DWORD recipientFlags, Release release)
    : m_recipient(recipient)
    , m_flags(recipientFlags)
    , m_release(std::move(release))
{
    if (m_recipient)
        return;
    // Console mode: a message-only window on a thread of its own, which
    // delivers WM_DEVICECHANGE as the service handle would.
    m_windowReady = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_windowThread = std::thread([this] {
        WNDCLASSW wc {};
        wc.lpfnWndProc = watchWindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        ::RegisterClassW(&wc);
        m_window = ::CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (m_window)
            ::SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ::SetEvent(m_windowReady);
        if (!m_window)
            return;
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    });
    ::WaitForSingleObject(m_windowReady, 5000);
    if (m_window) {
        m_recipient = m_window;
        m_flags = DEVICE_NOTIFY_WINDOW_HANDLE;
    }
}

DeviceWatch::~DeviceWatch()
{
    {
        const std::lock_guard lock(m_mutex);
        for (Entry& e : m_entries)
            closeEntry(e);
        m_entries.clear();
    }
    if (m_windowThread.joinable()) {
        if (m_window)
            ::PostMessageW(m_window, WM_QUIT, 0, 0);
        m_windowThread.join();
    }
    if (m_windowReady)
        ::CloseHandle(m_windowReady);
}

void DeviceWatch::closeEntry(Entry& e)
{
    if (e.handle != INVALID_HANDLE_VALUE)
        ::CloseHandle(e.handle);
    e.handle = INVALID_HANDLE_VALUE;
    if (e.token)
        ::UnregisterDeviceNotification(e.token);
    e.token = nullptr;
}

void* DeviceWatch::watch(const std::wstring& root)
{
    if (!m_recipient)
        return nullptr;
    // No access rights: the handle only tells Windows which volume we mean.
    const std::wstring path = L"\\\\.\\" + root;
    const HANDLE handle = ::CreateFileW(
        path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return nullptr;
    DEV_BROADCAST_HANDLE filter {};
    filter.dbch_size = sizeof filter;
    filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
    filter.dbch_handle = handle;
    const HDEVNOTIFY token = ::RegisterDeviceNotificationW(m_recipient, &filter, m_flags);
    if (!token) {
        ::CloseHandle(handle);
        return nullptr;
    }
    const std::lock_guard lock(m_mutex);
    m_entries.push_back({token, handle, root});
    return token;
}

void DeviceWatch::unwatch(void* token)
{
    if (!token)
        return;
    Entry gone {};
    {
        const std::lock_guard lock(m_mutex);
        const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.token == token; });
        if (it == m_entries.end())
            return;
        gone = *it;
        m_entries.erase(it);
    }
    closeEntry(gone);
}

LRESULT DeviceWatch::onEvent(DWORD eventType, const void* data)
{
    const auto* header = static_cast<const DEV_BROADCAST_HDR*>(data);
    if (!header || header->dbch_devicetype != DBT_DEVTYP_HANDLE)
        return TRUE;
    const auto* change = static_cast<const DEV_BROADCAST_HANDLE*>(data);
    const bool letGo = eventType == DBT_DEVICEQUERYREMOVE
        || (eventType == DBT_CUSTOMEVENT && change->dbch_eventguid == GUID_IO_VOLUME_LOCK);
    const bool gone = eventType == DBT_DEVICEREMOVEPENDING || eventType == DBT_DEVICEREMOVECOMPLETE;
    if (!letGo && !gone)
        return TRUE;
    // Every registration on that volume: our handle closes first, then the
    // connections' (the registrations stay until they unwatch, as they would).
    std::vector<std::wstring> roots;
    {
        const std::lock_guard lock(m_mutex);
        for (Entry& e : m_entries) {
            if (e.token != change->dbch_hdevnotify)
                continue;
            if (e.handle != INVALID_HANDLE_VALUE)
                ::CloseHandle(e.handle);
            e.handle = INVALID_HANDLE_VALUE;
            roots.push_back(e.root);
        }
    }
    for (const std::wstring& root : roots) {
        log::write(L"%ls: Windows is taking the volume (%ls): letting go", root.c_str(),
            eventType == DBT_DEVICEQUERYREMOVE ? L"eject" : eventType == DBT_CUSTOMEVENT ? L"lock" : L"removed");
        if (m_release)
            m_release(root);
    }
    return TRUE;
}

// ---- Server --------------------------------------------------------------------

Server::Server(Options options)
    : m_options(std::move(options))
{
}

Server::~Server()
{
    stop();
    if (m_descriptor)
        ::LocalFree(m_descriptor);
    if (m_stopEvent)
        ::CloseHandle(m_stopEvent);
}

HANDLE Server::createInstance(bool first, DWORD* error)
{
    SECURITY_ATTRIBUTES attributes {sizeof attributes, m_descriptor, FALSE};
    DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first)
        flags |= FILE_FLAG_FIRST_PIPE_INSTANCE; // not another program's pipe of this name
    const HANDLE handle = ::CreateNamedPipeW(fullPipePath(m_options.pipeName).c_str(), flags,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
        kOutBufferBytes, kInBufferBytes, 0, &attributes);
    if (handle == INVALID_HANDLE_VALUE && error)
        *error = ::GetLastError();
    return handle;
}

bool Server::start(DWORD* error)
{
    if (m_running)
        return true;
    if (!m_descriptor
        && !::ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &m_descriptor, nullptr)) {
        if (error)
            *error = ::GetLastError();
        return false;
    }
    m_listening = createInstance(true, error);
    if (m_listening == INVALID_HANDLE_VALUE)
        return false;
    if (!m_stopEvent)
        m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::ResetEvent(m_stopEvent);
    m_running = true;
    m_acceptor = std::thread([this] { acceptLoop(); });
    log::write(L"listening on \\\\.\\pipe\\%ls", m_options.pipeName.c_str());
    return true;
}

void Server::stop()
{
    if (!m_running)
        return;
    m_running = false;
    ::SetEvent(m_stopEvent);
    if (m_acceptor.joinable())
        m_acceptor.join();
    if (m_listening != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_listening);
        m_listening = INVALID_HANDLE_VALUE;
    }
    // Every connection lets go and ends itself; wait for the last, a while.
    std::unique_lock lock(m_mutex);
    for (auto& c : m_connections)
        ::SetEvent(c->abort);
    if (!m_finished.wait_for(lock, std::chrono::milliseconds(kStopWaitMs), [this] { return m_connections.empty(); }))
        log::write(L"stopping with %zu connections still open", m_connections.size());
    log::write(L"stopped");
}

unsigned Server::connectionCount() const
{
    const std::lock_guard lock(m_mutex);
    return static_cast<unsigned>(m_connections.size());
}

void Server::acceptLoop()
{
    OVERLAPPED overlapped {};
    overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (m_running) {
        ::ResetEvent(overlapped.hEvent);
        bool connected = ::ConnectNamedPipe(m_listening, &overlapped) != 0;
        if (!connected) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_PIPE_CONNECTED) {
                connected = true; // the client came first
            } else if (code == ERROR_IO_PENDING) {
                const HANDLE events[] = {m_stopEvent, overlapped.hEvent};
                if (::WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    ::CancelIoEx(m_listening, &overlapped);
                    DWORD unused = 0;
                    ::GetOverlappedResult(m_listening, &overlapped, &unused, TRUE);
                    break;
                }
                DWORD unused = 0;
                connected = ::GetOverlappedResult(m_listening, &overlapped, &unused, TRUE) != 0;
            } else {
                log::write(L"ConnectNamedPipe failed (%lu)", code);
                if (::WaitForSingleObject(m_stopEvent, 500) == WAIT_OBJECT_0)
                    break;
            }
        }
        if (!m_running)
            break;
        if (!connected)
            continue;

        auto conn = std::make_shared<Connection>();
        conn->channel = Channel(m_listening);
        conn->abort = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        conn->released = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ::GetNamedPipeClientProcessId(m_listening, &conn->pid);
        {
            const std::lock_guard lock(m_mutex);
            conn->id = m_nextId++;
            m_connections.push_back(conn);
        }
        std::thread([this, conn] { serve(conn); }).detach();

        // The next instance, for the next client. A failure (handles run
        // out?) is tried again after a moment.
        DWORD error = 0;
        m_listening = createInstance(false, &error);
        while (m_listening == INVALID_HANDLE_VALUE && m_running) {
            log::write(L"cannot make another pipe instance (%lu)", error);
            if (::WaitForSingleObject(m_stopEvent, 1000) == WAIT_OBJECT_0)
                break;
            m_listening = createInstance(false, &error);
        }
        if (m_listening == INVALID_HANDLE_VALUE)
            break;
    }
    ::CloseHandle(overlapped.hEvent);
}

bool Server::handshake(Connection& conn)
{
    Type type {};
    std::vector<std::byte> payload;
    if (!conn.channel.read(type, payload, m_options.helloTimeoutMs, conn.abort))
        return false; // never said hello
    Hello hello;
    if (type != Type::Hello || !decode(payload, hello)) {
        log::write(L"pid %lu: no hello (frame %u): dropped", conn.pid, static_cast<unsigned>(type));
        return false;
    }
    Writer w;
    if (hello.major != kMajor) {
        log::write(L"pid %lu (%u.%u.%u): protocol %u, ours %u: refused", conn.pid, hello.version[0], hello.version[1],
            hello.version[2], hello.major, kMajor);
        encode(w, HelloRefused {static_cast<std::uint32_t>(Refusal::Protocol)});
        conn.channel.write(w.finish(Type::HelloRefused), m_options.writeTimeoutMs);
        return false;
    }
    conn.minor = hello.minor;
    unsigned held = 0;
    {
        const std::lock_guard lock(m_mutex);
        for (const auto& c : m_connections)
            held += c->pid == conn.pid ? 1 : 0;
    }
    if (held > m_options.maxPerProcess) {
        log::write(L"pid %lu: %u connections already: refused", conn.pid, held - 1);
        encode(w, HelloRefused {static_cast<std::uint32_t>(Refusal::TooManyConnections)});
        conn.channel.write(w.finish(Type::HelloRefused), m_options.writeTimeoutMs);
        return false;
    }
    HelloOk ok;
    ok.version[0] = WINSHUN_VERSION_MAJOR;
    ok.version[1] = WINSHUN_VERSION_MINOR;
    ok.version[2] = WINSHUN_VERSION_PATCH;
    ok.capabilities = m_options.bulkReads ? kCapabilityBulkRead : 0;
    // Both session helpers start the same way but are not given alike. The
    // key listener only reports double taps of Ctrl, Alt or Shift, so any
    // client the pipe lets in gets it in its own session, as before #104 (a
    // portable copy keeps double taps over admin windows). The navigator acts
    // on another program's elevated window with uiAccess rights, so only the
    // WinShun.exe beside this service gets it; another copy is told it is kept
    // from it, to say so instead of suggesting to install the service.
    if (m_options.keyListener) {
        ok.capabilities |= kCapabilityKeyListener;
        ok.capabilities |= requesterIsWinShun(conn.pid) ? kCapabilityDialogNav : kCapabilityDialogNavBesideOnly;
    }
    encode(w, ok);
    return conn.channel.write(w.finish(Type::HelloOk), m_options.writeTimeoutMs);
}

void Server::serve(std::shared_ptr<Connection> conn)
{
    if (handshake(*conn)) {
        Type type {};
        std::vector<std::byte> payload;
        if (conn->channel.read(type, payload, m_options.helloTimeoutMs, conn->abort)) {
            switch (type) {
            case Type::OpenMft:
                serveOpenMft(*this, *conn, payload);
                break;
            case Type::QueryJournal:
                serveQueryJournal(*this, *conn, payload);
                break;
            case Type::FollowJournal:
                serveFollowJournal(*this, *conn, payload);
                break;
            case Type::CloseVolume:
                serveCloseVolume(*this, *conn, payload);
                break;
            case Type::OpenBulkRead:
                serveOpenBulkRead(*this, *conn, payload);
                break;
            case Type::OpenKeyListener:
                serveInputListener(*this, *conn, payload);
                break;
            case Type::OpenDialogNav:
                serveDialogNav(*this, *conn, payload);
                break;
            default:
                log::write(L"pid %lu: unknown request %u", conn->pid, static_cast<unsigned>(type));
                replyError(*conn, ERROR_INVALID_FUNCTION, L"unknown request", m_options.writeTimeoutMs);
                break;
            }
        }
    }
    unbind(*conn);
    ::FlushFileBuffers(conn->channel.handle()); // the last reply reaches the client before the disconnect
    ::DisconnectNamedPipe(conn->channel.handle());
    conn->channel.close();
    {
        const std::lock_guard lock(m_mutex);
        std::erase(m_connections, conn);
        ::SetEvent(conn->released);
    }
    m_finished.notify_all();
}

bool Server::bind(Connection& conn, std::wstring root, Bind kind)
{
    {
        const std::lock_guard lock(m_mutex);
        if (kind != Bind::Journal) {
            for (const auto& c : m_connections) {
                if (c.get() != &conn && c->bound && c->kind == kind && c->pid == conn.pid
                    && win32::equalsIgnoreCase(c->root, root))
                    return false;
            }
        }
        conn.root = std::move(root);
        conn.bound = true;
        conn.kind = kind;
        ::ResetEvent(conn.released);
    }
    if (m_watch)
        conn.watch = m_watch->watch(conn.root);
    return true;
}

void Server::unbind(Connection& conn)
{
    void* watch = nullptr;
    {
        const std::lock_guard lock(m_mutex);
        if (!conn.bound)
            return;
        conn.bound = false;
        conn.kind = Bind::Journal;
        watch = std::exchange(conn.watch, nullptr);
        ::SetEvent(conn.released);
    }
    if (m_watch)
        m_watch->unwatch(watch);
}

unsigned Server::closeVolume(std::wstring_view root, DWORD pid, DWORD waitMs)
{
    std::vector<std::shared_ptr<Connection>> targets;
    {
        const std::lock_guard lock(m_mutex);
        for (const auto& c : m_connections) {
            if (c->bound && (pid == 0 || c->pid == pid) && win32::equalsIgnoreCase(c->root, root))
                targets.push_back(c);
        }
        for (const auto& c : targets)
            ::SetEvent(c->abort);
    }
    const ULONGLONG deadline = ::GetTickCount64() + waitMs;
    for (const auto& c : targets) {
        const ULONGLONG now = ::GetTickCount64();
        if (::WaitForSingleObject(c->released, now < deadline ? static_cast<DWORD>(deadline - now) : 0) != WAIT_OBJECT_0)
            log::write(L"%ls: connection %u of pid %lu did not let go in time", std::wstring(root).c_str(), c->id, c->pid);
    }
    return static_cast<unsigned>(targets.size());
}

} // namespace ws::service
