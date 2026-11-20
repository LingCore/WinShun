#include "Engine.h"

#include "Network.h"
#include "Saver.h"
#include "Util.h"
#include "winshun.h"

#include <ws2tcpip.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace shunsync {

using winshun::Json;

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace {

constexpr std::size_t kMaxHandshakeFrame = 64 * 1024;
constexpr std::size_t kMaxQueuedBytes = 96u * 1024 * 1024; // a member that takes no more is let go
constexpr std::size_t kMaxText = 4u * 1024 * 1024; // UTF-8; a clip_text frame stays under 20 MB however it is escaped
constexpr std::size_t kMaxEntries = 20000; // files and folders in one copy
constexpr std::size_t kMaxPendingPairs = 1; // one at a time, as Mac顺
constexpr std::int64_t kPairAskMs = 8'000; // one request per device (or address) in this time, as Mac顺
constexpr std::size_t kPacedBytes = 4u * 1024 * 1024; // a file served over a sync link: at most this much queued
constexpr std::size_t kRelayBytes = 64u * 1024 * 1024; // a pull over a sync link this far behind: given up
constexpr std::int64_t kStalledMs = 60'000; // a sender that stops sending
constexpr std::size_t kReceivedKept = 20;
constexpr std::int64_t kPingMs = 20'000;
constexpr std::int64_t kDeadMs = 60'000;
constexpr std::int64_t kBeaconMs = 5'000;
constexpr std::int64_t kNearbyMs = 20'000;
constexpr std::int64_t kEchoMs = 5'000;
constexpr std::int64_t kCollisionMs = 5'000;
constexpr std::int64_t kOutgoingMs = 24ll * 3600 * 1000;
constexpr std::uint64_t kCacheBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::int64_t kCacheMs = 7ll * 24 * 3600 * 1000;

std::string socketHost(const sockaddr_storage& address)
{
    char text[INET6_ADDRSTRLEN] {};
    if (address.ss_family == AF_INET)
        ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in&>(address).sin_addr, text, sizeof text);
    else if (address.ss_family == AF_INET6)
        ::inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6&>(address).sin6_addr, text, sizeof text);
    return text;
}

void setTimeout(SOCKET s, int option, DWORD ms)
{
    ::setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&ms), sizeof ms);
}

