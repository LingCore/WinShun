#include "Client.h"

#include "HttpLite.h"
#include "Tls.h"
#include "Util.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace localsend {

namespace {

constexpr DWORD kSendMs = 60'000; // a device that takes no bytes for this long has gone
constexpr std::size_t kChunk = 256 * 1024;

void setTimeout(SOCKET s, int option, std::chrono::milliseconds time)
{
    const DWORD ms = static_cast<DWORD>(std::clamp<long long>(time.count(), 1, 24LL * 3600 * 1000));
    ::setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&ms), sizeof ms);
}

// A connection for one request: the socket, TLS over it when the device
// speaks HTTPS, and its place in the cutter. Closed at the end of its scope.
class Connection {
public:
    Connection(Credentials& credentials, Cutter* cutter) : m_credentials(credentials), m_cutter(cutter) {}
    ~Connection()
    {
        if (m_tls)
            m_tls->shutdown();
        m_tls.reset();
        if (m_socket != INVALID_SOCKET) {
            if (m_cutter)
                m_cutter->remove(m_socket);
            ::closesocket(m_socket);
        }
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool open(const Target& target, std::chrono::milliseconds connectTime, Reply& reply)
    {
        m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET) {
            reply.error = "socket " + std::to_string(::WSAGetLastError());
            return false;
        }
        if (m_cutter && !m_cutter->add(m_socket)) {
            reply.error = "cancelled";
            ::closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }
        if (target.interfaceIndex) {
            const DWORD index = htonl(target.interfaceIndex); // IPv4 wants it in network order
            ::setsockopt(m_socket, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<const char*>(&index), sizeof index);
        }
        if (target.localAddress) {
            sockaddr_in local {};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(target.localAddress);
            if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
                reply.error = "bind " + std::to_string(::WSAGetLastError());
                return false;
            }
        }
        sockaddr_in to {};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<u_short>(target.port));
        if (::inet_pton(AF_INET, target.ip.c_str(), &to.sin_addr) != 1) {
            reply.error = "not an address";
            return false;
        }
        // Connect without blocking, so that it can time out (and be cut) in
        // a moment: a scan tries a whole subnet of addresses nobody has.
        u_long nonBlocking = 1;
        ::ioctlsocket(m_socket, FIONBIO, &nonBlocking);
        if (::connect(m_socket, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0 && ::WSAGetLastError() != WSAEWOULDBLOCK) {
            reply.error = "connect " + std::to_string(::WSAGetLastError());
            return false;
        }
        const auto until = std::chrono::steady_clock::now() + connectTime;
        for (;;) {
            if (m_cutter && m_cutter->isCut()) {
                reply.error = "cancelled";
                return false;
            }
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(m_socket, &writable);
            FD_SET(m_socket, &failed);
            timeval slice {0, 100'000};
            const int ready = ::select(0, nullptr, &writable, &failed, &slice);
            if (ready < 0) {
                reply.error = "select " + std::to_string(::WSAGetLastError());
                return false;
            }
            if (ready > 0) {
                int problem = 0;
                int size = sizeof problem;
                ::getsockopt(m_socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&problem), &size);
                if (FD_ISSET(m_socket, &failed) || problem != 0) {
                    reply.error = "connect " + std::to_string(problem ? problem : WSAECONNREFUSED);
                    return false;
                }
                break;
            }
            if (std::chrono::steady_clock::now() >= until) {
                reply.error = "connect timed out";
                return false;
            }
        }
        nonBlocking = 0;
        ::ioctlsocket(m_socket, FIONBIO, &nonBlocking);
        BOOL noDelay = TRUE;
        ::setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
        setTimeout(m_socket, SO_RCVTIMEO, std::chrono::milliseconds(10'000)); // the handshake
        setTimeout(m_socket, SO_SNDTIMEO, std::chrono::milliseconds(kSendMs));
        if (!target.https) {
            m_plain = std::make_unique<http::SocketStream>(m_socket);
            return true;
        }
        m_tls = std::make_unique<TlsChannel>(m_socket, m_credentials);
        std::string error;
        if (!m_tls->handshake(&error)) {
            reply.error = "TLS: " + error;
            m_tls.reset(); // nothing to close politely
            return false;
        }
        reply.peerFingerprint = m_tls->peerFingerprint();
        if (!target.fingerprint.empty() && !equalsNoCase(target.fingerprint, reply.peerFingerprint)) {
            reply.error = "certificate is not the device's";
            reply.wrongCertificate = true;
            return false;
        }
        return true;
    }

    http::Stream& stream() { return m_tls ? static_cast<http::Stream&>(*m_tls) : *m_plain; }
    SOCKET socket() const noexcept { return m_socket; }

private:
    Credentials& m_credentials;
    Cutter* m_cutter;
    SOCKET m_socket = INVALID_SOCKET;
    std::unique_ptr<TlsChannel> m_tls;
    std::unique_ptr<http::SocketStream> m_plain;
};

std::string requestHead(const Target& target, std::string_view method, const std::string& pathAndQuery, std::uint64_t length,
    std::string_view contentType)
{
    std::string head;
    head.reserve(256);
    head.append(method).append(" ").append(pathAndQuery).append(" HTTP/1.1\r\n");
    head += "Host: " + target.ip + ":" + std::to_string(target.port) + "\r\n";
    head += "User-Agent: WinShun\r\n";
    if (length > 0 || method == "POST") {
        head.append("Content-Type: ").append(contentType).append("\r\n");
        head += "Content-Length: " + std::to_string(length) + "\r\n";
    }
    head += "Connection: close\r\n\r\n";
    return head;
}

void readReply(Connection& connection, std::chrono::milliseconds timeout, std::size_t maxReply, Reply& reply)
{
    setTimeout(connection.socket(), SO_RCVTIMEO, timeout);
    http::Reader reader(connection.stream());
    http::Head head;
    const http::ReadResult r = reader.head(head, false);
    if (r != http::ReadResult::Ok) {
        reply.error = r == http::ReadResult::Closed ? "no answer" : "a malformed answer";
        return;
    }
    reply.status = head.status;
    const http::ReadResult b = reader.bodyText(head, false, maxReply, reply.body);
    if (b == http::ReadResult::TooLarge) {
        // More than any answer of LocalSend's: not one, and not held.
        reply.status = 0;
        reply.body = std::string();
        reply.error = "the answer was too long";
    } else if (b != http::ReadResult::Ok && reply.error.empty()) {
        reply.error = "the answer's body was cut short";
    }
}

} // namespace

