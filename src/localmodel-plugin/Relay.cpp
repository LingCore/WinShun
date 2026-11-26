#include "Relay.h"

#include "HttpLite.h" // src/localsend-plugin: request heads and bodies with limits

#include "winshun.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

namespace lm {

namespace http = localsend::http;

namespace {

constexpr std::size_t kMaxHead = 64 * 1024;
constexpr std::uint64_t kMaxBody = 64ull << 20; // a batch of texts to embed fits
constexpr std::size_t kMaxConnections = 32;

std::string lowerAscii(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// The headers that belong to one hop, or that the relay sets itself.
bool passedOn(const std::string& name)
{
    static constexpr std::array kDropped {"host", "connection", "keep-alive", "proxy-connection", "proxy-authorization", "te",
        "trailer", "transfer-encoding", "upgrade", "content-length", "origin", "expect"};
    return std::ranges::find(kDropped, name) == kDropped.end();
}

bool sendAll(SOCKET socket, const char* data, std::size_t size)
{
    while (size > 0) {
        const int sent = ::send(socket, data, static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

} // namespace

std::string errorJson(const std::string& message, const std::string& code)
{
    using winshun::Json;
    return Json(Json::Object {{"error", Json::Object {{"message", message}, {"type", "winshun_local_model"}, {"code", code}}}}).dump();
}

bool loopbackHost(std::string_view host, int port)
{
    const std::string h = lowerAscii(host);
    const std::string p = ":" + std::to_string(port);
    for (const char* name : {"127.0.0.1", "localhost", "[::1]"}) {
        if (h == name || h == name + p)
            return true;
    }
    return false;
}

bool allowedOrigin(std::string_view origin, int port)
{
    const std::string o = lowerAscii(origin);
    const std::string p = ":" + std::to_string(port);
    for (const char* name : {"http://127.0.0.1", "http://localhost", "http://[::1]"}) {
        if (o == name + p)
            return true;
    }
    return false;
}

Relay::Relay(Acquire acquire, Busy busy, Local local) : m_acquire(std::move(acquire)), m_busy(std::move(busy)), m_local(std::move(local)) {}

Relay::~Relay() { close(); }

bool Relay::listen(int port, std::string& error, bool chinese)
{
    close();
    m_closing = false;
    m_chinese = chinese;
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = (chinese ? "网络初始化失败：" : "Sockets failed: ") + std::to_string(::WSAGetLastError());
        return false;
    }
    // No other program may bind the same port on top of it.
    const BOOL exclusive = TRUE;
    ::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // this computer only
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) == SOCKET_ERROR
        || ::listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        const int code = ::WSAGetLastError();
        ::closesocket(listener);
        if (code == WSAEADDRINUSE || code == WSAEACCES)
            error = chinese ? "端口 " + std::to_string(port) + " 被别的程序占着（或被系统保留）：在下面换一个端口"
                            : "Port " + std::to_string(port) + " is taken by another program (or reserved): pick another below";
        else
            error = (chinese ? "开不了本机接口：" : "Could not open the local address: ") + std::to_string(code);
        return false;
    }
    int size = sizeof address;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
    m_port = ntohs(address.sin_port);
    m_listener = listener;
    m_acceptor = std::thread([this] { acceptLoop(); });
    return true;
}

void Relay::close()
{
    m_closing = true;
    if (m_listener != INVALID_SOCKET) {
        ::closesocket(m_listener); // accept() returns
        m_listener = INVALID_SOCKET;
    }
    if (m_acceptor.joinable())
        m_acceptor.join();
    std::list<Connection> connections;
    {
        const std::lock_guard lock(m_mutex);
        for (Connection& c : m_connections) {
            if (!c.done)
                ::shutdown(c.socket, SD_BOTH); // its waits end; the thread closes it
        }
        connections.splice(connections.end(), m_connections);
    }
    for (Connection& c : connections) {
        if (c.thread.joinable())
            c.thread.join();
    }
    m_port = 0;
}

void Relay::acceptLoop()
{
    for (;;) {
        const SOCKET client = ::accept(m_listener, nullptr, nullptr);
        if (m_closing) {
            if (client != INVALID_SOCKET)
                ::closesocket(client);
            return;
        }
        if (client == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const std::lock_guard lock(m_mutex);
        for (auto it = m_connections.begin(); it != m_connections.end();) {
            if (it->done) {
                it->thread.join();
                it = m_connections.erase(it);
            } else {
                ++it;
            }
        }
        if (m_connections.size() >= kMaxConnections) {
            http::SocketStream stream(client);
            http::respond(stream, 503, errorJson(m_chinese ? "同时来的请求太多，稍后再试" : "Too many requests at once", "busy"),
                "application/json", true);
            ::closesocket(client);
            continue;
        }
        Connection& c = m_connections.emplace_back();
        c.socket = client;
        c.thread = std::thread([this, client, &c] {
            serve(client);
            {
                // Marked done while the socket is still open: close() never
                // shuts down a handle that may already belong to another socket.
                const std::lock_guard done(m_mutex);
                c.done = true;
            }
            ::closesocket(client);
        });
    }
}

void Relay::serve(SOCKET client)
{
    const DWORD timeoutMs = 30000; // the request must come within this
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
    http::SocketStream stream(client);
    http::Reader reader(stream);
    const auto refuse = [&](int status, const std::string& message, const std::string& code) {
        http::respond(stream, status, errorJson(message, code), "application/json", true);
    };
    const bool zh = m_chinese;
    // The server stopped (a crash, its memory) before it answered: the card
    // says why a moment later.
    const std::string stopped = zh ? "模型没有回答就停了，看“本地模型”卡片上的原因，再试一次"
                                   : "The model stopped before it answered: see the Local models card, then try again";
    http::Head head;
    if (const http::ReadResult r = reader.head(head, true, kMaxHead); r != http::ReadResult::Ok) {
        if (r != http::ReadResult::Closed)
            refuse(400, "Bad request", "bad_request");
        return;
    }
    // Only this computer's programs, and of web pages only the chat page
    // served from here: another site's page must not use the model (nor a
    // name of its own that resolves to 127.0.0.1).
    if (!loopbackHost(head.header("host"), m_port)) {
        refuse(403, "Only programs on this computer may use it (the Host header)", "forbidden_host");
        return;
    }
    if (const std::string origin = head.header("origin"); !origin.empty() && !allowedOrigin(origin, m_port)) {
        refuse(403, "Web pages of other sites may not use it", "forbidden_origin");
        return;
    }
    std::string body;
    if (const http::ReadResult r = reader.bodyText(head, true, kMaxBody, body); r != http::ReadResult::Ok) {
        refuse(r == http::ReadResult::TooLarge ? 413 : 400, "Bad request body", "bad_request");
        return;
    }
    if (head.method == "OPTIONS") {
        http::respond(stream, 204, {}, {}, true);
        return;
    }
    if (m_local) {
        if (const auto answer = m_local(head.method, head.path)) {
            http::respond(stream, answer->first, answer->second, "application/json", true);
            return;
        }
    }

    m_busy(+1, head.path);
    struct Done {
        const Busy& busy;
        const std::string& path;
        ~Done() { busy(-1, path); }
    } done {m_busy, head.path};
    const Target target = m_acquire(head.method, head.path);
    if (!target.port) {
        refuse(target.status, target.message, target.code);
        return;
    }
    const SOCKET upstream = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(target.port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (upstream == INVALID_SOCKET
        || ::connect(upstream, reinterpret_cast<const sockaddr*>(&address), sizeof address) == SOCKET_ERROR) {
        if (upstream != INVALID_SOCKET)
            ::closesocket(upstream);
        refuse(502, stopped, "upstream");
        return;
    }
    std::string request = head.method + " " + head.target + " HTTP/1.1\r\n";
    for (const auto& [name, value] : head.headers) {
        if (passedOn(name))
            request += name + ": " + value + "\r\n";
    }
    request += "host: 127.0.0.1:" + std::to_string(target.port) + "\r\n";
    if (!body.empty() || head.method == "POST" || head.method == "PUT" || head.method == "PATCH")
        request += "content-length: " + std::to_string(body.size()) + "\r\n";
    request += "connection: close\r\n\r\n";
    request += body;
    if (!sendAll(upstream, request.data(), request.size())) {
        ::closesocket(upstream);
        refuse(502, stopped, "upstream");
        return;
    }
    // Its answer, as it comes. The client going away (closed) ends it: the
    // server sees its connection close and stops generating.
    std::vector<char> buffer(64 * 1024);
    std::size_t forwarded = 0;
    WSAPOLLFD fds[2] {{upstream, POLLRDNORM, 0}, {client, POLLRDNORM, 0}};
    while (!m_closing) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        const int ready = ::WSAPoll(fds, 2, 1000);
        if (ready < 0)
            break;
        if (ready == 0)
            continue;
        if (fds[0].revents & (POLLRDNORM | POLLHUP | POLLERR)) {
            const int got = ::recv(upstream, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (got <= 0) {
                if (forwarded == 0) // nothing of an answer yet: one that says so
                    refuse(502, stopped, "stopped");
                break;
            }
            if (!sendAll(client, buffer.data(), static_cast<std::size_t>(got)))
                break;
            forwarded += static_cast<std::size_t>(got);
        }
        if (fds[1].revents & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL)) {
            const int got = ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (got <= 0)
                break; // gone (anything more it sends is no request of this connection)
        }
    }
    ::closesocket(upstream);
}

} // namespace lm
