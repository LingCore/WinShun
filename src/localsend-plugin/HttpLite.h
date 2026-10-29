#pragma once

// The little HTTP/1.1 LocalSend needs, over TLS or plain TCP: request and
// response heads with limits, bodies with Content-Length or chunked
// (LocalSend's own sender streams files chunked), responses, query strings.
// Called HttpLite so that it is not taken for src/app/platform/Http.*.

#include <winsock2.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace localsend::http {

// Where the bytes come from and go to: TLS (TlsChannel) or a plain socket.
class Stream {
public:
    virtual ~Stream() = default;
    // Up to `size` bytes: 0 at the end, below 0 on errors (a timeout too).
    virtual int receive(char* buffer, int size) = 0;
    virtual bool sendAll(const char* data, std::size_t size) = 0;
    bool sendAll(std::string_view data) { return sendAll(data.data(), data.size()); }
};

class SocketStream final : public Stream {
public:
    explicit SocketStream(SOCKET socket) : m_socket(socket) {}
    int receive(char* buffer, int size) override;
    bool sendAll(const char* data, std::size_t size) override;

private:
    SOCKET m_socket;
};

struct Head {
    std::string method; // a request's
    std::string target; // "/api/localsend/v2/upload?sessionId=…"
    std::string path; // the target up to '?'
    std::string query; // after it
    int status = 0; // a response's
    std::vector<std::pair<std::string, std::string>> headers; // names in lower case
    std::string header(std::string_view name) const; // "" when absent
    bool chunked() const;
    bool close() const; // "Connection: close"
};

enum class ReadResult { Ok, Closed, TooLarge, Malformed };

// What comes on a connection: a head, then its body; with limits.
class Reader {
public:
    explicit Reader(Stream& stream) : m_stream(stream) {}
    ReadResult head(Head& out, bool request, std::size_t maxBytes = 16 * 1024);
    // The body, to `sink` as it comes (false from it: stop, Malformed). More
    // than `maxBytes`: TooLarge, after what fitted went to the sink. A
    // response with neither a length nor chunks runs to the connection's end.
    ReadResult body(const Head& head, bool request, std::uint64_t maxBytes,
        const std::function<bool(const char* data, std::size_t size)>& sink, std::uint64_t* bytes = nullptr);
    ReadResult bodyText(const Head& head, bool request, std::size_t maxBytes, std::string& out);

private:
    bool fill(); // more bytes into the buffer; false at the end or on errors
    ReadResult line(std::string& out, std::size_t maxBytes);
    std::size_t buffered() const noexcept { return m_buffer.size() - m_at; }

    Stream& m_stream;
    std::string m_buffer;
    std::size_t m_at = 0; // read up to here
};

// A response with its body (Content-Length); the connection is kept unless `close`.
bool respond(Stream& stream, int status, std::string_view body = {}, std::string_view contentType = "application/json",
    bool close = false);
std::string reasonPhrase(int status);
// {"message":"…"}: what LocalSend's own server answers errors with.
std::string messageJson(std::string_view message);

std::map<std::string, std::string> parseQuery(std::string_view query); // names and values decoded
std::string percentEncode(std::string_view text); // for a query value

} // namespace localsend::http