void Cutter::cut()
{
    const std::lock_guard lock(m_mutex);
    m_cut = true;
    for (const SOCKET s : m_sockets) {
        ::shutdown(s, SD_BOTH);
        ::CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
    }
}

void Cutter::reset()
{
    const std::lock_guard lock(m_mutex);
    m_cut = false;
}

bool Cutter::add(SOCKET s)
{
    const std::lock_guard lock(m_mutex);
    if (m_cut)
        return false;
    m_sockets.insert(s);
    return true;
}

void Cutter::remove(SOCKET s)
{
    const std::lock_guard lock(m_mutex);
    m_sockets.erase(s);
}

Reply Client::request(const Target& target, std::string_view method, const std::string& pathAndQuery, const std::string& body,
    std::chrono::milliseconds timeout, Cutter* cutter, std::chrono::milliseconds connectTime, std::size_t maxReply)
{
    Reply reply;
    Connection connection(m_credentials, cutter);
    if (!connection.open(target, connectTime, reply))
        return reply;
    const std::string head = requestHead(target, method, pathAndQuery, body.size(), "application/json");
    if (!connection.stream().sendAll(head + body)) {
        reply.error = "the device stopped taking bytes";
        return reply;
    }
    readReply(connection, timeout, maxReply, reply);
    return reply;
}

Reply Client::upload(const Target& target, const std::string& pathAndQuery, const std::wstring& file, std::uint64_t size,
    const std::function<bool(std::uint64_t)>& sent, Cutter* cutter)
{
    Reply reply;
    const Handle in(::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER length {};
    if (!in.valid() || !::GetFileSizeEx(in.get(), &length)) {
        reply.error = "the file could not be read (" + std::to_string(::GetLastError()) + ")";
        return reply;
    }
    if (static_cast<std::uint64_t>(length.QuadPart) != size) {
        reply.error = "the file changed after it was chosen";
        return reply;
    }
    Connection connection(m_credentials, cutter);
    if (!connection.open(target, kConnectTime, reply))
        return reply;
    if (!connection.stream().sendAll(requestHead(target, "POST", pathAndQuery, size, "application/octet-stream"))) {
        reply.error = "the device stopped taking bytes";
        return reply;
    }
    std::vector<char> buffer(kChunk);
    std::uint64_t done = 0;
    while (done < size) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(kChunk, size - done));
        if (!::ReadFile(in.get(), buffer.data(), want, &got, nullptr) || got == 0) {
            reply.error = "the file could not be read to its end";
            return reply;
        }
        if (!connection.stream().sendAll(buffer.data(), got)) {
            reply.error = cutter && cutter->isCut() ? "cancelled" : "the device stopped taking bytes";
            // A receiver that refuses mid-way may have said why: read it.
            readReply(connection, std::chrono::milliseconds(2000), kMaxReply, reply);
            return reply;
        }
        done += got;
        if (sent && !sent(done)) {
            reply.error = "cancelled";
            return reply;
        }
    }
    readReply(connection, std::chrono::milliseconds(60'000), kMaxReply, reply);
    return reply;
}

} // namespace localsend