bool readExact(SOCKET s, char* buffer, std::size_t size)
{
    while (size > 0) {
        const int n = ::recv(s, buffer, static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
        if (n <= 0)
            return false;
        buffer += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool writeAll(SOCKET s, const char* data, std::size_t size)
{
    while (size > 0) {
        const int n = ::send(s, data, static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
        if (n <= 0)
            return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

// A frame: four bytes of length (big-endian), then that many. Nothing on a
// length of 0 or above `max` (the other end is broken or hostile: the
// connection ends).
std::optional<Bytes> readFrame(SOCKET s, std::size_t max)
{
    char head[4];
    if (!readExact(s, head, 4))
        return std::nullopt;
    const std::uint32_t size = be32(std::string_view(head, 4));
    if (size == 0 || size > max)
        return std::nullopt;
    Bytes payload(size, '\0');
    if (!readExact(s, payload.data(), size))
        return std::nullopt;
    return payload;
}

bool writeFrame(SOCKET s, std::string_view payload)
{
    Bytes framed;
    framed.reserve(payload.size() + 4);
    putBe32(framed, static_cast<std::uint32_t>(payload.size()));
    framed.append(payload);
    return writeAll(s, framed.data(), framed.size());
}

std::optional<Json> parseJson(std::string_view text)
{
    if (!text.empty() && text.front() == static_cast<char>(FrameKind::Json))
        text.remove_prefix(1); // a handshake frame sent with its kind in front: taken as well
    auto json = Json::parse(std::string(text));
    if (!json || !json->isObject())
        return std::nullopt;
    return json;
}

Bytes jsonFrame(const Json& message)
{
    Bytes plain(1, static_cast<char>(FrameKind::Json));
    plain += message.dump();
    return plain;
}

SOCKET connectTo(const std::string& host, std::uint16_t port, int timeoutMs)
{
    addrinfoW hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* found = nullptr;
    if (::GetAddrInfoW(localsend::wide(host).c_str(), std::to_wstring(port).c_str(), &hints, &found) != 0)
        return INVALID_SOCKET;
    SOCKET result = INVALID_SOCKET;
    for (addrinfoW* a = found; a && result == INVALID_SOCKET; a = a->ai_next) {
        const SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        u_long nonBlocking = 1;
        ::ioctlsocket(s, FIONBIO, &nonBlocking);
        ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval wait {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        int error = 0;
        int length = sizeof error;
        if (::select(0, nullptr, &writable, &failed, &wait) == 1 && FD_ISSET(s, &writable)
            && ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0) {
            nonBlocking = 0;
            ::ioctlsocket(s, FIONBIO, &nonBlocking);
            result = s;
        } else {
            ::closesocket(s);
        }
    }
    ::FreeAddrInfoW(found);
    return result;
}

void tuned(SOCKET s)
{
    BOOL on = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
    setTimeout(s, SO_SNDTIMEO, 30'000);
    setTimeout(s, SO_RCVTIMEO, 15'000); // the handshake; none once it is done
}

std::string normalizedText(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;
        out += text[i];
    }
    return out;
}

std::string previewOf(const std::string& text)
{
    std::string line;
    for (const char c : text) {
        line += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        if (line.size() >= 300)
            break;
    }
    return localsend::cutUtf8(localsend::trimmed(line), 160);
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty())
        return b;
    return a.back() == L'\\' ? a + b : a + L'\\' + b;
}

// "\\?\" in front of a full path: past MAX_PATH too.
std::wstring longPath(const std::wstring& path)
{
    if (path.rfind(L"\\\\", 0) == 0)
        return path;
    return L"\\\\?\\" + path;
}

FILETIME fileTimeOf(std::int64_t unixMs)
{
    const std::uint64_t ticks = static_cast<std::uint64_t>(unixMs) * 10'000 + 116444736000000000ull;
    return FILETIME {static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
}

std::int64_t unixMsOf(const FILETIME& time)
{
    const std::uint64_t ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return ticks < 116444736000000000ull ? 0 : static_cast<std::int64_t>((ticks - 116444736000000000ull) / 10'000);
}

std::string pathsFingerprint(const std::vector<std::wstring>& paths)
{
    std::string joined = "files:";
    for (const std::wstring& path : paths)
        joined += localsend::narrow(path) + '\n';
    return hexLower(sha256(joined));
}

std::string platformName(const std::string& platform)
{
    if (platform == "macos")
        return "macOS";
    if (platform == "windows")
        return "Windows";
    return platform;
}

} // namespace

// ---- a connection -------------------------------------------------------------

struct Engine::Conn {
    SOCKET socket = INVALID_SOCKET;
    bool outbound = false;
    std::string mode; // sync | file | pair
    std::string peerId;
    std::string peerName;
    std::string peerPlatform;
    std::string host; // the other end's address, as seen here
    std::uint16_t peerPort = 0; // where it listens
    std::unique_ptr<FrameSealer> sealer;
    std::unique_ptr<FrameOpener> opener;
    std::int64_t createdMs = nowMs();
    std::atomic<std::int64_t> lastRecvMs {nowMs()};
    std::atomic<std::int64_t> lastPingMs {nowMs()};
    std::atomic_bool closed {false};

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<Bytes> queue; // plaintexts, sealed in this order by the writer
    std::size_t queued = 0;
    bool closeWhenFlushed = false;
    std::thread writer;
    std::mutex writeMutex; // a write straight to the socket (the handshake, file frames) vs the writer
    std::mutex serving; // file_pull_req on a sync link: one file after another

    void close()
    {
        if (!closed.exchange(true))
            ::shutdown(socket, SD_BOTH); // the reader's recv returns; the socket is closed by whoever owns it
        queueCv.notify_all();
    }
    bool writeSealed(std::string_view plaintext)
    {
        const std::lock_guard lock(writeMutex);
        return !closed && sealer && writeFrame(socket, sealer->seal(plaintext));
    }
    std::optional<Bytes> readSealed(std::size_t max = kMaxSealedFrame)
    {
        std::optional<Bytes> frame = readFrame(socket, max);
        if (!frame || !opener)
            return std::nullopt;
        std::optional<Bytes> plain = opener->open(*frame);
        if (plain && !plain->empty())
            lastRecvMs = nowMs();
        return plain && !plain->empty() ? plain : std::nullopt;
    }
    void startWriter()
    {
        writer = std::thread([this] {
            for (;;) {
                Bytes next;
                bool flushThenClose = false;
                {
                    std::unique_lock lock(queueMutex);
                    queueCv.wait(lock, [this] { return closed || !queue.empty() || closeWhenFlushed; });
                    if (closed)
                        return;
                    if (queue.empty()) {
                        flushThenClose = closeWhenFlushed;
                    } else {
                        next = std::move(queue.front());
                        queue.pop_front();
                        queued -= next.size();
                    }
                }
                queueCv.notify_all(); // room for a file served on this link
                if (flushThenClose) {
                    ::Sleep(200); // the last frame on its way before the end
                    close();
                    return;
                }
                if (!writeSealed(next)) {
                    close();
                    return;
                }
            }
        });
    }
    void finish() // by the thread that read it: after this, nobody uses the socket
    {
        close();
        if (writer.joinable())
            writer.join();
        ::closesocket(socket);
        socket = INVALID_SOCKET;
    }
};

// ---- the engine ---------------------------------------------------------------------

Engine::Engine(Options options, ClipboardSink& sink, Events events)
    : m_options(std::move(options)), m_sink(sink), m_events(std::move(events))
{
}

Engine::~Engine() { stop(); }

bool Engine::zh() const
{
    const std::lock_guard lock(m_mutex);
    return m_options.chinese;
}

void Engine::changed()
{
    if (m_events.changed)
        m_events.changed();
}

void Engine::log(const std::string& line)
{
    if (m_events.log)
        m_events.log(line);
}

void Engine::toast(const std::string& title, const std::string& text)
{
    if (m_events.toast)
        m_events.toast(title, text);
}

std::string Engine::deviceId() const
{
    const std::lock_guard lock(m_mutex);
    return m_deviceId;
}

void Engine::spawn(const char* what, std::function<void()> work)
{
    auto done = std::make_shared<std::atomic_bool>(false);
    std::thread thread([this, what, work = std::move(work), done] {
        try {
            work();
        } catch (const std::exception& e) {
            log(std::string(what) + " stopped: " + e.what());
        } catch (...) {
            log(std::string(what) + " stopped");
        }
        done->store(true);
    });
    const std::lock_guard lock(m_threadsMutex);
    m_threads.emplace_back(std::move(thread), std::move(done));
}

void Engine::reap(bool all)
{
    std::vector<std::thread> joining;
    {
        const std::lock_guard lock(m_threadsMutex);
        for (auto it = m_threads.begin(); it != m_threads.end();) {
            if (all || it->second->load()) {
                joining.push_back(std::move(it->first));
                it = m_threads.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (std::thread& t : joining) {
        if (t.joinable())
            t.join();
    }
}

bool Engine::start(std::string* problem)
{
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (problem)
            *problem = "WSAStartup";
        return false;
    }
    loadIdentity();
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        if (problem)
            *problem = "socket";
        return false;
    }
    BOOL exclusive = TRUE;
    ::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    ::inet_pton(AF_INET, m_options.bindAddress.c_str(), &address.sin_addr);
    address.sin_port = ::htons(m_options.preferredPort);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        address.sin_port = 0; // taken (another program, an old copy): any, told in the beacons
        if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
            ::closesocket(listener);
            if (problem)
                *problem = "bind";
            return false;
        }
    }
    int length = sizeof address;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    if (::listen(listener, 16) != 0) {
        ::closesocket(listener);
        if (problem)
            *problem = "listen";
        return false;
    }
    m_port = ::ntohs(address.sin_port);
    {
        const std::lock_guard lock(m_mutex);
        m_listener = listener;
        m_running = true;
    }
    m_stopping = false;
    m_acceptThread = std::thread([this, listener] { acceptLoop(listener); });
    if (m_options.discovery)
        m_beaconThread = std::thread([this] { beaconLoop(); });
    m_tickThread = std::thread([this] { tickLoop(); });
    spawn("cache", [this] { sweepCache(); });
    log("listening on port " + std::to_string(m_port.load()));
    return true;
}

void Engine::stop()
{
    if (m_stopping.exchange(true))
        return;
    std::vector<std::shared_ptr<Conn>> conns;
    {
        const std::lock_guard lock(m_mutex);
        m_running = false;
        if (m_listener != INVALID_SOCKET)
            ::closesocket(m_listener); // accept() returns; its thread ends
        m_listener = INVALID_SOCKET;
        conns.assign(m_conns.begin(), m_conns.end());
        for (auto& [id, incoming] : m_incoming) {
            if (incoming.cancel)
                incoming.cancel->store(true);
        }
    }
    for (const auto& conn : conns)
        conn->close();
    m_wake.notify_all();
    if (m_acceptThread.joinable())
        m_acceptThread.join();
    if (m_beaconThread.joinable())
        m_beaconThread.join();
    if (m_tickThread.joinable())
        m_tickThread.join();
    reap(true);
    ::WSACleanup();
}

void Engine::setOptions(const Options& options)
{
    bool renamed = false;
    {
        const std::lock_guard lock(m_mutex);
        renamed = options.deviceName != m_options.deviceName;
        const std::wstring dataDir = m_options.dataDir;
        const std::string bind = m_options.bindAddress;
        const std::uint16_t preferred = m_options.preferredPort;
        const bool discovery = m_options.discovery;
        m_options = options;
        m_options.dataDir = dataDir;
        m_options.bindAddress = bind;
        m_options.preferredPort = preferred;
        m_options.discovery = discovery;
        m_dialing.clear(); // addresses typed in: tried again now
    }
    if (renamed) {
        Json update = Json::Object {{"type", "peer_update"}, {"deviceName", options.deviceName}};
        {
            const std::lock_guard lock(m_mutex);
            update.set("peers", peersLocked({}));
        }
        broadcast(jsonFrame(update), {});
    }
    m_wake.notify_all();
    changed();
}

// ---- identity -------------------------------------------------------------------

void Engine::loadIdentity()
{
    const std::wstring path = joinPath(m_options.dataDir, L"identity.json");
    std::string text;
    if (localsend::Handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        file.valid()) {
        LARGE_INTEGER size {};
        if (::GetFileSizeEx(file.get(), &size) && size.QuadPart < 4 * 1024 * 1024) {
            text.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            if (!::ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
                text.clear();
            text.resize(read);
        }
    }
    const std::lock_guard lock(m_mutex);
    const std::optional<Json> json = text.empty() ? std::nullopt : Json::parse(text);
    if (json && json->isObject()) {
        m_deviceId = (*json)["deviceId"].toString();
        const std::string stored = (*json)["groupKey"].toString();
        if (stored.rfind("dpapi:", 0) == 0) {
            if (const auto blob = fromBase64(stored.substr(6)))
                if (const auto key = unprotectForUser(*blob))
                    m_groupKey = *key;
        }
        for (const Json& id : (*json)["revokedDeviceIds"].toArray())
            m_revoked.push_back(id.toString());
        for (const Json& p : (*json)["knownPeers"].toArray()) {
            Peer peer;
            peer.deviceId = p["deviceId"].toString();
            peer.name = p["deviceName"].toString();
            peer.platform = p["platform"].toString();
            peer.host = p["host"].toString();
            peer.port = static_cast<std::uint16_t>(p["port"].toInt());
            peer.lastSeenMs = p["lastSeenMs"].toInt();
            if (!peer.deviceId.empty() && peer.deviceId != m_deviceId)
                m_peers[peer.deviceId] = std::move(peer);
        }
    }
    if (!uuidBytes(m_deviceId))
        m_deviceId = newUuid();
    if (m_groupKey.size() != kKeyBytes) {
        newGroupLocked(randomBytes(kKeyBytes), false); // a group of one until another device joins
        return;
    }
    m_groupId = groupIdOf(m_groupKey);
}

void Engine::saveIdentityLocked() const
{
    const std::optional<Bytes> protectedKey = protectForUser(m_groupKey);
    if (!protectedKey)
        return;
    Json::Array revoked;
    for (const std::string& id : m_revoked)
        revoked.push_back(id);
    Json::Array peers;
    for (const auto& [id, peer] : m_peers) {
        peers.push_back(Json::Object {{"deviceId", id}, {"deviceName", peer.name}, {"platform", peer.platform},
            {"host", peer.host}, {"port", static_cast<int>(peer.port)}, {"lastSeenMs", static_cast<double>(peer.lastSeenMs)}});
    }
    const Json json = Json::Object {{"deviceId", m_deviceId}, {"groupKey", "dpapi:" + base64(*protectedKey)},
        {"revokedDeviceIds", Json(std::move(revoked))}, {"knownPeers", Json(std::move(peers))}};
    const std::string text = json.dump();
    ::CreateDirectoryW(m_options.dataDir.c_str(), nullptr);
    const std::wstring path = joinPath(m_options.dataDir, L"identity.json");
    const std::wstring temp = path + L".new";
    {
        localsend::Handle file(::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        if (!file.valid() || !::WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
            || written != text.size())
            return;
        ::FlushFileBuffers(file.get());
    }
    ::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void Engine::newGroupLocked(Bytes key, bool keepPeers)
{
    m_groupKey = std::move(key);
    m_groupId = groupIdOf(m_groupKey);
    m_revoked.clear();
    if (!keepPeers)
        m_peers.clear();
    saveIdentityLocked();
}

void Engine::notePeerLocked(const std::string& id, const std::string& name, const std::string& platform,
    const std::string& host, std::uint16_t port)
{
    if (id.empty() || id == m_deviceId || std::ranges::find(m_revoked, id) != m_revoked.end())
        return;
    Peer& peer = m_peers[id];
    peer.deviceId = id;
    if (!name.empty())
        peer.name = localsend::cutUtf8(name, 128);
    if (!platform.empty())
        peer.platform = localsend::cutUtf8(platform, 32);
    if (!host.empty() && port) {
        peer.host = host;
        peer.port = port;
    }
    peer.lastSeenMs = nowMs();
}

Json Engine::peersLocked(const std::string& except) const
{
    Json::Array list;
    for (const auto& [id, conn] : m_links) {
        if (id == except)
            continue;
        const auto peer = m_peers.find(id);
        const std::string host = peer != m_peers.end() && !peer->second.host.empty() ? peer->second.host : conn->host;
        const std::uint16_t port = peer != m_peers.end() && peer->second.port ? peer->second.port : conn->peerPort;
        list.push_back(Json::Object {{"deviceId", id}, {"deviceName", conn->peerName}, {"platform", conn->peerPlatform},
            {"host", host}, {"port", static_cast<int>(port)}});
    }
    return Json(std::move(list));
}

void Engine::learnPeers(const Json& peers)
{
    bool any = false;
    {
        const std::lock_guard lock(m_mutex);
        for (const Json& p : peers.toArray()) {
            const std::string id = p["deviceId"].toString();
            if (!uuidBytes(id) || id == m_deviceId)
                continue;
            const bool known = m_peers.count(id) > 0;
            const auto port = p["port"].toInt();
            notePeerLocked(id, p["deviceName"].toString(), p["platform"].toString(), p["host"].toString(),
                port > 0 && port < 65536 ? static_cast<std::uint16_t>(port) : 0);
            if (!known && m_peers.count(id)) {
                m_peers[id].nextTryMs = 0; // dialed at the next tick
                m_peers[id].lastSeenMs = 0;
                any = true;
            }
        }
        if (any)
            saveIdentityLocked();
    }
    if (any)
        m_wake.notify_all();
}

// ---- connections ------------------------------------------------------------------------

std::vector<std::shared_ptr<Engine::Conn>> Engine::links() const
{
    const std::lock_guard lock(m_mutex);
    std::vector<std::shared_ptr<Conn>> out;
    for (const auto& [id, conn] : m_links)
        out.push_back(conn);
    return out;
}

void Engine::send(const std::shared_ptr<Conn>& conn, Bytes plaintext)
{
    if (!conn || conn->closed)
        return;
    {
        const std::lock_guard lock(conn->queueMutex);
        if (conn->queued + plaintext.size() > kMaxQueuedBytes) {
            log("a member takes nothing more: let go");
            conn->close();
            return;
        }
        conn->queued += plaintext.size();
        conn->queue.push_back(std::move(plaintext));
    }
    conn->queueCv.notify_all();
}

void Engine::sendJson(const std::shared_ptr<Conn>& conn, const Json& message) { send(conn, jsonFrame(message)); }

void Engine::broadcast(const Bytes& plaintext, const std::string& target)
{
    for (const auto& conn : links()) {
        if (target.empty() || conn->peerId == target)
            send(conn, plaintext);
    }
}

void Engine::acceptLoop(SOCKET listener)
{
    for (;;) {
        sockaddr_storage from {};
        int length = sizeof from;
        const SOCKET s = ::accept(listener, reinterpret_cast<sockaddr*>(&from), &length);
        if (s == INVALID_SOCKET) {
            if (m_stopping)
                return;
            ::Sleep(100);
            continue;
        }
        auto conn = std::make_shared<Conn>();
        conn->socket = s;
        conn->host = socketHost(from);
        tuned(s);
        {
            const std::lock_guard lock(m_mutex);
            if (m_stopping || m_conns.size() >= 64) {
                ::closesocket(s);
                continue;
            }
            m_conns.insert(conn);
        }
        spawn("connection", [this, conn] {
            serve(conn);
            conn->finish();
            const std::lock_guard lock(m_mutex);
            m_conns.erase(conn);
        });
        reap(false);
    }
}

void Engine::serve(std::shared_ptr<Conn> conn)
{
    const std::optional<Bytes> helloFrame = readFrame(conn->socket, kMaxHandshakeFrame);
    const std::optional<Json> hello = helloFrame ? parseJson(*helloFrame) : std::nullopt;
    if (!hello || (*hello)["type"].toString() != "hello" || (*hello)["proto"].toInt() != kProtocol)
        return;
    const std::string mode = (*hello)["mode"].toString();
    const std::string clientId = (*hello)["deviceId"].toString();
    const auto clientPub = fromBase64((*hello)["ephPub"].toString());
    const auto clientNonce = fromBase64((*hello)["nonce"].toString());
    const auto listenPort = (*hello)["listenPort"].toInt();
    if ((mode != "sync" && mode != "file" && mode != "pair") || clientId.empty() || clientId.size() > 64 || !clientPub
        || clientPub->size() != 32 || !clientNonce || clientNonce->size() != kNonceBytes)
        return;
    conn->mode = mode;
    conn->peerId = clientId;
    conn->peerName = localsend::cutUtf8(localsend::validUtf8((*hello)["deviceName"].toString()), 128);
    conn->peerPlatform = localsend::cutUtf8((*hello)["platform"].toString(), 32);
    conn->peerPort = listenPort > 0 && listenPort < 65536 ? static_cast<std::uint16_t>(listenPort) : 0;

    std::string refusal;
    std::string myId, myName;
    Bytes groupKey;
    {
        const std::lock_guard lock(m_mutex);
        myId = m_deviceId;
        myName = m_options.deviceName;
        groupKey = m_groupKey;
        if (clientId == m_deviceId)
            refusal = "self";
        else if (mode == "pair") {
            // Answered whenever sync is on (Mac顺's default, 别的设备可以请求加入), or
            // only in the 2-minute window; one at a time, a device once in 8 s.
            std::size_t waiting = 0;
            for (const auto& [id, pair] : m_pairs)
                waiting += pair.view.incoming && pair.view.stage == PairView::Stage::Confirm;
            const std::int64_t now = nowMs();
            std::erase_if(m_pairAsked, [&](const auto& asked) { return now - asked.second >= 60'000; });
            const std::string asker = clientId.empty() ? conn->host : clientId;
            if (!m_options.acceptPairing && m_pairingUntil <= now)
                refusal = "pairing_closed";
            else if (waiting >= kMaxPendingPairs)
                refusal = "pairing_busy";
            else if (const auto asked = m_pairAsked.find(asker); asked != m_pairAsked.end() && now - asked->second < kPairAskMs)
                refusal = "pairing_rate_limited";
            else
                m_pairAsked[asker] = now;
        } else if ((*hello)["groupId"].toString() != m_groupId)
            refusal = "wrong_group"; // Mac顺 then asks to pair instead
        else if (std::ranges::find(m_revoked, clientId) != m_revoked.end())
            refusal = "revoked";
        else if (m_options.paused && mode == "sync")
            refusal = "paused";
    }
    const Json::Object ackBase {{"type", "hello_ack"}, {"proto", kProtocol}, {"deviceId", myId}, {"deviceName", myName},
        {"platform", m_options.platform}, {"listenPort", static_cast<int>(m_port.load())}};
    if (!refusal.empty()) {
        Json ack = ackBase;
        ack.set("accepted", false);
        ack.set("error", refusal);
        writeFrame(conn->socket, ack.dump());
        return;
    }
    const Bytes priv = randomBytes(32);
    const Bytes pub = x25519Public(priv);
    const Bytes nonce = randomBytes(kNonceBytes);
    const Bytes shared = x25519(priv, *clientPub);
    const Bytes transcript = transcriptOf(mode, *clientNonce, nonce, *clientPub, pub, clientId, myId);
    Json ack = ackBase;
    ack.set("accepted", true);
    ack.set("ephPub", base64(pub));
    ack.set("nonce", base64(nonce));

    if (mode == "pair") {
        const PairKeys keys = pairKeys(shared, transcript);
        conn->sealer = std::make_unique<FrameSealer>(keys.s2c);
        conn->opener = std::make_unique<FrameOpener>(keys.c2s);
        if (!writeFrame(conn->socket, ack.dump()))
            return;
        setTimeout(conn->socket, SO_RCVTIMEO, 0);
        conn->startWriter();
        serverPair(conn, clientId, conn->peerName, conn->peerPlatform, keys.sas, conn->peerPort);
        // Until the user answers here, or the other end gives up.
        while (const std::optional<Bytes> plain = conn->readSealed(kMaxHandshakeFrame)) {
            if (static_cast<FrameKind>((*plain)[0]) != FrameKind::Json)
                continue;
            const std::optional<Json> message = parseJson(std::string_view(*plain).substr(1));
            if (message && (*message)["type"].toString() == "pair_cancel")
                break;
        }
        bool lost = false;
        {
            const std::lock_guard lock(m_mutex);
            for (auto& [id, pair] : m_pairs) {
                if (pair.conn == conn) {
                    if (pair.view.stage == PairView::Stage::Confirm) {
                        pair.view.stage = PairView::Stage::Failed;
                        pair.view.problem = "canceled";
                        pair.updatedMs = nowMs();
                        lost = true;
                    }
                    pair.conn.reset();
                }
            }
        }
        if (lost)
            changed();
        return;
    }

    const SyncKeys keys = syncKeys(shared, groupKey, transcript);
    ack.set("proof", base64(keys.serverProof));
    conn->sealer = std::make_unique<FrameSealer>(keys.s2c);
    conn->opener = std::make_unique<FrameOpener>(keys.c2s);
    if (!writeFrame(conn->socket, ack.dump()))
        return;
    const std::optional<Bytes> finishFrame = conn->readSealed(kMaxHandshakeFrame);
    if (!finishFrame || static_cast<FrameKind>((*finishFrame)[0]) != FrameKind::Json)
        return;
    const std::optional<Json> finish = parseJson(std::string_view(*finishFrame).substr(1));
    const auto proof = finish ? fromBase64((*finish)["proof"].toString()) : std::nullopt;
    if (!finish || (*finish)["type"].toString() != "auth_finish" || !proof || !sameBytes(*proof, keys.clientProof)) {
        log("a device without this group's key was turned away");
        return;
    }
    Json ok = Json::Object {{"type", "auth_ok"}};
    {
        const std::lock_guard lock(m_mutex);
        ok.set("peers", peersLocked(clientId));
    }
    if (!conn->writeSealed(jsonFrame(ok)))
        return;
    setTimeout(conn->socket, SO_RCVTIMEO, 0);
    if (mode == "file") {
        serveFiles(conn);
        return;
    }
    adopt(conn);
    runSync(conn);
}

bool Engine::clientHandshake(Conn& conn, const std::string& mode, std::string* problem)
{
    std::string myId, myName, groupId;
    Bytes groupKey;
    {
        const std::lock_guard lock(m_mutex);
        myId = m_deviceId;
        myName = m_options.deviceName;
        groupId = m_groupId;
        groupKey = m_groupKey;
    }
    const Bytes priv = randomBytes(32);
    const Bytes pub = x25519Public(priv);
    const Bytes nonce = randomBytes(kNonceBytes);
    Json hello = Json::Object {{"type", "hello"}, {"proto", kProtocol}, {"mode", mode}, {"deviceId", myId},
        {"deviceName", myName}, {"platform", m_options.platform}, {"ephPub", base64(pub)}, {"nonce", base64(nonce)},
        {"listenPort", static_cast<int>(m_port.load())}};
    hello.set("groupId", mode == "pair" ? Json(nullptr) : Json(groupId));
    conn.mode = mode;
    if (!writeFrame(conn.socket, hello.dump())) {
        *problem = "unreachable";
        return false;
    }
    const std::optional<Bytes> ackFrame = readFrame(conn.socket, kMaxHandshakeFrame);
    const std::optional<Json> ack = ackFrame ? parseJson(*ackFrame) : std::nullopt;
    if (!ack || (*ack)["type"].toString() != "hello_ack") {
        *problem = "unreachable";
        return false;
    }
    if (!(*ack)["accepted"].toBool()) {
        *problem = (*ack)["error"].isString() ? (*ack)["error"].toString() : "declined";
        return false;
    }
    const std::string serverId = (*ack)["deviceId"].toString();
    const auto serverPub = fromBase64((*ack)["ephPub"].toString());
    const auto serverNonce = fromBase64((*ack)["nonce"].toString());
    if (serverId.empty() || serverId.size() > 64 || serverId == myId || !serverPub || serverPub->size() != 32 || !serverNonce
        || serverNonce->size() != kNonceBytes) {
        *problem = "unreachable";
        return false;
    }
    conn.peerId = serverId;
    conn.peerName = localsend::cutUtf8(localsend::validUtf8((*ack)["deviceName"].toString()), 128);
    conn.peerPlatform = localsend::cutUtf8((*ack)["platform"].toString(), 32);
    if (const auto port = (*ack)["listenPort"].toInt(); port > 0 && port < 65536)
        conn.peerPort = static_cast<std::uint16_t>(port);
    const Bytes shared = x25519(priv, *serverPub);
    const Bytes transcript = transcriptOf(mode, nonce, *serverNonce, pub, *serverPub, myId, serverId);
    if (mode == "pair") {
        const PairKeys keys = pairKeys(shared, transcript);
        conn.sealer = std::make_unique<FrameSealer>(keys.c2s);
        conn.opener = std::make_unique<FrameOpener>(keys.s2c);
        *problem = keys.sas; // the code to show
        return true;
    }
    const SyncKeys keys = syncKeys(shared, groupKey, transcript);
    const auto proof = fromBase64((*ack)["proof"].toString());
    if (!proof || !sameBytes(*proof, keys.serverProof)) {
        *problem = "wrong_group"; // it does not hold our key
        return false;
    }
    conn.sealer = std::make_unique<FrameSealer>(keys.c2s);
    conn.opener = std::make_unique<FrameOpener>(keys.s2c);
    if (!conn.writeSealed(jsonFrame(Json::Object {{"type", "auth_finish"}, {"proof", base64(keys.clientProof)}}))) {
        *problem = "unreachable";
        return false;
    }
    const std::optional<Bytes> okFrame = conn.readSealed(kMaxHandshakeFrame);
    const std::optional<Json> ok = okFrame && static_cast<FrameKind>((*okFrame)[0]) == FrameKind::Json
        ? parseJson(std::string_view(*okFrame).substr(1))
        : std::nullopt;
    if (!ok || (*ok)["type"].toString() != "auth_ok") {
        *problem = "wrong_group";
        return false;
    }
    setTimeout(conn.socket, SO_RCVTIMEO, 0);
    if (mode == "sync")
        learnPeers((*ok)["peers"]);
    return true;
}

void Engine::dial(std::string host, std::uint16_t port, std::string expectId)
{
    // A typed address (expectId ""): tried again 15 s after a failure, and
    // while its member is online, not at all. One from an invite is
    // forgotten once it worked: the member is known by then.
    const std::string key = host + ":" + std::to_string(port);
    const auto ended = [&](bool reached, const std::string& peerId) {
        const std::lock_guard lock(m_mutex);
        if (const auto peer = m_peers.find(expectId); peer != m_peers.end()) {
            peer->second.connecting = false;
            if (reached) {
                peer->second.failures = 0;
                peer->second.nextTryMs = nowMs() + 2'000;
            } else {
                peer->second.failures = std::min(peer->second.failures + 1, 5);
                peer->second.nextTryMs = nowMs() + std::min<std::int64_t>(60'000, 3'000ll << peer->second.failures);
            }
        }
        if (expectId.empty()) {
            const bool typed = std::ranges::find(m_options.addresses, key) != m_options.addresses.end()
                || std::ranges::find(m_options.addresses, host) != m_options.addresses.end();
            if (reached && !typed && m_peers.count(peerId))
                m_dialing.erase(key);
            else
                m_dialing[key] = nowMs() + (reached ? 2'000 : 15'000);
        }
    };
    const SOCKET s = connectTo(host, port, 5000);
    if (s == INVALID_SOCKET) {
        ended(false, {});
        return;
    }
    auto conn = std::make_shared<Conn>();
    conn->socket = s;
    conn->outbound = true;
    conn->host = host;
    conn->peerPort = port;
    tuned(s);
    {
        const std::lock_guard lock(m_mutex);
        if (m_stopping) {
            ::closesocket(s);
            return;
        }
        m_conns.insert(conn);
    }
    std::string problem;
    bool ok = clientHandshake(*conn, "sync", &problem);
    if (ok && !expectId.empty() && conn->peerId != expectId) {
        ok = false; // another device at that address now
        problem = "someone else";
    }
    bool already = false;
    if (ok && expectId.empty()) {
        const std::lock_guard lock(m_mutex);
        already = m_links.count(conn->peerId) > 0; // online by another way: this one is not needed
    }
    if (ok && !already) {
        adopt(conn);
        runSync(conn); // until it ends
    } else if (!ok) {
        log("dial: " + problem);
    }
    conn->finish();
    {
        const std::lock_guard lock(m_mutex);
        m_conns.erase(conn);
    }
    ended(ok, conn->peerId);
}

void Engine::adopt(const std::shared_ptr<Conn>& conn)
{
    bool fresh = false;
    std::shared_ptr<Conn> replaced;
    {
        const std::lock_guard lock(m_mutex);
        if (m_stopping || std::ranges::find(m_revoked, conn->peerId) != m_revoked.end()) {
            conn->close();
            return;
        }
        if (const auto existing = m_links.find(conn->peerId); existing != m_links.end() && existing->second != conn) {
            // Both dialed at once (#4.1): the one dialed by the smaller id stays,
            // on both ends alike. An older one gave way: the other end found it dead.
            const auto initiator = [this](const Conn& c) { return c.outbound ? m_deviceId : c.peerId; };
            const bool collision = conn->createdMs - existing->second->createdMs < kCollisionMs;
            if (collision && initiator(*existing->second) < initiator(*conn)) {
                conn->close();
                return;
            }
            replaced = existing->second;
        }
        fresh = m_peers.count(conn->peerId) == 0;
        m_links[conn->peerId] = conn;
        notePeerLocked(conn->peerId, conn->peerName, conn->peerPlatform, conn->host, conn->peerPort);
        if (auto peer = m_peers.find(conn->peerId); peer != m_peers.end())
            peer->second.failures = 0;
        saveIdentityLocked();
    }
    if (replaced)
        replaced->close();
    conn->startWriter();
    log(std::string("member online (") + (conn->outbound ? "dialed" : "accepted") + ")");
    if (fresh) {
        // The others learn of it, and dial it (a mesh of three or more).
        Json update = Json::Object {{"type", "peer_update"}};
        {
            const std::lock_guard lock(m_mutex);
            update.set("deviceName", m_options.deviceName);
            update.set("peers", peersLocked({}));
        }
        for (const auto& link : links()) {
            if (link != conn)
                sendJson(link, update);
        }
    }
    changed();
}

void Engine::runSync(const std::shared_ptr<Conn>& conn)
{
    while (const std::optional<Bytes> plain = conn->readSealed()) {
        const auto kind = static_cast<FrameKind>((*plain)[0]);
        if (kind == FrameKind::Json)
            onJson(conn, plain->substr(1));
        else if (kind == FrameKind::Image)
            onImage(conn, std::string_view(*plain).substr(1));
        else if (kind == FrameKind::FileChunk && plain->size() >= 1 + 28)
            relayed(uuidText(std::string_view(*plain).substr(1, 16)), std::move(*plain)); // a pull of ours over this link
        if (conn->closed)
            break;
    }
    dropped(conn);
}

void Engine::dropped(const std::shared_ptr<Conn>& conn)
{
    conn->close();
    bool was = false;
    {
        const std::lock_guard lock(m_mutex);
        if (const auto it = m_links.find(conn->peerId); it != m_links.end() && it->second == conn) {
            m_links.erase(it);
            was = true;
            if (auto peer = m_peers.find(conn->peerId); peer != m_peers.end()) {
                peer->second.lastSeenMs = nowMs();
                peer->second.nextTryMs = nowMs() + 2'000;
            }
            saveIdentityLocked();
        }
    }
    {
        const std::lock_guard lock(m_mutex);
        for (const auto& [id, relay] : m_relays) {
            if (relay->link == conn) {
                const std::lock_guard relayLock(relay->mutex);
                relay->lost = true;
                relay->cv.notify_all();
            }
        }
    }
    if (was) {
        log("member offline");
        changed();
        m_wake.notify_all();
    }
}

void Engine::closeAllLinks()
{
    for (const auto& conn : links())
        conn->close();
}

// ---- messages ----------------------------------------------------------------------

void Engine::onJson(const std::shared_ptr<Conn>& conn, const std::string& text)
{
    const std::optional<Json> parsed = parseJson(text);
    if (!parsed)
        return;
    const Json& m = *parsed;
    const std::string type = m["type"].toString();
    if (type == "ping") {
        sendJson(conn, Json::Object {{"type", "pong"}, {"ts", m["ts"]}});
    } else if (type == "pong") {
        // lastRecvMs is set by every frame
    } else if (type == "peer_update") {
        if (m["deviceName"].isString()) {
            const std::lock_guard lock(m_mutex);
            conn->peerName = localsend::cutUtf8(localsend::validUtf8(m["deviceName"].toString()), 128);
            notePeerLocked(conn->peerId, conn->peerName, {}, {}, 0);
        }
        learnPeers(m["peers"]);
        changed();
    } else if (type == "file_pull_req") {
        // A member that could not open a file connection to this PC (NAT, a
        // second router) pulls over this one: the file streamed back here.
        const std::string transferId = m["transferId"].toString();
        const long long index = m["fileIndex"].toInt(-1);
        const long long offset = m["offset"].toInt(-1);
        spawn("serve file", [this, conn, transferId, index, offset] {
            const std::lock_guard serving(conn->serving);
            serveFile(transferId, index, offset, [this, &conn](Bytes plaintext) { return sendPaced(conn, std::move(plaintext)); });
        });
    } else if (type == "file_pull_done" || type == "file_pull_err") {
        Bytes plain(1, static_cast<char>(FrameKind::Json));
        plain += text;
        relayed(m["transferId"].toString(), std::move(plain));
    } else if (type == "group_rotate") {
        const auto key = fromBase64(m["newGroupKey"].toString());
        std::vector<std::string> revoked;
        for (const Json& id : m["revokedDeviceIds"].toArray())
            revoked.push_back(id.toString());
        if (key && key->size() == kKeyBytes)
            rotate(*key, revoked);
    } else if (type == "clip_text") {
        const std::string eventId = m["eventId"].toString();
        const std::string origin = m["originDeviceId"].toString();
        const std::string target = m["targetDeviceId"].toString();
        const std::int64_t ts = m["timestampMs"].toInt();
        std::string myId;
        bool paused = false, autoWrite = true;
        {
            const std::lock_guard lock(m_mutex);
            myId = m_deviceId;
            paused = m_options.paused;
            autoWrite = m_options.autoWrite;
            const bool seen = std::ranges::any_of(m_seenEvents, [&](const auto& e) { return e.first == eventId; });
            if (seen || eventId.empty())
                return;
            m_seenEvents.emplace_back(eventId, nowMs());
            if (m_seenEvents.size() > 512)
                m_seenEvents.pop_front();
        }
        if (paused || origin == myId || (!target.empty() && target != myId) || !m["text"].isString())
            return;
        const std::string body = localsend::validUtf8(m["text"].toString());
        Received r;
        r.view.eventId = eventId;
        r.view.kind = ReceivedView::Kind::Text;
        r.view.originName = localsend::cutUtf8(m["originDeviceName"].toString(), 128);
        r.view.timestampMs = ts;
        r.view.preview = previewOf(body);
        r.originId = origin;
        r.text = body;
        // A text sent to this PC alone is put on the clipboard whatever came last.
        if (autoWrite && (!target.empty() || newest(ts, origin))) {
            {
                const std::lock_guard lock(m_mutex);
                m_echoFingerprint = hexLower(sha256(body));
                m_echoUntil = nowMs() + kEchoMs;
            }
            r.view.written = m_sink.putText(body, origin);
        }
        recordReceived(std::move(r));
    } else if (type == "clip_files") {
        const std::string eventId = m["eventId"].toString();
        const std::string origin = m["originDeviceId"].toString();
        const std::string target = m["targetDeviceId"].toString();
        const std::string transferId = m["transferId"].toString();
        const std::int64_t ts = m["timestampMs"].toInt();
        std::string myId;
        bool paused = false, autoWrite = true, shareFiles = true, notice = true;
        std::uint64_t prefetch = 0;
        {
            const std::lock_guard lock(m_mutex);
            myId = m_deviceId;
            paused = m_options.paused;
            autoWrite = m_options.autoWrite;
            shareFiles = m_options.shareFiles;
            prefetch = m_options.prefetchBytes;
            notice = m_options.fileNotice;
            const bool seen = std::ranges::any_of(m_seenEvents, [&](const auto& e) { return e.first == eventId; });
            if (seen || eventId.empty() || m_incoming.count(transferId))
                return;
            m_seenEvents.emplace_back(eventId, nowMs());
            if (m_seenEvents.size() > 512)
                m_seenEvents.pop_front();
        }
        if (paused || !shareFiles || origin == myId || (!target.empty() && target != myId) || !uuidBytes(transferId))
            return;
        Incoming in;
        in.view.transferId = transferId;
        in.view.originName = localsend::cutUtf8(m["originDeviceName"].toString(), 128);
        in.view.timestampMs = ts;
        in.originId = origin;
        in.eventTs = ts;
        std::set<std::uint32_t> indexes;
        for (const Json& e : m["entries"].toArray()) {
            EntryIn entry;
            const auto index = e["index"].toInt(-1);
            entry.relativePath = e["relativePath"].toString();
            entry.directory = e["isDirectory"].toBool();
            entry.size = static_cast<std::uint64_t>(std::max<long long>(0, e["size"].toInt()));
            entry.modifiedMs = e["modifiedMs"].toInt();
            if (index < 0 || index > 0x7FFFFFFF || !validRelativePath(entry.relativePath)
                || !localsend::safeRelativePath(entry.relativePath) || !indexes.insert(static_cast<std::uint32_t>(index)).second)
                continue; // a name that is not one is left out
            entry.index = static_cast<std::uint32_t>(index);
            if (entry.relativePath.find('/') == std::string::npos)
                (entry.directory ? in.view.folders : in.view.files) += 1; // what was copied, not what is in it
            in.view.totalBytes += entry.directory ? 0 : entry.size;
            in.entries.push_back(std::move(entry));
            if (in.entries.size() > kMaxEntries)
                return;
        }
        if (in.entries.empty())
            return;
        const bool latest = !target.empty() || newest(ts, origin);
        const bool fewBytes = prefetch > 0 && in.view.totalBytes <= prefetch;
        const std::uint64_t totalBytes = in.view.totalBytes;
        in.writeWhenDone = autoWrite && latest;
        in.cancel = std::make_shared<std::atomic_bool>(false);
        {
            const std::lock_guard lock(m_mutex);
            m_incoming[transferId] = std::move(in);
            // The oldest lists go: at most 20 kept.
            while (m_incoming.size() > 20) {
                auto oldest = std::ranges::min_element(m_incoming, {}, [](const auto& p) { return p.second.view.timestampMs; });
                if (oldest->second.view.stage == FilesView::Stage::Pulling)
                    break;
                m_incoming.erase(oldest);
            }
        }
        Received r;
        r.view.eventId = eventId;
        r.view.kind = ReceivedView::Kind::Files;
        r.view.originName = localsend::cutUtf8(m["originDeviceName"].toString(), 128);
        r.view.timestampMs = ts;
        r.view.transferId = transferId;
        r.originId = origin;
        recordReceived(std::move(r));
        if (autoWrite && latest && fewBytes) {
            pullFiles(transferId);
        } else if (latest && notice) {
            const std::string who = localsend::cutUtf8(m["originDeviceName"].toString(), 64);
            if (zh())
                toast(who + " 复制了文件（" + localsend::sizeText(totalBytes) + "）", "点这里接收并复制，或存到「下载」");
            else
                toast(who + " copied files (" + localsend::sizeText(totalBytes) + ")", "Click to receive and copy them, or save them to Downloads");
        }
        changed();
    }
    // clip_image never comes as JSON (frame 0x03); unknown types are for newer versions.
}

void Engine::onImage(const std::shared_ptr<Conn>&, std::string_view payload)
{
    if (payload.size() < 4)
        return;
    const std::uint32_t metaLength = be32(payload);
    if (metaLength == 0 || metaLength > payload.size() - 4 || metaLength > 64 * 1024)
        return;
    const std::optional<Json> meta = parseJson(payload.substr(4, metaLength));
    const std::string_view png = payload.substr(4 + metaLength);
    static constexpr std::string_view signature("\x89PNG\r\n\x1a\n", 8);
    if (!meta || png.size() > kMaxImageBytes || png.substr(0, 8) != signature)
        return;
    const Json& m = *meta;
    const std::string eventId = m["eventId"].toString();
    const std::string origin = m["originDeviceId"].toString();
    const std::string target = m["targetDeviceId"].toString();
    const std::int64_t ts = m["timestampMs"].toInt();
    std::string myId;
    bool paused = false, autoWrite = true;
    {
        const std::lock_guard lock(m_mutex);
        myId = m_deviceId;
        paused = m_options.paused;
        autoWrite = m_options.autoWrite;
        const bool seen = std::ranges::any_of(m_seenEvents, [&](const auto& e) { return e.first == eventId; });
        if (seen || eventId.empty())
            return;
        m_seenEvents.emplace_back(eventId, nowMs());
        if (m_seenEvents.size() > 512)
            m_seenEvents.pop_front();
    }
    if (paused || origin == myId || (!target.empty() && target != myId))
        return;
    Received r;
    r.view.eventId = eventId;
    r.view.kind = ReceivedView::Kind::Image;
    r.view.originName = localsend::cutUtf8(m["originDeviceName"].toString(), 128);
    r.view.timestampMs = ts;
    r.view.preview = std::to_string(m["width"].toInt()) + " × " + std::to_string(m["height"].toInt());
    r.originId = origin;
    r.png = Bytes(png);
    if (autoWrite && (!target.empty() || newest(ts, origin))) {
        {
            const std::lock_guard lock(m_mutex);
            m_echoFingerprint = hexLower(sha256(r.png));
            m_echoUntil = nowMs() + kEchoMs;
        }
        r.view.written = m_sink.putImage(r.png, origin);
    }
    recordReceived(std::move(r));
}

bool Engine::newest(std::int64_t ts, const std::string& origin)
{
    const std::lock_guard lock(m_mutex);
    if (ts < m_lastTs || (ts == m_lastTs && origin <= m_lastOrigin))
        return false;
    m_lastTs = ts;
    m_lastOrigin = origin;
    return true;
}

bool Engine::echoOf(const std::string& fingerprint)
{
    const std::lock_guard lock(m_mutex);
    return nowMs() < m_echoUntil && fingerprint == m_echoFingerprint;
}

void Engine::recordReceived(Received received)
{
    {
        const std::lock_guard lock(m_mutex);
        m_received.push_front(std::move(received));
        while (m_received.size() > kReceivedKept)
            m_received.pop_back();
        // Pictures are large: only the newest five keep theirs.
        int pictures = 0;
        for (Received& r : m_received) {
            if (!r.png.empty() && ++pictures > 5)
                r.png.clear();
        }
    }
    changed();
}

std::string Engine::originName(const std::string& id) const
{
    const auto peer = m_peers.find(id);
    return peer != m_peers.end() ? peer->second.name : std::string();
}

bool Engine::rewrite(const std::string& eventId)
{
    std::string text, origin, transferId;
    Bytes png;
    ReceivedView::Kind kind = ReceivedView::Kind::Text;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = std::ranges::find_if(m_received, [&](const Received& r) { return r.view.eventId == eventId; });
        if (it == m_received.end())
            return false;
        kind = it->view.kind;
        text = it->text;
        png = it->png;
        origin = it->originId;
        transferId = it->view.transferId;
        m_echoFingerprint = kind == ReceivedView::Kind::Image ? hexLower(sha256(png)) : hexLower(sha256(text));
        m_echoUntil = nowMs() + kEchoMs;
    }
    if (kind == ReceivedView::Kind::Text)
        return m_sink.putText(text, origin);
    if (kind == ReceivedView::Kind::Image)
        return !png.empty() && m_sink.putImage(png, origin);
    pullFiles(transferId);
    return true;
}

// ---- what is copied here ---------------------------------------------------------------

void Engine::localText(std::string text, const std::string& target)
{
    text = normalizedText(std::move(text));
    if (text.empty() || text.size() > kMaxText)
        return;
    const std::string fingerprint = hexLower(sha256(text));
    if (target.empty() && echoOf(fingerprint))
        return; // our own write coming back
    Json message;
    {
        const std::lock_guard lock(m_mutex);
        if (m_options.paused || !m_running)
            return;
        const std::int64_t now = nowMs();
        if (target.empty() && fingerprint == m_lastSentFingerprint && now - m_lastSentMs < 1500)
            return; // the same copy told twice
        m_lastSentFingerprint = fingerprint;
        m_lastSentMs = now;
        m_lastTs = std::max(now, m_lastTs + 1);
        m_lastOrigin = m_deviceId;
        message = Json::Object {{"type", "clip_text"}, {"eventId", newUuid()}, {"timestampMs", static_cast<double>(m_lastTs)},
            {"originDeviceId", m_deviceId}, {"originDeviceName", m_options.deviceName}, {"originPlatform", m_options.platform},
            {"fingerprint", fingerprint}, {"text", text}};
        message.set("targetDeviceId", target.empty() ? Json(nullptr) : Json(target));
    }
    broadcast(jsonFrame(message), target);
}

void Engine::localImage(Bytes png, int width, int height, const std::string& target)
{
    if (png.empty() || png.size() > kMaxImageBytes)
        return;
    const std::string fingerprint = hexLower(sha256(png));
    if (target.empty() && echoOf(fingerprint))
        return;
    Json meta;
    {
        const std::lock_guard lock(m_mutex);
        if (m_options.paused || !m_running)
            return;
        const std::int64_t now = nowMs();
        if (target.empty() && fingerprint == m_lastSentFingerprint && now - m_lastSentMs < 1500)
            return;
        m_lastSentFingerprint = fingerprint;
        m_lastSentMs = now;
        m_lastTs = std::max(now, m_lastTs + 1);
        m_lastOrigin = m_deviceId;
        meta = Json::Object {{"eventId", newUuid()}, {"timestampMs", static_cast<double>(m_lastTs)}, {"originDeviceId", m_deviceId},
            {"originDeviceName", m_options.deviceName}, {"originPlatform", m_options.platform}, {"fingerprint", fingerprint},
            {"width", width}, {"height", height}, {"byteSize", static_cast<double>(png.size())}};
        meta.set("targetDeviceId", target.empty() ? Json(nullptr) : Json(target));
    }
    const std::string metaText = meta.dump();
    Bytes frame(1, static_cast<char>(FrameKind::Image));
    putBe32(frame, static_cast<std::uint32_t>(metaText.size()));
    frame += metaText;
    frame += png;
    broadcast(frame, target);
}

void Engine::localFiles(std::vector<std::wstring> paths, const std::string& target)
{
    {
        const std::lock_guard lock(m_mutex);
        if (m_options.paused || !m_running || !m_options.shareFiles || paths.empty())
            return;
    }
    if (target.empty() && echoOf(pathsFingerprint(paths)))
        return; // the files just pulled here, put on the clipboard
    spawn("files", [this, paths = std::move(paths), target]() mutable { filesManifest(std::move(paths), target); });
}

void Engine::filesManifest(std::vector<std::wstring> paths, std::string target)
{
    Outgoing out;
    out.createdMs = nowMs();
    Json::Array entries;
    std::uint64_t total = 0;
    std::string fingerprintInput;
    bool tooMany = false;
    const auto add = [&](const std::wstring& path, const std::string& relative, bool directory, std::uint64_t size,
                         std::int64_t modified) {
        const auto index = static_cast<std::uint32_t>(out.files.size());
        out.files.push_back({directory ? std::wstring() : path, size});
        entries.push_back(Json::Object {{"index", static_cast<double>(index)}, {"relativePath", relative},
            {"isDirectory", directory}, {"size", static_cast<double>(size)}, {"modifiedMs", static_cast<double>(modified)}});
        total += size;
        fingerprintInput += relative + '\0' + std::to_string(size) + '\0';
        tooMany = out.files.size() > kMaxEntries;
    };
    std::function<void(const std::wstring&, const std::string&)> walk = [&](const std::wstring& folder, const std::string& relative) {
        WIN32_FIND_DATAW data;
        const HANDLE find = ::FindFirstFileExW(longPath(joinPath(folder, L"*")).c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE)
            return;
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L".." || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue; // links not followed: no loops, nothing from elsewhere
            const std::wstring full = joinPath(folder, name);
            const std::string rel = relative + "/" + localsend::narrow(name);
            const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                add(full, rel, true, 0, unixMsOf(data.ftLastWriteTime));
                walk(full, rel);
            } else {
                add(full, rel, false, size, unixMsOf(data.ftLastWriteTime));
            }
        } while (!tooMany && !m_stopping && ::FindNextFileW(find, &data));
        ::FindClose(find);
    };
    for (const std::wstring& path : paths) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!::GetFileAttributesExW(longPath(path).c_str(), GetFileExInfoStandard, &data))
            continue;
        std::wstring name = path;
        while (!name.empty() && name.back() == L'\\')
            name.pop_back();
        name = name.substr(name.find_last_of(L'\\') + 1);
        if (name.empty() || name.find(L':') != std::wstring::npos)
            continue; // a whole drive is not shared
        const std::string rel = localsend::narrow(name);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            add(path, rel, true, 0, unixMsOf(data.ftLastWriteTime));
            walk(path, rel);
        } else {
            add(path, rel, false, (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                unixMsOf(data.ftLastWriteTime));
        }
        if (tooMany)
            break;
    }
    if (tooMany || out.files.empty()) {
        if (tooMany)
            log("files: more than 20000 in one copy, not shared");
        return;
    }
    const std::string transferId = newUuid();
    Json message;
    {
        const std::lock_guard lock(m_mutex);
        if (m_options.paused || !m_running)
            return;
        m_outgoing[transferId] = std::move(out);
        while (m_outgoing.size() > 20)
            m_outgoing.erase(std::ranges::min_element(m_outgoing, {}, [](const auto& p) { return p.second.createdMs; }));
        m_lastTs = std::max(nowMs(), m_lastTs + 1);
        m_lastOrigin = m_deviceId;
        message = Json::Object {{"type", "clip_files"}, {"eventId", newUuid()}, {"timestampMs", static_cast<double>(m_lastTs)},
            {"originDeviceId", m_deviceId}, {"originDeviceName", m_options.deviceName}, {"originPlatform", m_options.platform},
            {"fingerprint", hexLower(sha256(fingerprintInput))}, {"transferId", transferId},
            {"totalBytes", static_cast<double>(total)}, {"entries", Json(std::move(entries))}};
        message.set("targetDeviceId", target.empty() ? Json(nullptr) : Json(target));
    }
    broadcast(jsonFrame(message), target);
}

// Another member pulls files copied here over a file connection: one
// request after another, until "bye".
void Engine::serveFiles(const std::shared_ptr<Conn>& conn)
{
    while (const std::optional<Bytes> plain = conn->readSealed(kMaxHandshakeFrame)) {
        if (static_cast<FrameKind>((*plain)[0]) != FrameKind::Json)
            continue;
        const std::optional<Json> request = parseJson(std::string_view(*plain).substr(1));
        if (!request)
            continue;
        const std::string type = (*request)["type"].toString();
        if (type == "bye")
            return;
        if (type != "file_pull_req")
            continue;
        const auto emit = [&conn](Bytes plaintext) { return conn->writeSealed(plaintext); };
        if (!serveFile((*request)["transferId"].toString(), (*request)["fileIndex"].toInt(-1), (*request)["offset"].toInt(-1), emit))
            return;
    }
}

// The file's bytes from `offset` in 0x02 chunks of 256 KB, then their
// count and SHA-256 (file_pull_done), or why not (file_pull_err).
bool Engine::serveFile(const std::string& transferId, long long index, long long offset, const std::function<bool(Bytes)>& emit)
{
    std::wstring path;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_outgoing.find(localsend::lower(transferId)); // ours are lower case; a Mac may write them in capitals
        if (it != m_outgoing.end() && index >= 0 && static_cast<std::size_t>(index) < it->second.files.size())
            path = it->second.files[static_cast<std::size_t>(index)].path;
    }
    const auto fail = [&](const char* code, const char* message) {
        return emit(jsonFrame(Json::Object {{"type", "file_pull_err"}, {"transferId", transferId}, {"fileIndex", static_cast<double>(index)},
            {"code", code}, {"message", message}}));
    };
    const auto tid = uuidBytes(transferId);
    localsend::Handle file;
    if (!path.empty() && tid)
        file = localsend::Handle(::CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid())
        return fail("not_found", "no such file offered here");
    LARGE_INTEGER size {};
    ::GetFileSizeEx(file.get(), &size);
    if (offset < 0 || offset > size.QuadPart)
        return fail("invalid_offset", "the offset is past the end");
    LARGE_INTEGER at {};
    at.QuadPart = offset;
    ::SetFilePointerEx(file.get(), at, nullptr, FILE_BEGIN);
    Sha256Stream hash;
    std::uint64_t sent = 0;
    std::string buffer(kChunkBytes, '\0');
    for (;;) {
        if (m_stopping)
            return false;
        DWORD read = 0;
        if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            return fail("read_error", "the file could not be read");
        if (read == 0)
            break;
        Bytes frame(1, static_cast<char>(FrameKind::FileChunk));
        frame += *tid;
        putBe32(frame, static_cast<std::uint32_t>(index));
        putBe64(frame, static_cast<std::uint64_t>(offset) + sent);
        frame.append(buffer.data(), read);
        hash.add(std::string_view(buffer.data(), read));
        if (!emit(std::move(frame)))
            return false;
        sent += read;
    }
    return emit(jsonFrame(Json::Object {{"type", "file_pull_done"}, {"transferId", transferId}, {"fileIndex", static_cast<double>(index)},
        {"totalSent", static_cast<double>(sent)}, {"sha256", hexLower(hash.finish())}}));
}

bool Engine::sendPaced(const std::shared_ptr<Conn>& conn, Bytes plaintext)
{
    {
        std::unique_lock lock(conn->queueMutex);
        conn->queueCv.wait(lock, [&] { return conn->closed.load() || conn->queued < kPacedBytes; });
        if (conn->closed)
            return false;
        conn->queued += plaintext.size();
        conn->queue.push_back(std::move(plaintext));
    }
    conn->queueCv.notify_all();
    return true;
}

void Engine::relayed(const std::string& transferId, Bytes plaintext)
{
    std::shared_ptr<Relay> relay;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_relays.find(localsend::lower(transferId)); // a chunk gives it in lower case, a Mac's list may be in capitals
        if (it == m_relays.end())
            return; // a pull that ended (canceled): what is still coming is dropped
        relay = it->second;
    }
    const std::lock_guard lock(relay->mutex);
    if (relay->bytes + plaintext.size() > kRelayBytes) {
        relay->lost = true; // the disk here is far slower than the network
    } else {
        relay->bytes += plaintext.size();
        relay->frames.push_back(std::move(plaintext));
    }
    relay->cv.notify_all();
}

void Engine::pullFiles(const std::string& transferId, bool toClipboard)
{
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_incoming.find(transferId);
        if (it == m_incoming.end() || it->second.view.stage == FilesView::Stage::Pulling)
            return;
        it->second.writeWhenDone = toClipboard; // asked for (or small and the newest): onto the clipboard when here
        if (it->second.view.stage != FilesView::Stage::Ready) {
            it->second.view.stage = FilesView::Stage::Pulling;
            it->second.view.receivedBytes = 0;
            it->second.view.problem.clear();
            if (it->second.cancel)
                it->second.cancel->store(false);
        }
    }
    changed();
    spawn("pull", [this, transferId] { pull(transferId); });
}

void Engine::cancelPull(const std::string& transferId)
{
    std::shared_ptr<Relay> relay;
    {
        const std::lock_guard lock(m_mutex);
        if (const auto it = m_incoming.find(transferId); it != m_incoming.end() && it->second.cancel)
            it->second.cancel->store(true);
        if (const auto it = m_relays.find(localsend::lower(transferId)); it != m_relays.end())
            relay = it->second;
    }
    if (relay) {
        const std::lock_guard lock(relay->mutex);
        relay->cv.notify_all();
    }
}

// The files of a clip_files from the device they were copied on, into
// <data>\SharedFiles\<transferId>: over a file connection of their own, or,
// when one cannot be made (the other device behind NAT or a second router,
// it reached us but we cannot reach it), over its sync connection.
void Engine::pull(const std::string& transferId)
{
    std::string originId, host;
    std::uint16_t port = 0;
    std::vector<EntryIn> entries;
    std::shared_ptr<std::atomic_bool> cancel;
    bool alreadyReady = false;
    bool direct = true;
    std::wstring folder;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_incoming.find(transferId);
        if (it == m_incoming.end())
            return;
        alreadyReady = it->second.view.stage == FilesView::Stage::Ready;
        originId = it->second.originId;
        entries = it->second.entries;
        cancel = it->second.cancel;
        direct = m_options.directFiles;
        if (const auto peer = m_peers.find(originId); peer != m_peers.end()) {
            host = peer->second.host;
            port = peer->second.port;
        }
        if (const auto link = m_links.find(originId); link != m_links.end() && (host.empty() || !port)) {
            host = link->second->host;
            port = link->second->peerPort;
        }
        folder = joinPath(joinPath(m_options.dataDir, L"SharedFiles"), localsend::wide(transferId));
    }
    std::vector<std::wstring> tops;
    std::set<std::wstring> topSet;
    for (const EntryIn& e : entries) {
        if (const auto rel = localsend::safeRelativePath(e.relativePath)) {
            const std::wstring top = joinPath(folder, rel->substr(0, rel->find(L'\\')));
            if (topSet.insert(top).second)
                tops.push_back(top);
        }
    }
    const auto finishWith = [&](FilesView::Stage stage, const std::string& problem) {
        bool write = false;
        {
            const std::lock_guard lock(m_mutex);
            if (const auto it = m_incoming.find(transferId); it != m_incoming.end()) {
                it->second.view.stage = stage;
                it->second.view.problem = problem;
                write = stage == FilesView::Stage::Ready && it->second.writeWhenDone;
                it->second.writeWhenDone = false;
            }
        }
        if (write) {
            {
                const std::lock_guard lock(m_mutex);
                m_echoFingerprint = pathsFingerprint(tops);
                m_echoUntil = nowMs() + kEchoMs;
            }
            if (m_sink.putFiles(tops, originId)) {
                const std::lock_guard lock(m_mutex);
                for (Received& r : m_received) {
                    if (r.view.transferId == transferId)
                        r.view.written = true;
                }
            }
        }
        changed();
    };
    if (alreadyReady) {
        finishWith(FilesView::Stage::Ready, {});
        return;
    }

    // A file connection first.
    std::shared_ptr<Conn> conn;
    if (direct && !host.empty() && port) {
        if (const SOCKET s = connectTo(host, port, 5000); s != INVALID_SOCKET) {
            conn = std::make_shared<Conn>();
            conn->socket = s;
            conn->outbound = true;
            conn->host = host;
            tuned(s);
            {
                const std::lock_guard lock(m_mutex);
                m_conns.insert(conn);
            }
            std::string problem;
            if (clientHandshake(*conn, "file", &problem) && conn->peerId == originId) {
                setTimeout(s, SO_RCVTIMEO, static_cast<DWORD>(kStalledMs));
            } else {
                log("file connection: " + (problem.empty() ? std::string("another device") : problem) + "; over the sync link");
                conn->finish();
                const std::lock_guard lock(m_mutex);
                m_conns.erase(conn);
                conn.reset();
            }
        } else {
            log("file connection: unreachable; over the sync link");
        }
    }
    // Else the sync connection the other device keeps with this PC.
    std::shared_ptr<Relay> relay;
    if (!conn) {
        const std::lock_guard lock(m_mutex);
        const auto link = m_links.find(originId);
        if (link != m_links.end() && !link->second->closed) {
            relay = std::make_shared<Relay>();
            relay->link = link->second;
            m_relays[localsend::lower(transferId)] = relay;
        }
    }
    if (!conn && !relay) {
        finishWith(FilesView::Stage::Failed, "unreachable");
        return;
    }
    const auto request = [&](const Json& message) {
        if (conn)
            return conn->writeSealed(jsonFrame(message));
        if (relay->link->closed)
            return false;
        send(relay->link, jsonFrame(message));
        return true;
    };
    const auto next = [&]() -> std::optional<Bytes> {
        if (conn)
            return conn->readSealed();
        std::unique_lock lock(relay->mutex);
        const std::int64_t until = nowMs() + kStalledMs;
        while (relay->frames.empty() && !relay->lost && !(cancel && cancel->load()) && !m_stopping && nowMs() < until)
            relay->cv.wait_for(lock, std::chrono::milliseconds(250));
        if (relay->frames.empty())
            return std::nullopt;
        Bytes frame = std::move(relay->frames.front());
        relay->frames.pop_front();
        relay->bytes -= frame.size();
        return frame;
    };

    bool ok = true;
    std::string problem;
    std::uint64_t received = 0;
    std::int64_t lastShown = 0;
    std::string tid;
    if (const auto bytes = uuidBytes(transferId))
        tid = *bytes;
    for (std::size_t i = 0; ok && i < entries.size(); ++i) {
        const EntryIn& e = entries[i];
        const auto rel = localsend::safeRelativePath(e.relativePath);
        if (!rel)
            continue;
        const std::wstring full = joinPath(folder, *rel);
        std::error_code error;
        if (e.directory) {
            std::filesystem::create_directories(std::filesystem::path(longPath(full)), error);
            continue;
        }
        std::filesystem::create_directories(std::filesystem::path(longPath(full)).parent_path(), error);
        const std::wstring part = full + L".shunsync-part";
        localsend::Handle out(::CreateFileW(longPath(part).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!out.valid()) {
            ok = false;
            problem = "disk";
            break;
        }
        if (!request(Json::Object {{"type", "file_pull_req"}, {"transferId", transferId}, {"fileIndex", static_cast<double>(e.index)},
                {"offset", 0}})) {
            ok = false;
            problem = "lost";
            break;
        }
        Sha256Stream hash;
        std::uint64_t got = 0;
        bool done = false;
        while (!done) {
            if (cancel && cancel->load()) {
                ok = false;
                problem = "canceled";
                break;
            }
            const std::optional<Bytes> plain = next();
            if (!plain || plain->empty()) {
                ok = false;
                problem = cancel && cancel->load() ? "canceled" : "lost";
                break;
            }
            const auto kind = static_cast<FrameKind>((*plain)[0]);
            const std::string_view body = std::string_view(*plain).substr(1);
            if (kind == FrameKind::FileChunk) {
                if (body.size() < 28 || body.substr(0, 16) != tid || be32(body.substr(16)) != e.index || be64(body.substr(20)) != got) {
                    ok = false;
                    problem = "sender";
                    break;
                }
                const std::string_view data = body.substr(28);
                DWORD written = 0;
                if (!::WriteFile(out.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr) || written != data.size()) {
                    ok = false;
                    problem = "disk";
                    break;
                }
                hash.add(data);
                got += data.size();
                received += data.size();
                if (const std::int64_t now = nowMs(); now - lastShown > 300) {
                    lastShown = now;
                    {
                        const std::lock_guard lock(m_mutex);
                        if (const auto it = m_incoming.find(transferId); it != m_incoming.end())
                            it->second.view.receivedBytes = received;
                    }
                    changed();
                }
            } else if (kind == FrameKind::Json) {
                const std::optional<Json> m = parseJson(body);
                const std::string type = m ? (*m)["type"].toString() : std::string();
                if ((type == "file_pull_done" || type == "file_pull_err") && (*m)["fileIndex"].toInt(-1) != e.index)
                    continue; // an earlier file's, late
                if (type == "file_pull_done") {
                    if (static_cast<std::uint64_t>((*m)["totalSent"].toInt(-1)) != got
                        || !localsend::equalsNoCase(hexLower(hash.finish()), (*m)["sha256"].toString())) {
                        ok = false;
                        problem = "checksum";
                    }
                    done = true;
                } else if (type == "file_pull_err") {
                    ok = false;
                    problem = (*m)["code"].toString();
                    done = true;
                }
            }
        }
        if (ok) {
            if (e.modifiedMs > 0) {
                const FILETIME modified = fileTimeOf(e.modifiedMs);
                ::SetFileTime(out.get(), nullptr, nullptr, &modified);
            }
            out.reset();
            if (!::MoveFileExW(longPath(part).c_str(), longPath(full).c_str(), MOVEFILE_REPLACE_EXISTING)) {
                ok = false;
                problem = "disk";
            }
        }
        if (!ok) {
            out.reset();
            ::DeleteFileW(longPath(part).c_str());
        }
    }
    if (conn) {
        if (ok)
            conn->writeSealed(jsonFrame(Json::Object {{"type", "bye"}}));
        conn->finish();
    }
    {
        const std::lock_guard lock(m_mutex);
        if (conn)
            m_conns.erase(conn);
        if (relay)
            m_relays.erase(localsend::lower(transferId));
        if (const auto it = m_incoming.find(transferId); it != m_incoming.end())
            it->second.view.receivedBytes = received;
    }
    if (relay && !ok && problem != "canceled" && relay->link)
        log("pull over the sync link: " + problem);
    if (!ok) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(longPath(folder)), error);
        finishWith(FilesView::Stage::Failed, problem);
        return;
    }
    finishWith(FilesView::Stage::Ready, {});
    sweepCache();
}

// <data>\SharedFiles: what came more than 7 days ago goes, then the oldest
// until all of it is under 2 GB.
void Engine::sweepCache()
{
    std::wstring root;
    std::set<std::wstring> busy;
    {
        const std::lock_guard lock(m_mutex);
        root = joinPath(m_options.dataDir, L"SharedFiles");
        for (const auto& [id, in] : m_incoming) {
            if (in.view.stage == FilesView::Stage::Pulling)
                busy.insert(localsend::wide(id));
        }
    }
    std::error_code error;
    namespace fs = std::filesystem;
    if (!fs::is_directory(root, error))
        return;
    struct Folder {
        fs::path path;
        std::int64_t modifiedMs = 0;
        std::uint64_t bytes = 0;
    };
    std::vector<Folder> folders;
    for (const auto& entry : fs::directory_iterator(root, error)) {
        if (!entry.is_directory(error) || busy.count(entry.path().filename().wstring()))
            continue;
        Folder f;
        f.path = entry.path();
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (::GetFileAttributesExW(f.path.c_str(), GetFileExInfoStandard, &data))
            f.modifiedMs = unixMsOf(data.ftCreationTime);
        for (const auto& file : fs::recursive_directory_iterator(longPath(f.path.wstring()), error)) {
            if (file.is_regular_file(error))
                f.bytes += file.file_size(error);
        }
        folders.push_back(std::move(f));
    }
    std::ranges::sort(folders, {}, &Folder::modifiedMs);
    std::uint64_t total = 0;
    for (const Folder& f : folders)
        total += f.bytes;
    const std::int64_t now = nowMs();
    for (const Folder& f : folders) {
        if (now - f.modifiedMs < kCacheMs && total <= kCacheBytes)
            break;
        fs::remove_all(fs::path(longPath(f.path.wstring())), error);
        total -= std::min(total, f.bytes);
    }
}

// ---- group changes --------------------------------------------------------------------

void Engine::rotate(const Bytes& key, const std::vector<std::string>& revoked)
{
    bool removed = false;
    {
        const std::lock_guard lock(m_mutex);
        removed = std::ranges::find(revoked, m_deviceId) != revoked.end();
        if (removed) {
            newGroupLocked(randomBytes(kKeyBytes), false); // this PC was taken out: on its own again
        } else {
            m_groupKey = key;
            m_groupId = groupIdOf(key);
            for (const std::string& id : revoked) {
                if (std::ranges::find(m_revoked, id) == m_revoked.end())
                    m_revoked.push_back(id);
                m_peers.erase(id);
            }
            for (auto& [id, peer] : m_peers)
                peer.nextTryMs = nowMs() + 1'500; // the others switch too
            saveIdentityLocked();
        }
    }
    log(removed ? "this PC was removed from its group" : "the group has a new key");
    closeAllLinks();
    changed();
}

void Engine::removeDevice(const std::string& deviceId)
{
    const Bytes key = randomBytes(kKeyBytes);
    Json message;
    std::vector<std::shared_ptr<Conn>> others;
    {
        const std::lock_guard lock(m_mutex);
        if (std::ranges::find(m_revoked, deviceId) == m_revoked.end())
            m_revoked.push_back(deviceId);
        Json::Array revoked;
        for (const std::string& id : m_revoked)
            revoked.push_back(id);
        message = Json::Object {{"type", "group_rotate"}, {"newGroupKey", base64(key)}, {"revokedDeviceIds", Json(std::move(revoked))}};
        for (const auto& [id, conn] : m_links) {
            if (id != deviceId)
                others.push_back(conn);
        }
    }
    for (const auto& conn : others) {
        sendJson(conn, message);
        const std::lock_guard lock(conn->queueMutex);
        conn->closeWhenFlushed = true;
        conn->queueCv.notify_all();
    }
    {
        const std::lock_guard lock(m_mutex);
        m_groupKey = key;
        m_groupId = groupIdOf(key);
        m_peers.erase(deviceId);
        for (auto& [id, peer] : m_peers)
            peer.nextTryMs = nowMs() + 1'500;
        saveIdentityLocked();
        if (const auto link = m_links.find(deviceId); link != m_links.end())
            link->second->close();
    }
    changed();
}

std::string Engine::invite() const
{
    const std::lock_guard lock(m_mutex);
    std::string host;
    if (m_options.bindAddress != "0.0.0.0") {
        host = m_options.bindAddress;
    } else {
        for (const localsend::Interface& i : localsend::currentInterfaces()) {
            if (localsend::usable(i) && i.category != localsend::Interface::Category::Public) {
                host = i.ip;
                break;
            }
        }
    }
    return inviteCode(m_groupKey, host.empty() ? std::string() : host + ":" + std::to_string(m_port.load()));
}

bool Engine::joinWithInvite(const std::string& text)
{
    const std::optional<Invite> parsed = parseInvite(text);
    if (!parsed)
        return false;
    {
        const std::lock_guard lock(m_mutex);
        if (parsed->groupKey == m_groupKey)
            return true;
        newGroupLocked(parsed->groupKey, false);
        if (!parsed->hostPort.empty())
            m_dialing.emplace(parsed->hostPort, 0); // dialed at the next tick
    }
    closeAllLinks();
    m_wake.notify_all();
    changed();
    return true;
}

// ---- pairing -----------------------------------------------------------------------------

void Engine::openPairing(std::chrono::seconds length)
{
    {
        const std::lock_guard lock(m_mutex);
        m_pairingUntil = nowMs() + std::chrono::duration_cast<std::chrono::milliseconds>(length).count();
    }
    m_wake.notify_all();
    changed();
}

void Engine::closePairing()
{
    {
        const std::lock_guard lock(m_mutex);
        m_pairingUntil = 0;
    }
    m_wake.notify_all();
    changed();
}

void Engine::serverPair(const std::shared_ptr<Conn>& conn, const std::string& peerId, const std::string& name,
    const std::string& platform, const std::string& sas, std::uint16_t listenPort)
{
    {
        const std::lock_guard lock(m_mutex);
        Pair pair;
        pair.view.id = m_nextPair++;
        pair.view.incoming = true;
        pair.view.name = name;
        pair.view.platform = platform;
        pair.view.sas = sas;
        pair.view.stage = PairView::Stage::Confirm;
        pair.conn = conn;
        pair.peerId = peerId;
        pair.host = conn->host;
        pair.peerPort = listenPort;
        pair.updatedMs = nowMs();
        m_pairs[pair.view.id] = std::move(pair);
    }
    const std::string shownSas = sas.size() == 6 ? sas.substr(0, 3) + " " + sas.substr(3) : sas;
    if (zh())
        toast("「" + (name.empty() ? std::string("新设备") : name) + "」想加入剪贴板同步", "确认码 " + shownSas + "，打开「同步」核对后同意");
    else
        toast((name.empty() ? std::string("A new device") : name) + " asks to join clipboard sync",
            "Code " + shownSas + ": open sync, check it, then agree");
    changed();
}

void Engine::answerPair(std::uint64_t id, bool accept)
{
    std::shared_ptr<Conn> conn;
    Json message;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_pairs.find(id);
        if (it == m_pairs.end() || !it->second.view.incoming || it->second.view.stage != PairView::Stage::Confirm || !it->second.conn)
            return;
        Pair& pair = it->second;
        conn = pair.conn;
        if (accept) {
            Json peers = peersLocked(pair.peerId);
            message = Json::Object {{"type", "pair_grant"}, {"groupKey", base64(m_groupKey)}, {"peers", peers}};
            notePeerLocked(pair.peerId, pair.view.name, pair.view.platform, pair.host, pair.peerPort);
            if (auto peer = m_peers.find(pair.peerId); peer != m_peers.end())
                peer->second.nextTryMs = nowMs() + 1'500; // it dials us first; we try it too after a moment
            saveIdentityLocked();
            pair.view.stage = PairView::Stage::Done;
            m_pairingUntil = 0; // one device let in: the window closes
        } else {
            message = Json::Object {{"type", "pair_reject"}, {"reason", "declined"}};
            pair.view.stage = PairView::Stage::Failed;
            pair.view.problem = "declined_here";
        }
        pair.updatedMs = nowMs();
    }
    sendJson(conn, message);
    {
        const std::lock_guard lock(conn->queueMutex);
        conn->closeWhenFlushed = true;
    }
    conn->queueCv.notify_all();
    m_wake.notify_all();
    changed();
}

std::uint64_t Engine::pairWith(const std::string& deviceIdOrAddress)
{
    std::string host, name;
    std::uint16_t port = 0;
    std::uint64_t id = 0;
    {
        const std::lock_guard lock(m_mutex);
        if (const auto nearby = m_nearby.find(deviceIdOrAddress); nearby != m_nearby.end()) {
            host = nearby->second.view.host;
            port = nearby->second.view.port;
            name = nearby->second.view.name;
        } else if (const auto hp = parseHostPort(deviceIdOrAddress, kTcpPort)) {
            host = hp->host;
            port = hp->port;
            name = deviceIdOrAddress;
        } else {
            return 0;
        }
        Pair pair;
        pair.view.id = id = m_nextPair++;
        pair.view.incoming = false;
        pair.view.name = name;
        pair.view.stage = PairView::Stage::Connecting;
        pair.updatedMs = nowMs();
        m_pairs[id] = std::move(pair);
    }
    changed();
    spawn("pair", [this, id, host, port] { pairClient(id, host, port); });
    return id;
}

void Engine::pairClient(std::uint64_t id, std::string host, std::uint16_t port)
{
    const auto fail = [&](const std::string& problem) {
        {
            const std::lock_guard lock(m_mutex);
            if (const auto it = m_pairs.find(id); it != m_pairs.end() && it->second.view.stage != PairView::Stage::Done) {
                it->second.view.stage = PairView::Stage::Failed;
                if (it->second.view.problem.empty())
                    it->second.view.problem = problem;
                it->second.updatedMs = nowMs();
                it->second.conn.reset();
            }
        }
        changed();
    };
    const SOCKET s = connectTo(host, port, 5000);
    if (s == INVALID_SOCKET) {
        fail("unreachable");
        return;
    }
    auto conn = std::make_shared<Conn>();
    conn->socket = s;
    conn->outbound = true;
    conn->host = host;
    tuned(s);
    {
        const std::lock_guard lock(m_mutex);
        m_conns.insert(conn);
    }
    std::string sasOrProblem;
    if (!clientHandshake(*conn, "pair", &sasOrProblem)) {
        conn->finish();
        {
            const std::lock_guard lock(m_mutex);
            m_conns.erase(conn);
        }
        fail(sasOrProblem);
        return;
    }
    setTimeout(s, SO_RCVTIMEO, 0);
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_pairs.find(id);
        if (it == m_pairs.end() || it->second.view.stage == PairView::Stage::Failed) {
            conn->writeSealed(jsonFrame(Json::Object {{"type", "pair_cancel"}}));
            conn->close();
        } else {
            it->second.view.sas = sasOrProblem;
            it->second.view.stage = PairView::Stage::Confirm;
            it->second.view.name = conn->peerName.empty() ? it->second.view.name : conn->peerName;
            it->second.view.platform = conn->peerPlatform;
            it->second.peerId = conn->peerId;
            it->second.conn = conn;
            it->second.updatedMs = nowMs();
        }
    }
    changed();
    std::string problem = "lost";
    while (const std::optional<Bytes> plain = conn->readSealed(kMaxHandshakeFrame)) {
        if (static_cast<FrameKind>((*plain)[0]) != FrameKind::Json)
            continue;
        const std::optional<Json> m = parseJson(std::string_view(*plain).substr(1));
        if (!m)
            continue;
        const std::string type = (*m)["type"].toString();
        if (type == "pair_grant") {
            const auto key = fromBase64((*m)["groupKey"].toString());
            if (!key || key->size() != kKeyBytes)
                break;
            {
                const std::lock_guard lock(m_mutex);
                newGroupLocked(*key, false);
                notePeerLocked(conn->peerId, conn->peerName, conn->peerPlatform, host, conn->peerPort ? conn->peerPort : port);
                if (auto peer = m_peers.find(conn->peerId); peer != m_peers.end())
                    peer->second.nextTryMs = 0;
                saveIdentityLocked();
                if (const auto it = m_pairs.find(id); it != m_pairs.end()) {
                    it->second.view.stage = PairView::Stage::Done;
                    it->second.updatedMs = nowMs();
                    it->second.conn.reset();
                }
            }
            learnPeers((*m)["peers"]);
            closeAllLinks(); // the old group's
            if (zh())
                toast("已加入「" + conn->peerName + "」的剪贴板同步", "在这里复制的内容会出现在对方的剪贴板，反过来也一样");
            else
                toast("Joined " + conn->peerName + "'s clipboard sync", "What you copy here appears on its clipboard, and the other way round");
            problem.clear();
            break;
        }
        if (type == "pair_reject") {
            problem = "declined";
            break;
        }
    }
    conn->finish();
    {
        const std::lock_guard lock(m_mutex);
        m_conns.erase(conn);
    }
    if (!problem.empty())
        fail(problem);
    m_wake.notify_all();
    changed();
}

