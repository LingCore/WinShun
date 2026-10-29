#include "Server.h"

#include "HttpLite.h"
#include "Network.h"
#include "Tls.h"
#include "Util.h"

#include <ws2tcpip.h>

#include <iphlpapi.h>

#include <algorithm>
#include <deque>

using winshun::Json;
using namespace std::chrono_literals;

namespace localsend {

namespace {

using Clock = std::chrono::steady_clock;

// What a peer may send us.
constexpr std::size_t kMaxRegisterBytes = 64 * 1024;
constexpr std::size_t kMaxPrepareBytes = Receiver::kMaxPrepareBytes;
constexpr std::size_t kMaxSmallBody = 64 * 1024; // cancel, info: nothing to speak of
constexpr DWORD kHandshakeMs = 10'000;
constexpr DWORD kIdleMs = 60'000; // a connection silent for this long is closed
constexpr auto kTextGap = 5s; // one text message per sender in this time
constexpr auto kPinBlock = 10min;
// Whole requests in time, not only bytes now and then: a peer sending a
// byte a minute would otherwise hold a connection (and a thread) for ever.
constexpr auto kFirstRequestTime = 20s; // the handshake and the first request's head
constexpr auto kHeadTime = 60s; // a later head (a kept connection waits for one)
constexpr auto kBodyTime = 60s; // a body, but an upload's (tick() watches those stall)
constexpr auto kAnswerGrace = 10s; // a request left asking past its deadline: its thread is gone
constexpr DWORD kDrainMs = 1'000;
constexpr auto kDrainTime = 2s;
constexpr std::size_t kPruneAt = 256; // a map by peer address this big: the entries that no longer matter go
constexpr Clock::time_point kNoDeadline = Clock::time_point::max();

constexpr const char* kApi = "/api/localsend/v2/";

void setTimeouts(SOCKET s, DWORD ms)
{
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
}

// `work` when the scope is left, however it is left.
template <typename Work>
class AtExit {
public:
    explicit AtExit(Work work) : m_work(std::move(work)) {}
    ~AtExit() { m_work(); }
    AtExit(const AtExit&) = delete;
    AtExit& operator=(const AtExit&) = delete;

private:
    Work m_work;
};

// A flood of addresses fills a map by peer: once it is big, the entries
// that no longer matter go (never the ones that still do).
template <typename Map, typename Stale>
void prune(Map& map, Stale stale)
{
    if (map.size() > kPruneAt)
        std::erase_if(map, stale);
}

// Refused before the body was read: answered, the connection to be closed,
// and what the peer still sends read and dropped for a moment first (a
// socket closed on unread bytes is reset, and the answer can go with it).
bool refuseUnread(http::Stream& stream, http::Reader& reader, const http::Head& head, SOCKET socket, int status, const char* message)
{
    http::respond(stream, status, http::messageJson(message), "application/json", true);
    setTimeouts(socket, kDrainMs);
    const Clock::time_point until = Clock::now() + kDrainTime;
    reader.body(head, true, kMaxPrepareBytes, [&](const char*, std::size_t) { return Clock::now() < until; });
    return false;
}

// Equal tokens, in time that does not depend on where they differ.
bool sameToken(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

// A connection another thread serves, stopped now: its blocking receive or
// send returns at once (shutdown alone leaves a receive waiting on a peer
// that keeps quiet). The serving thread still closes the socket.
void cutOff(SOCKET s)
{
    ::shutdown(s, SD_BOTH);
    ::CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
}

// The sender gave up while its prepare-upload waited: it closed the
// connection (or sent something, which a waiting sender never does).
bool peerLeft(SOCKET s)
{
    fd_set read;
    FD_ZERO(&read);
    FD_SET(s, &read);
    timeval none {0, 0};
    return ::select(0, &read, nullptr, nullptr, &none) > 0;
}

// Is the port free on every address (nobody, LocalSend included, holds
// it)? A bind to one address would otherwise win over another program's
// wildcard bind of the same port and take its connections.
bool portFree(int port)
{
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return false;
    BOOL exclusive = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    const bool free = ::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    ::closesocket(s);
    return free;
}

// Another program listening on the port's wildcard address: LocalSend
// started on this PC after us. Windows lets it bind 0.0.0.0 (and [::]) next
// to our own address, yet the connections to our address keep coming to
// us, never to it (the #113 interop with the official CLI). False when the
// tables cannot be read.
bool wildcardTakenByAnother(int port)
{
    const DWORD me = ::GetCurrentProcessId();
    for (const ULONG family : {ULONG {AF_INET}, ULONG {AF_INET6}}) {
        ULONG size = 0;
        if (::GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
            continue;
        std::vector<unsigned char> buffer(size + 4096);
        size = static_cast<ULONG>(buffer.size());
        if (::GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
            continue;
        if (family == AF_INET) {
            const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                const MIB_TCPROW_OWNER_PID& row = table->table[i];
                if (row.dwOwningPid != me && row.dwLocalAddr == INADDR_ANY && ::ntohs(static_cast<u_short>(row.dwLocalPort)) == port)
                    return true;
            }
        } else {
            const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                const MIB_TCP6ROW_OWNER_PID& row = table->table[i];
                const bool any = std::all_of(std::begin(row.ucLocalAddr), std::end(row.ucLocalAddr), [](UCHAR b) { return b == 0; });
                if (row.dwOwningPid != me && any && ::ntohs(static_cast<u_short>(row.dwLocalPort)) == port)
                    return true;
            }
        }
    }
    return false;
}

SOCKET listenOn(const std::string& ip, int port)
{
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return s;
    BOOL exclusive = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    ::inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || ::listen(s, SOMAXCONN) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

} // namespace

struct Receiver::Listener {
    ListenAddress address;
    SOCKET socket = INVALID_SOCKET;
    std::thread thread;
};

struct Receiver::Session {
    enum class Phase { Asking, Receiving };
    enum class FileState { Waiting, Receiving, Done, Failed };
    std::uint64_t id = 0;
    std::string sessionId; // the sender's handle on it
    DeviceInfo sender;
    std::string senderIp;
    bool verified = false;
    std::vector<FileMeta> files;
    std::vector<std::wstring> relative; // files[i]'s safe path
    std::map<std::string, std::size_t> byId;
    std::map<std::string, std::string> tokens;
    std::vector<FileState> states;
    std::vector<std::wstring> saved;
    std::unique_ptr<Placement> placement;
    std::uint64_t totalBytes = 0;
    std::atomic<std::uint64_t> receivedBytes {0};
    Phase phase = Phase::Asking;
    std::optional<bool> answer;
    std::atomic<bool> ended {false}; // finished: uploads still going stop
    Fault fault = Fault::Sender; // the worst a file failed for
    int busy = 0; // threads using it: its request's, its uploads'
    bool tidy = false; // ended early: its empty folders go once it is not busy
    Clock::time_point deadline;
    Clock::time_point lastActivity;
    std::set<SOCKET> uploading;
    std::deque<std::pair<Clock::time_point, std::uint64_t>> samples; // for the speed
    double bytesPerSecond = 0;
    std::wstring folder;
};

Receiver::Receiver(Credentials& credentials, Events events)
    : m_credentials(credentials)
    , m_events(std::move(events))
{
}

Receiver::~Receiver()
{
    stopListening();
    guarded("cancelling", [&] { cancel(); });
    std::unique_lock lock(m_mutex);
    m_stopping = true;
    m_answered.notify_all();
    for (const auto& connection : m_connections)
        cutOff(connection.first);
    // The connections' threads use this object: none may outlive it. They
    // end at once with their sockets cut; one that does not is a bug, and
    // ending the process beats it running on freed memory.
    if (!m_threadsDone.wait_for(lock, 20s, [&] { return m_threads == 0; })) {
        logWarning("connections did not end; exiting");
        ::TerminateProcess(::GetCurrentProcess(), 3);
    }
}

void Receiver::setConfig(ReceiverConfig config)
{
    const std::lock_guard lock(m_mutex);
    config.self.port = m_port ? m_port : config.self.port;
    m_config = std::move(config);
}

ReceiverConfig Receiver::config() const
{
    const std::lock_guard lock(m_mutex);
    return m_config;
}

int Receiver::port() const
{
    const std::lock_guard lock(m_mutex);
    return m_port;
}

std::vector<std::string> Receiver::addresses() const
{
    const std::lock_guard lock(m_mutex);
    std::vector<std::string> out;
    for (const auto& l : m_listeners)
        out.push_back(l->address.ip);
    return out;
}

int Receiver::listen(const std::vector<ListenAddress>& addresses, int port, bool idle)
{
    int current = 0;
    bool underWay = false;
    {
        const std::lock_guard lock(m_mutex);
        if (m_stopping)
            return 0; // shut down: never listening again
        std::vector<std::string> now, wanted;
        for (const auto& l : m_listeners)
            now.push_back(l->address.ip);
        for (const ListenAddress& a : addresses)
            wanted.push_back(a.ip);
        std::sort(now.begin(), now.end());
        std::sort(wanted.begin(), wanted.end());
        if (m_port != 0 && now == wanted && !wanted.empty()) {
            current = m_port;
            underWay = m_session != nullptr;
        }
    }
    if (current != 0) {
        // LocalSend opened on this PC since: it gets the port (once nothing
        // is under way here) and we take the next free one. On that later
        // port, back to ours once LocalSend let it go, between transfers (the
        // announcement that follows tells the devices nearby).
        if (!underWay && wildcardTakenByAnother(current))
            logInfo("port " + std::to_string(current) + " given up: another program listens on it too");
        else if (current != port && idle && !underWay && portFree(port))
            logInfo("port " + std::to_string(port) + " free again: back to it");
        else
            return current; // as it is
    }
    stopListening();
    if (addresses.empty())
        return 0;
    for (int p = port; p <= kLastPort; ++p) {
        if (!portFree(p))
            continue;
        std::vector<std::unique_ptr<Listener>> made;
        bool ok = true;
        for (const ListenAddress& a : addresses) {
            auto l = std::make_unique<Listener>();
            l->address = a;
            l->socket = listenOn(a.ip, p);
            if (l->socket == INVALID_SOCKET) {
                ok = false;
                break;
            }
            made.push_back(std::move(l));
        }
        if (!ok) {
            for (const auto& l : made)
                ::closesocket(l->socket);
            continue;
        }
        const std::lock_guard lock(m_mutex);
        if (m_stopping) {
            for (const auto& l : made)
                ::closesocket(l->socket);
            return 0;
        }
        m_listeners.reserve(m_listeners.size() + made.size()); // a listener whose thread runs is never dropped
        m_port = p;
        m_config.self.port = p;
        for (auto& l : made) {
            Listener* raw = l.get();
            try {
                l->thread = std::thread([this, raw] { acceptLoop(raw); });
            } catch (const std::exception& e) {
                logStopped("listening on an address", e.what());
                ::closesocket(l->socket);
                continue;
            }
            m_listeners.push_back(std::move(l));
        }
        return p;
    }
    logWarning("no port free from " + std::to_string(port) + " to " + std::to_string(kLastPort));
    return 0;
}

void Receiver::stopListening()
{
    std::vector<std::unique_ptr<Listener>> listeners;
    {
        const std::lock_guard lock(m_mutex);
        listeners = std::move(m_listeners);
        m_listeners.clear();
        m_port = 0;
    }
    for (const auto& l : listeners)
        ::closesocket(l->socket); // accept() returns: the thread ends
    for (const auto& l : listeners) {
        if (l->thread.joinable())
            l->thread.join();
    }
}

void Receiver::acceptLoop(Listener* listener)
{
    for (;;) {
        sockaddr_in peer {};
        int size = sizeof peer;
        const SOCKET s = ::accept(listener->socket, reinterpret_cast<sockaddr*>(&peer), &size);
        if (s == INVALID_SOCKET) {
            const int error = ::WSAGetLastError();
            if (error == WSAECONNRESET || error == WSAEMFILE || error == WSAENOBUFS) {
                ::Sleep(50);
                continue;
            }
            return; // closed
        }
        const std::uint32_t address = ntohl(peer.sin_addr.s_addr);
        // Only from the network it came in on: nothing routed from elsewhere.
        bool taken = false;
        if (sameSubnet(address, listener->address.address, listener->address.prefix))
            guarded("taking a connection", [&] { taken = admit(s, ipv4Text(address)); });
        if (!taken)
            ::closesocket(s);
    }
}

bool Receiver::admit(SOCKET s, const std::string& peerIp)
{
    const std::lock_guard lock(m_mutex);
    const auto peerCount = m_perPeer.find(peerIp);
    if (m_stopping || m_connections.size() >= kMaxConnections
        || (peerCount != m_perPeer.end() && peerCount->second >= kMaxConnectionsPerPeer))
        return false;
    const auto entry = m_connections.emplace(s, Clock::now() + kFirstRequestTime).first;
    try {
        ++m_perPeer[peerIp];
        std::thread([this, s, peerIp] { connection(s, peerIp); }).detach();
    } catch (...) {
        // No thread: undone (the count only when it was made).
        m_connections.erase(entry);
        if (const auto it = m_perPeer.find(peerIp); it != m_perPeer.end() && --it->second <= 0)
            m_perPeer.erase(it);
        throw;
    }
    ++m_threads;
    return true;
}

void Receiver::connection(SOCKET s, const std::string& peerIp)
{
    guarded("a connection", [&] { serve(s, peerIp); });
    {
        // Out of the map before it is closed: its number may be the next
        // connection's at once, and that one must not be cut for it.
        const std::lock_guard lock(m_mutex);
        m_connections.erase(s);
        if (const auto it = m_perPeer.find(peerIp); it != m_perPeer.end() && --it->second <= 0)
            m_perPeer.erase(it);
    }
    ::closesocket(s);
    const std::lock_guard lock(m_mutex);
    --m_threads;
    m_threadsDone.notify_all();
}

void Receiver::serve(SOCKET socket, const std::string& peerIp)
{
    setTimeouts(socket, kHandshakeMs);
    BOOL noDelay = TRUE;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
    TlsChannel tls(socket, m_credentials);
    std::string error;
    if (!tls.handshake(&error))
        return; // a port scan, a plain HTTP client, a peer that gave up: not worth a line
    setTimeouts(socket, kIdleMs);
    http::Reader reader(tls);
    for (bool first = true;; first = false) {
        if (!first)
            deadline(socket, Clock::now() + kHeadTime);
        http::Head head;
        const http::ReadResult r = reader.head(head, true);
        if (r == http::ReadResult::TooLarge) {
            http::respond(tls, 400, http::messageJson("Request header too large"), "application/json", true);
            break;
        }
        if (r != http::ReadResult::Ok)
            break;
        deadline(socket, Clock::now() + kBodyTime); // an upload, or a request waiting for the user, lifts it
        if (!allowed(head, peerIp)) {
            refuseUnread(tls, reader, head, socket, 429, "Too many requests");
            break;
        }
        if (!route(tls, reader, head, peerIp, socket) || head.close())
            break;
    }
    tls.shutdown();
}

bool Receiver::allowed(const http::Head& head, const std::string& peerIp)
{
    const bool upload = head.method == "POST" && head.path == std::string(kApi) + "upload";
    std::string sessionId;
    if (upload) {
        const std::map<std::string, std::string> query = http::parseQuery(head.query);
        if (const auto it = query.find("sessionId"); it != query.end())
            sessionId = it->second;
    }
    const std::lock_guard lock(m_mutex);
    if (upload && m_session && !m_session->ended && m_session->phase == Session::Phase::Receiving && m_session->senderIp == peerIp
        && sameToken(sessionId, m_session->sessionId))
        return true; // a file of the transfer under way: its token is checked next
    return m_requests.allow(peerIp, Clock::now());
}

void Receiver::deadline(SOCKET socket, Clock::time_point until)
{
    const std::lock_guard lock(m_mutex);
    if (const auto it = m_connections.find(socket); it != m_connections.end())
        it->second = until;
}

bool Receiver::route(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp, SOCKET socket)
{
    const std::string path = head.path;
    const bool api = path.rfind(kApi, 0) == 0;
    const std::string what = api ? path.substr(std::char_traits<char>::length(kApi)) : std::string();
    if (head.method == "POST" && what == "register")
        return registerPeer(tls, reader, head, peerIp);
    if (head.method == "POST" && what == "prepare-upload")
        return prepareUpload(tls, reader, head, peerIp, socket);
    if (head.method == "POST" && what == "upload")
        return upload(tls, reader, head, peerIp, socket);
    if (head.method == "POST" && what == "cancel")
        return cancelRequest(tls, reader, head, peerIp);
    std::string body;
    if (reader.bodyText(head, true, kMaxSmallBody, body) != http::ReadResult::Ok)
        return false;
    if ((head.method == "GET" || head.method == "POST") && what == "info") {
        const ReceiverConfig c = config();
        return http::respond(tls, 200, toJson(c.self, false).dump());
    }
    return http::respond(tls, 404, http::messageJson("Not found"));
}

bool Receiver::registerPeer(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp)
{
    std::string body;
    const http::ReadResult r = reader.bodyText(head, true, kMaxRegisterBytes, body);
    if (r != http::ReadResult::Ok) {
        if (r == http::ReadResult::TooLarge)
            http::respond(tls, 413, http::messageJson("Too large"), "application/json", true);
        return false;
    }
    // (Too many a second: cut off in serve(), as every route is.)
    const std::optional<Json> json = parsePeerJson(body, kMaxDeviceValues);
    const std::optional<DeviceInfo> peer = json ? deviceFromJson(*json) : std::nullopt;
    // LocalSend 1.18 sends the certificate its fingerprint names; one that
    // shows another is ignored, as LocalSend's own server does (still 200).
    const bool agrees = peer && (tls.peerFingerprint().empty() || equalsNoCase(tls.peerFingerprint(), peer->fingerprint));
    const ReceiverConfig c = config();
    if (peer && agrees && !equalsNoCase(peer->fingerprint, c.self.fingerprint) && m_events.registered)
        m_events.registered(*peer, peerIp, !tls.peerFingerprint().empty());
    return http::respond(tls, 200, toJson(c.self, false).dump());
}

bool Receiver::prepareUpload(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp, SOCKET socket)
{
    using Kind = ReceiveOutcome::Kind;
    const std::map<std::string, std::string> query = http::parseQuery(head.query);
    const ReceiverConfig c = config();
    // Settled before the body is read: a sender that may not ask (no PIN,
    // a wrong one) or not now (busy) never has megabytes of its JSON held.
    int refusal = 0;
    const char* message = nullptr;
    {
        const std::lock_guard lock(m_mutex);
        const Clock::time_point now = Clock::now();
        if (m_stopping) {
            refusal = 503;
            message = "Shutting down";
        } else if (!c.pin.empty()) {
            prune(m_pinAttempts, [&](const auto& entry) { return now - entry.second.last > kPinBlock; });
            Attempts& a = m_pinAttempts[peerIp];
            if (a.count > 0 && now - a.last > kPinBlock)
                a = {};
            const auto pin = query.find("pin");
            if (a.count >= kPinAttempts) {
                refusal = 429;
                message = "Too many attempts";
            } else if (pin == query.end()) {
                refusal = 401;
                message = "PIN required";
            } else if (!sameToken(pin->second, c.pin)) {
                ++a.count;
                a.last = now;
                refusal = a.count >= kPinAttempts ? 429 : 401;
                message = "Invalid PIN";
            } else {
                a = {};
            }
        }
        if (!refusal && m_session) {
            refusal = 409;
            message = "Blocked by another session";
        }
        // A body to read: one of the few slots for it, or refused unread (409,
        // as busy: another Win顺 would take a 429 for "you were just told no").
        if (!refusal && !m_reading.claim(peerIp)) {
            refusal = 409;
            message = "Blocked by another session";
        }
    }
    if (refusal)
        return refuseUnread(tls, reader, head, socket, refusal, message);

    std::optional<PrepareUpload> request;
    bool tooMany = false;
    {
        // The slot given back once the body is parsed, however this is left;
        // never held through the wait for the user.
        const AtExit unclaim([&] {
            const std::lock_guard lock(m_mutex);
            m_reading.release(peerIp);
        });
        std::string body;
        const http::ReadResult r = reader.bodyText(head, true, kMaxPrepareBytes, body);
        // Few enough values to parse: "[0,0,0,…]" of 4 MB would be two million parsed.
        if (r == http::ReadResult::TooLarge || (r == http::ReadResult::Ok && !fewValues(body, kMaxValues))) {
            http::respond(tls, 413, http::messageJson("Too many files"), "application/json", true);
            return false;
        }
        if (r != http::ReadResult::Ok)
            return false;
        std::string why;
        if (const std::optional<Json> json = Json::parse(body)) {
            // More files than one request takes: said as a list too big is
            // (413: another Win顺 says "fewer at once"), not "malformed".
            const Json& files = (*json)["files"];
            tooMany = files.isObject() && files.toObject().size() > kMaxFiles;
            if (!tooMany)
                request = parsePrepareUpload(*json, &why);
        }
    } // the body and its JSON let go before the wait
    if (tooMany)
        return http::respond(tls, 413, http::messageJson("Too many files"));
    if (!request)
        return http::respond(tls, 400, http::messageJson("Request body malformed"));
    // A certificate that is not the one its fingerprint names is somebody
    // else's word for it: refused. None at all (older senders): unverified.
    if (!tls.peerFingerprint().empty() && !equalsNoCase(tls.peerFingerprint(), request->info.fingerprint))
        return http::respond(tls, 403, http::messageJson("Fingerprint does not match the certificate"));
    const bool verified = !tls.peerFingerprint().empty();

    if (isTextMessage(*request)) {
        TextMessage text;
        bool tooSoon = false;
        {
            const std::lock_guard lock(m_mutex);
            const Clock::time_point now = Clock::now();
            prune(m_lastText, [&](const auto& entry) { return now - entry.second >= kTextGap; });
            auto& last = m_lastText[peerIp];
            tooSoon = last != Clock::time_point {} && now - last < kTextGap;
            if (!tooSoon) {
                last = now;
                text.id = m_nextId++;
            }
        }
        if (tooSoon)
            return http::respond(tls, 429, http::messageJson("Too many messages"));
        text.sender = request->info;
        text.text = std::move(request->files.front().preview);
        if (m_events.text)
            m_events.text(text);
        return http::respond(tls, 204);
    }

    // Every name made safe up front: one that is not refuses the whole request.
    auto session = std::make_shared<Session>();
    for (const FileMeta& f : request->files) {
        std::string why;
        std::optional<std::wstring> relative = safeRelativePath(f.fileName, &why);
        if (!relative)
            return http::respond(tls, 400, http::messageJson("Invalid file name"));
        session->relative.push_back(std::move(*relative));
    }
    bool cooling = false;
    {
        const std::lock_guard lock(m_mutex);
        const auto peer = m_peers.find(peerIp);
        cooling = peer != m_peers.end() && Clock::now() < peer->second.coolUntil;
    }
    if (cooling)
        return http::respond(tls, 429, http::messageJson("Too many requests"));
    const std::uint64_t total = request->totalBytes();
    // Refused each time, told once in a while: no place to save (a drive
    // pulled out, a share gone; else each file would fail once accepted), or
    // not enough space there.
    const auto refuse = [&](Kind kind, const char* why) {
        bool tellIt = false;
        {
            const std::lock_guard lock(m_mutex);
            const Clock::time_point now = Clock::now();
            prune(m_peers, [&](const auto& entry) { return entry.second.stale(now); });
            PeerState& peer = m_peers[peerIp];
            tellIt = peer.refusalTold == Clock::time_point {} || now - peer.refusalTold >= kCooldown;
            if (tellIt)
                peer.refusalTold = now;
        }
        if (tellIt && m_events.finished) {
            ReceiveOutcome outcome;
            outcome.kind = kind;
            outcome.sender = request->info;
            outcome.bytes = total;
            outcome.folder = c.folder;
            m_events.finished(outcome);
        }
        return http::respond(tls, 403, http::messageJson(why));
    };
    if (!folderUsable(c.folder))
        return refuse(Kind::NoFolder, "Save location unavailable");
    const std::uint64_t free = freeBytes(c.folder);
    if (free != 0 && (total > free || free - total < c.spareBytes))
        return refuse(Kind::NoSpace, "Not enough space");
    session->sender = request->info;
    session->senderIp = peerIp;
    session->verified = verified;
    session->files = std::move(request->files);
    for (std::size_t i = 0; i < session->files.size(); ++i)
        session->byId[session->files[i].id] = i;
    session->states.assign(session->files.size(), Session::FileState::Waiting);
    session->totalBytes = total;
    session->folder = c.folder;
    request.reset();
    bool taken = false;
    {
        const std::lock_guard lock(m_mutex);
        if (!m_session && !m_stopping) {
            session->id = m_nextId++;
            session->deadline = Clock::now() + kAnswerTime;
            session->busy = 1; // this thread, till it has answered
            m_session = session;
            taken = true;
        }
    }
    if (!taken)
        return http::respond(tls, 409, http::messageJson("Blocked by another session"));
    const AtExit released([&] { release(session); });
    deadline(socket, kNoDeadline); // the wait has its own
    IncomingRequest asked;
    asked.id = session->id;
    asked.sender = session->sender;
    asked.verified = verified;
    asked.files = session->files;
    asked.totalBytes = total;
    asked.deadline = session->deadline;
    if (m_events.asked)
        m_events.asked(asked);

    // Waiting for the user, the deadline or the sender giving up. What ends
    // the wait is settled under the lock, the session ended with it: a yes
    // can never come after "no answer" was decided (decide() says false).
    ReceiveOutcome outcome;
    std::optional<Kind> end; // how this thread ends it
    std::unique_lock lock(m_mutex);
    while (!session->ended && !session->answer && !m_stopping && Clock::now() < session->deadline) {
        m_answered.wait_for(lock, 500ms);
        if (session->ended || session->answer || m_stopping)
            break;
        lock.unlock();
        const bool left = peerLeft(socket);
        lock.lock();
        if (left && !session->ended && !session->answer) {
            end = Kind::SenderCancelled;
            break;
        }
    }
    if (!session->ended && !end) {
        if (m_stopping)
            end = Kind::Cancelled;
        else if (!session->answer)
            end = Kind::TimedOut;
        else if (!*session->answer)
            end = Kind::Declined;
    }
    if (session->ended || end) {
        const bool tellIt = end && endLocked(*session, *end, outcome);
        lock.unlock();
        if (tellIt)
            tell(session, outcome);
        if (end == Kind::SenderCancelled)
            return false; // nobody to answer
        if (end == Kind::TimedOut)
            return http::respond(tls, 403, http::messageJson("No answer"));
        if (end == Kind::Declined)
            return http::respond(tls, 403, http::messageJson("Rejected"));
        http::respond(tls, 403, http::messageJson("Cancelled"), "application/json", true);
        return false;
    }
    // Accepted: tokens for the files, and where they go.
    session->sessionId = randomToken();
    session->phase = Session::Phase::Receiving;
    session->lastActivity = Clock::now();
    session->placement = std::make_unique<Placement>(session->folder);
    Json tokens;
    for (const FileMeta& f : session->files) {
        const std::string token = randomToken();
        session->tokens[f.id] = token;
        tokens.set(f.id, token);
    }
    Json reply;
    reply.set("sessionId", session->sessionId);
    reply.set("files", tokens);
    lock.unlock();
    return http::respond(tls, 200, reply.dump());
}

bool Receiver::upload(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp, SOCKET socket)
{
    const std::map<std::string, std::string> query = http::parseQuery(head.query);
    const auto value = [&](const char* name) {
        const auto it = query.find(name);
        return it == query.end() ? std::string() : it->second;
    };
    const std::string sessionId = value("sessionId");
    const std::string fileId = value("fileId");
    const std::string token = value("token");
    deadline(socket, kNoDeadline); // a big file takes its time; tick() watches it stall
    std::shared_ptr<Session> session;
    std::size_t index = 0;
    std::wstring target;
    int refusal = 0;
    const char* message = nullptr;
    {
        const std::lock_guard lock(m_mutex);
        if (!m_session || m_session->ended || m_session->phase != Session::Phase::Receiving
            || !sameToken(sessionId, m_session->sessionId) || m_session->senderIp != peerIp) {
            refusal = 403;
            message = "Invalid session";
        } else {
            const auto it = m_session->byId.find(fileId);
            const auto t = m_session->tokens.find(fileId);
            if (it == m_session->byId.end() || t == m_session->tokens.end() || !sameToken(token, t->second)) {
                refusal = 403;
                message = "Invalid token";
            } else if (m_session->states[it->second] != Session::FileState::Waiting) {
                refusal = 409;
                message = "Already received";
            } else {
                session = m_session;
                index = it->second;
                target = session->placement->place(session->relative[index]);
                session->uploading.insert(socket);
                session->states[index] = Session::FileState::Receiving;
                session->lastActivity = Clock::now();
                ++session->busy;
            }
        }
    }
    if (refusal) // its body is not read
        return refuseUnread(tls, reader, head, socket, refusal, message);
    const AtExit released([&] {
        {
            const std::lock_guard lock(m_mutex);
            session->uploading.erase(socket);
        }
        release(session);
    });
    const FileMeta& meta = session->files[index];
    std::string error;
    // Its partial file straight in the save folder, where the next start finds one left behind.
    std::unique_ptr<IncomingFile> file = IncomingFile::create(target, meta.size, meta.sha256, &error, session->folder);
    std::uint64_t got = 0;
    bool diskFailed = false;
    http::ReadResult r = http::ReadResult::Malformed;
    if (file) {
        // The reader never gives more than declared: a write that fails is the disk's.
        r = reader.body(head, true, meta.size, [&](const char* data, std::size_t size) {
            if (session->ended)
                return false;
            if (!file->write(data, size)) {
                diskFailed = true;
                return false;
            }
            session->receivedBytes += size;
            return true;
        }, &got);
    } else {
        logWarning("a received file could not be made: " + error);
    }
    std::optional<std::wstring> saved;
    std::string why;
    Fault fault = !file || diskFailed ? Fault::Disk : Fault::Sender;
    if (file && r == http::ReadResult::Ok && !session->ended)
        saved = file->finish("https://" + peerIp + ":" + std::to_string(session->sender.port) + "/", &why, &fault);
    else if (file)
        file->abort();
    {
        const std::lock_guard lock(m_mutex);
        session->uploading.erase(socket);
        session->lastActivity = Clock::now();
        if (saved) {
            session->states[index] = Session::FileState::Done;
            session->saved.push_back(*saved);
        } else {
            session->states[index] = Session::FileState::Failed;
            session->receivedBytes -= std::min<std::uint64_t>(session->receivedBytes, got);
            if (!session->ended && fault > session->fault)
                session->fault = fault;
        }
    }
    if (!saved && !why.empty())
        logWarning("a received file was dropped: " + why);
    const bool cut = session->ended; // cancelled while this one came in (not ended by it, the last file)
    settleIfDone(session);
    if (saved)
        return http::respond(tls, 200);
    if (cut) {
        http::respond(tls, 403, http::messageJson("Cancelled"), "application/json", true);
        return false;
    }
    http::respond(tls, fault == Fault::Disk ? 500 : 400,
        http::messageJson(r == http::ReadResult::TooLarge ? "More than declared" : "Upload failed"), "application/json", true);
    return false;
}

bool Receiver::cancelRequest(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp)
{
    std::string body;
    if (reader.bodyText(head, true, kMaxSmallBody, body) != http::ReadResult::Ok)
        return false;
    const std::map<std::string, std::string> query = http::parseQuery(head.query);
    const auto id = query.find("sessionId");
    std::shared_ptr<Session> session;
    {
        const std::lock_guard lock(m_mutex);
        if (m_session && m_session->senderIp == peerIp
            && (m_session->phase == Session::Phase::Asking || (id != query.end() && sameToken(id->second, m_session->sessionId))))
            session = m_session;
    }
    if (session)
        finish(session, ReceiveOutcome::Kind::SenderCancelled);
    return http::respond(tls, 200);
}

void Receiver::settleIfDone(const std::shared_ptr<Session>& session)
{
    bool done = true;
    bool all = true;
    {
        const std::lock_guard lock(m_mutex);
        for (const Session::FileState s : session->states) {
            if (s == Session::FileState::Waiting || s == Session::FileState::Receiving)
                done = false;
            if (s != Session::FileState::Done)
                all = false;
        }
    }
    if (done)
        finish(session, all ? ReceiveOutcome::Kind::Received : ReceiveOutcome::Kind::Failed);
}

bool Receiver::endLocked(Session& session, ReceiveOutcome::Kind kind, ReceiveOutcome& outcome)
{
    using Kind = ReceiveOutcome::Kind;
    if (session.ended.exchange(true))
        return false; // told already
    for (const SOCKET s : session.uploading)
        cutOff(s); // the uploads under way stop now, their files deleted
    if (m_session.get() == &session)
        m_session.reset();
    // Not taken: the same sender is not heard for a while. Not when this
    // side cancelled (its next request may well be wanted), nor when the
    // sender gave up its first one in a while (sent again at once, rightly).
    if (session.phase == Session::Phase::Asking && (kind == Kind::Declined || kind == Kind::TimedOut || kind == Kind::SenderCancelled)) {
        try {
            const Clock::time_point now = Clock::now();
            prune(m_peers, [&](const auto& entry) { return entry.second.stale(now); });
            PeerState& peer = m_peers[session.senderIp];
            const bool again = peer.untaken != Clock::time_point {} && now - peer.untaken < kRepeatTime;
            if (kind != Kind::SenderCancelled || again)
                peer.coolUntil = now + kCooldown;
            peer.untaken = now;
        } catch (const std::exception&) {
            // No memory for it: the session ends all the same.
        }
    }
    session.tidy = kind != Kind::Received && session.placement != nullptr;
    m_answered.notify_all();
    outcome.kind = kind;
    outcome.sender = session.sender;
    outcome.saved = session.saved;
    outcome.bytes = session.receivedBytes;
    outcome.folder = session.folder;
    outcome.fault = session.fault;
    for (const Session::FileState s : session.states) {
        if (s != Session::FileState::Done)
            ++outcome.failed;
    }
    return true;
}

void Receiver::finish(const std::shared_ptr<Session>& session, ReceiveOutcome::Kind kind)
{
    ReceiveOutcome outcome;
    {
        const std::lock_guard lock(m_mutex);
        if (!endLocked(*session, kind, outcome))
            return;
    }
    tell(session, outcome);
}

void Receiver::tell(const std::shared_ptr<Session>& session, const ReceiveOutcome& outcome)
{
    if (m_events.finished)
        m_events.finished(outcome);
    tidyIfIdle(session);
}

void Receiver::release(const std::shared_ptr<Session>& session) noexcept
{
    {
        const std::lock_guard lock(m_mutex);
        --session->busy;
        m_threadsDone.notify_all(); // shutDown() waits for the files to be let go
    }
    guarded("tidying up", [&] { tidyIfIdle(session); });
}

void Receiver::tidyIfIdle(const std::shared_ptr<Session>& session)
{
    std::vector<std::wstring> folders;
    {
        const std::lock_guard lock(m_mutex);
        if (!session->tidy || session->busy > 0)
            return;
        session->tidy = false;
        folders = session->placement->folders();
    }
    for (const std::wstring& folder : folders)
        removeEmptyFolders(folder);
}

bool Receiver::decide(std::uint64_t requestId, bool accept)
{
    const std::lock_guard lock(m_mutex);
    // Past the deadline it is too late, even before the waiting request has
    // noticed: the sender hears "no answer", never a late yes.
    if (!m_session || m_session->ended || m_session->id != requestId || m_session->phase != Session::Phase::Asking
        || m_session->answer || Clock::now() >= m_session->deadline)
        return false;
    m_session->answer = accept;
    m_answered.notify_all();
    return true;
}

bool Receiver::cancel()
{
    std::shared_ptr<Session> session;
    {
        const std::lock_guard lock(m_mutex);
        session = m_session;
    }
    if (!session)
        return false;
    finish(session, ReceiveOutcome::Kind::Cancelled);
    return true;
}

void Receiver::shutDown(std::chrono::milliseconds wait)
{
    stopListening();
    std::shared_ptr<Session> session;
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true; // no connection, request or listening more
        session = m_session;
    }
    if (!session)
        return;
    finish(session, ReceiveOutcome::Kind::Cancelled);
    std::unique_lock lock(m_mutex);
    m_threadsDone.wait_for(lock, wait, [&] { return session->busy == 0; });
}

std::optional<IncomingRequest> Receiver::pending() const
{
    const std::lock_guard lock(m_mutex);
    if (!m_session || m_session->phase != Session::Phase::Asking || m_session->answer)
        return std::nullopt;
    IncomingRequest r;
    r.id = m_session->id;
    r.sender = m_session->sender;
    r.verified = m_session->verified;
    r.files = m_session->files;
    r.totalBytes = m_session->totalBytes;
    r.deadline = m_session->deadline;
    return r;
}

std::optional<ReceiveProgress> Receiver::progress() const
{
    const std::lock_guard lock(m_mutex);
    // Accepted, its thread not yet on to receiving: the transfer already
    // (the rows drawn right after Enter show it, not nothing).
    const bool accepted = m_session && m_session->answer && *m_session->answer;
    if (!m_session || (m_session->phase != Session::Phase::Receiving && !accepted))
        return std::nullopt;
    ReceiveProgress p;
    p.sender = m_session->sender;
    p.files = m_session->files.size();
    p.filesDone = static_cast<std::size_t>(std::count(m_session->states.begin(), m_session->states.end(), Session::FileState::Done));
    p.totalBytes = m_session->totalBytes;
    p.receivedBytes = m_session->receivedBytes;
    p.bytesPerSecond = m_session->bytesPerSecond;
    return p;
}

void Receiver::tick()
{
    std::shared_ptr<Session> stalled;
    std::shared_ptr<Session> forgotten;
    {
        const std::lock_guard lock(m_mutex);
        const Clock::time_point now = Clock::now();
        for (auto& [s, until] : m_connections) {
            if (now >= until) {
                cutOff(s); // its request did not come whole in time
                until = kNoDeadline;
            }
        }
        if (m_session && m_session->phase == Session::Phase::Asking && now >= m_session->deadline + kAnswerGrace)
            forgotten = m_session; // its request's thread, which ends it at the deadline, is gone
        if (m_session && m_session->phase == Session::Phase::Receiving) {
            Session& s = *m_session;
            s.samples.emplace_back(now, s.receivedBytes.load());
            while (s.samples.size() > 2 && now - s.samples.front().first > 3s)
                s.samples.pop_front();
            if (s.samples.size() >= 2) {
                const double seconds = std::chrono::duration<double>(s.samples.back().first - s.samples.front().first).count();
                const auto bytes = static_cast<double>(s.samples.back().second - std::min(s.samples.back().second, s.samples.front().second));
                s.bytesPerSecond = seconds > 0.2 ? bytes / seconds : s.bytesPerSecond;
                if (bytes > 0)
                    s.lastActivity = now;
            }
            if (now - s.lastActivity > kStallTime) // no bytes, nor a file begun or ended
                stalled = m_session;
        }
    }
    if (forgotten)
        finish(forgotten, ReceiveOutcome::Kind::TimedOut);
    if (stalled)
        finish(stalled, ReceiveOutcome::Kind::Failed);
}

} // namespace localsend
