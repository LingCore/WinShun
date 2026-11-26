// The local models' address, http://127.0.0.1:<port>: an OpenAI-compatible
// API (llama-server's own) that is there whenever the plugin runs, while
// llama-server itself runs only when a request needs it. Each request is
// read here, checked (from this computer, not a web page of another site),
// then handed to the server that the acquire callback started, or answered
// with why there is none ({"error": {"message", "type", "code"}}, as
// OpenAI's API says errors). The answer is passed on as it comes, so
// streamed answers stream. One request per connection.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace lm {

// Where a request goes: the port of a server that is up, or why none is.
struct Target {
    int port = 0;
    int status = 503;
    std::string code; // "no_model", "downloading", "load_failed", …
    std::string message; // for the user, in their language
};

// The error body: {"error": {"message": …, "type": "winshun_local_model", "code": …}}.
std::string errorJson(const std::string& message, const std::string& code);

// The Host header names this computer: 127.0.0.1, localhost or [::1], with
// the port or without (a web page on another name that resolves here, DNS
// rebinding, does not).
bool loopbackHost(std::string_view host, int port);
// A browser's Origin that may use it: a page served from this address itself
// (llama-server's chat page). Other sites' pages may not.
bool allowedOrigin(std::string_view origin, int port);

class Relay {
public:
    // Blocks until the server is up (the model loaded) or cannot be.
    using Acquire = std::function<Target(const std::string& method, const std::string& path)>;
    // +1 when a request goes to the server, -1 when its answer is through.
    using Busy = std::function<void(int delta, const std::string& path)>;
    // A request answered without the server ({status, JSON body}): /health, /v1/models.
    using Local = std::function<std::optional<std::pair<int, std::string>>(const std::string& method, const std::string& path)>;

    Relay(Acquire acquire, Busy busy, Local local);
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;
    ~Relay();

    // Starts taking connections on 127.0.0.1:<port> (0: any free port).
    bool listen(int port, std::string& error, bool chinese);
    void close();
    int port() const { return m_port; }

private:
    void acceptLoop();
    void serve(SOCKET client);

    Acquire m_acquire;
    Busy m_busy;
    Local m_local;
    SOCKET m_listener = INVALID_SOCKET;
    int m_port = 0;
    std::thread m_acceptor;
    std::mutex m_mutex; // the connections
    struct Connection {
        SOCKET socket = INVALID_SOCKET;
        std::thread thread;
        bool done = false;
    };
    std::list<Connection> m_connections;
    std::atomic_bool m_closing = false;
    std::atomic_bool m_chinese = true; // the errors a user may read (busy, the server stopped)
};

} // namespace lm