void Engine::cancelPair(std::uint64_t id)
{
    std::shared_ptr<Conn> conn;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_pairs.find(id);
        if (it == m_pairs.end())
            return;
        conn = it->second.conn;
        const bool open = it->second.view.stage == PairView::Stage::Connecting || it->second.view.stage == PairView::Stage::Confirm;
        if (open) {
            it->second.view.stage = PairView::Stage::Failed;
            it->second.view.problem = "canceled_here";
            it->second.updatedMs = nowMs();
        } else {
            m_pairs.erase(it);
        }
    }
    if (conn) {
        if (!conn->writer.joinable())
            conn->writeSealed(jsonFrame(Json::Object {{"type", "pair_cancel"}}));
        else
            sendJson(conn, Json::Object {{"type", "pair_reject"}, {"reason", "canceled"}});
        conn->close();
    }
    changed();
}

// ---- discovery and the clock ---------------------------------------------------------------

void Engine::beaconLoop()
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the network list (NLM)
    SOCKET udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET) {
        if (SUCCEEDED(com))
            ::CoUninitialize();
        return;
    }
    BOOL on = TRUE;
    ::setsockopt(udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = ::htons(kUdpPort);
    if (::bind(udp, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        address.sin_port = 0; // another program has it: beacons still go out, none come in
        ::bind(udp, reinterpret_cast<const sockaddr*>(&address), sizeof address);
        log("UDP 37220 is taken: devices are found only by their own beacons");
    }
    std::int64_t lastSent = 0;
    bool lastOpen = false;
    std::string lastName;
    while (!m_stopping) {
        const std::int64_t now = nowMs();
        bool open = false;
        std::string name;
        {
            const std::lock_guard lock(m_mutex);
            open = m_options.acceptPairing || m_pairingUntil > now;
            name = m_options.deviceName;
        }
        if (now - lastSent >= kBeaconMs || open != lastOpen || name != lastName) {
            sendBeacons(udp);
            lastSent = now;
            lastOpen = open;
            lastName = name;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(udp, &readable);
        timeval wait {0, 500'000};
        if (::select(0, &readable, nullptr, nullptr, &wait) != 1)
            continue;
        char buffer[4096];
        sockaddr_in from {};
        int length = sizeof from;
        const int n = ::recvfrom(udp, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &length);
        if (n <= 0)
            continue;
        const std::optional<Json> beacon = parseJson(std::string_view(buffer, static_cast<std::size_t>(n)));
        if (!beacon || (*beacon)["proto"].toInt() != kProtocol)
            continue;
        const std::string id = (*beacon)["deviceId"].toString();
        const auto port = (*beacon)["tcpPort"].toInt();
        if (!uuidBytes(id) || port <= 0 || port > 65535)
            continue;
        char ip[INET_ADDRSTRLEN] {};
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        bool member = false;
        bool fresh = false;
        {
            const std::lock_guard lock(m_mutex);
            if (id == m_deviceId || std::ranges::find(m_revoked, id) != m_revoked.end())
                continue;
            const std::string groupId = (*beacon)["groupId"].toString();
            Nearby& seen = m_nearby[id];
            fresh = seen.seenMs == 0 || seen.view.pairingOpen != (*beacon)["pairingOpen"].toBool();
            seen.view.deviceId = id;
            seen.view.name = localsend::cutUtf8(localsend::validUtf8((*beacon)["deviceName"].toString()), 128);
            seen.view.platform = localsend::cutUtf8((*beacon)["platform"].toString(), 32);
            seen.view.host = ip;
            seen.view.port = static_cast<std::uint16_t>(port);
            seen.view.pairingOpen = (*beacon)["pairingOpen"].toBool();
            seen.groupId = groupId;
            seen.seenMs = nowMs();
            if (groupId == m_groupId) {
                member = true;
                const bool linked = m_links.count(id) > 0;
                notePeerLocked(id, seen.view.name, seen.view.platform, ip, static_cast<std::uint16_t>(port));
                if (!linked) {
                    Peer& peer = m_peers[id];
                    if (!peer.connecting && peer.nextTryMs > nowMs() + 1'000)
                        peer.nextTryMs = 0; // it is right there: now
                }
            }
        }
        if (member)
            m_wake.notify_all();
        if (fresh)
            changed();
    }
    ::closesocket(udp);
    if (SUCCEEDED(com))
        ::CoUninitialize();
}

void Engine::sendBeacons(SOCKET udp)
{
    std::string text;
    bool allNetworks = false;
    {
        const std::lock_guard lock(m_mutex);
        if (m_options.paused)
            return; // paused: not seen either
        const Json beacon = Json::Object {{"proto", kProtocol}, {"deviceId", m_deviceId}, {"deviceName", m_options.deviceName},
            {"platform", m_options.platform}, {"groupId", m_groupId}, {"tcpPort", static_cast<int>(m_port.load())},
            {"pairingOpen", m_options.acceptPairing || m_pairingUntil > nowMs()}};
        text = beacon.dump();
        allNetworks = m_options.allNetworks;
    }
    static std::vector<localsend::Interface> interfaces;
    static std::int64_t listed = 0;
    if (nowMs() - listed > 30'000) {
        interfaces = localsend::currentInterfaces();
        listed = nowMs();
    }
    for (const localsend::Interface& i : interfaces) {
        if (!localsend::usable(i)
            || !localsend::receiveAllowed(i, allNetworks ? localsend::ReceiveNetworks::All : localsend::ReceiveNetworks::Trusted, {}))
            continue;
        const std::uint32_t mask = i.prefix <= 0 ? 0 : (i.prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - i.prefix)) - 1));
        sockaddr_in to {};
        to.sin_family = AF_INET;
        to.sin_port = ::htons(kUdpPort);
        to.sin_addr.s_addr = ::htonl(i.address | ~mask);
        ::sendto(udp, text.data(), static_cast<int>(text.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
}

void Engine::tickLoop()
{
    while (!m_stopping) {
        tick();
        std::unique_lock lock(m_mutex);
        m_wake.wait_for(lock, std::chrono::milliseconds(1000), [this] { return m_stopping.load(); });
    }
}

void Engine::tick()
{
    const std::int64_t now = nowMs();
    std::vector<std::shared_ptr<Conn>> ping;
    std::vector<std::tuple<std::string, std::uint16_t, std::string>> dials;
    bool show = false;
    {
        const std::lock_guard lock(m_mutex);
        if (!m_running)
            return;
        for (const auto& [id, conn] : m_links) {
            if (now - conn->lastRecvMs > kDeadMs) {
                conn->close();
                continue;
            }
            if (now - conn->lastPingMs >= kPingMs) {
                conn->lastPingMs = now;
                ping.push_back(conn);
            }
        }
        if (m_pairingUntil && m_pairingUntil <= now) {
            m_pairingUntil = 0;
            show = true;
        }
        for (auto it = m_nearby.begin(); it != m_nearby.end();) {
            if (now - it->second.seenMs > kNearbyMs) {
                it = m_nearby.erase(it);
                show = true;
            } else {
                ++it;
            }
        }
        for (auto it = m_pairs.begin(); it != m_pairs.end();) {
            const bool over = it->second.view.stage == PairView::Stage::Done || it->second.view.stage == PairView::Stage::Failed;
            if (!over && now - it->second.updatedMs > 150'000) {
                // Nobody answered: given up on both ends (the other end sees the connection end).
                it->second.view.stage = PairView::Stage::Failed;
                it->second.view.problem = "timeout";
                it->second.updatedMs = now;
                if (it->second.conn)
                    it->second.conn->close();
                show = true;
                ++it;
            } else if (over && now - it->second.updatedMs > 60'000) {
                it = m_pairs.erase(it);
                show = true;
            } else {
                ++it;
            }
        }
        for (auto it = m_outgoing.begin(); it != m_outgoing.end();) {
            it = now - it->second.createdMs > kOutgoingMs ? m_outgoing.erase(it) : std::next(it);
        }
        if (!m_options.paused) {
            for (auto& [id, peer] : m_peers) {
                if (m_links.count(id) || peer.connecting || peer.host.empty() || !peer.port || now < peer.nextTryMs)
                    continue;
                peer.connecting = true;
                peer.nextTryMs = now + 30'000; // until this try ends
                dials.emplace_back(peer.host, peer.port, id);
            }
            std::vector<std::string> addresses = m_options.addresses;
            for (const auto& [address, at] : m_dialing) {
                if (std::ranges::find(addresses, address) == addresses.end())
                    addresses.push_back(address);
            }
            for (const std::string& address : addresses) {
                const auto hp = parseHostPort(address, kTcpPort);
                if (!hp)
                    continue;
                const bool linked = std::ranges::any_of(m_links, [&](const auto& l) {
                    return l.second->host == hp->host && l.second->peerPort == hp->port;
                });
                const std::string key = hp->host + ":" + std::to_string(hp->port);
                auto& next = m_dialing[key];
                if (linked || now < next)
                    continue;
                next = now + 30'000;
                dials.emplace_back(hp->host, hp->port, std::string());
            }
        }
    }
    for (const auto& conn : ping)
        sendJson(conn, Json::Object {{"type", "ping"}, {"ts", static_cast<double>(now)}});
    for (auto& [host, port, id] : dials)
        spawn("dial", [this, host, port, id] { dial(host, port, id); });
    reap(false);
    if (show)
        changed();
}

// ---- what the list shows ------------------------------------------------------------------

Snapshot Engine::snapshot() const
{
    const std::lock_guard lock(m_mutex);
    Snapshot s;
    s.running = m_running;
    s.problem = m_problem;
    s.deviceId = m_deviceId;
    s.deviceName = m_options.deviceName;
    s.groupId = m_groupId;
    s.port = m_port.load();
    s.paused = m_options.paused;
    s.pairingLeftMs = std::max<std::int64_t>(0, m_pairingUntil - nowMs());
    s.acceptingPairing = m_options.acceptPairing || s.pairingLeftMs > 0;
    for (const auto& [id, peer] : m_peers) {
        MemberView v;
        v.deviceId = id;
        v.name = peer.name;
        v.platform = platformName(peer.platform);
        v.host = peer.host;
        v.port = peer.port;
        v.lastSeenMs = peer.lastSeenMs;
        if (const auto link = m_links.find(id); link != m_links.end()) {
            v.state = PeerState::Online;
            v.name = link->second->peerName.empty() ? v.name : link->second->peerName;
        } else {
            v.state = peer.connecting ? PeerState::Connecting : PeerState::Offline;
        }
        s.members.push_back(std::move(v));
    }
    std::ranges::sort(s.members, [](const MemberView& a, const MemberView& b) {
        if ((a.state == PeerState::Online) != (b.state == PeerState::Online))
            return a.state == PeerState::Online;
        return a.name < b.name;
    });
    for (const auto& [id, nearby] : m_nearby) {
        if (nearby.groupId == m_groupId || m_peers.count(id))
            continue;
        NearbyView v = nearby.view;
        v.platform = platformName(v.platform);
        s.nearby.push_back(std::move(v));
    }
    for (const auto& [id, pair] : m_pairs)
        s.pairs.push_back(pair.view);
    for (const auto& [id, in] : m_incoming)
        s.files.push_back(in.view);
    std::ranges::sort(s.files, std::greater<>(), &FilesView::timestampMs);
    for (const Received& r : m_received)
        s.received.push_back(r.view);
    return s;
}

} // namespace shunsync
