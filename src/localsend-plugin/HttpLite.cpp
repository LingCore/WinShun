#include "HttpLite.h"

#include "winshun.h"

#include <algorithm>
#include <charconv>

namespace localsend::http {

namespace {

std::string lowerAscii(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

// Only digits (a length of "-1", "1e3" or " 12x" is a malformed one).
bool parseDecimal(std::string_view text, std::uint64_t& value)
{
    text = trim(text);
    if (text.empty() || text.size() > 19)
        return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return ec == std::errc() && end == text.data() + text.size();
}

bool parseHexSize(std::string_view text, std::uint64_t& value)
{
    if (const std::size_t semicolon = text.find(';'); semicolon != std::string_view::npos)
        text = text.substr(0, semicolon); // chunk extensions: none we know of
    text = trim(text);
    if (text.empty() || text.size() > 15)
        return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return ec == std::errc() && end == text.data() + text.size();
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string decode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hexValue(text[i + 1]);
            const int low = hexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        out += text[i] == '+' ? ' ' : text[i];
    }
    return out;
}

} // namespace

int SocketStream::receive(char* buffer, int size)
{
    const int got = ::recv(m_socket, buffer, size, 0);
    return got < 0 ? -1 : got;
}

bool SocketStream::sendAll(const char* data, std::size_t size)
{
    while (size > 0) {
        const int sent = ::send(m_socket, data, static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

std::string Head::header(std::string_view name) const
{
    for (const auto& [key, value] : headers) {
        if (key == name)
            return value;
    }
    return {};
}

bool Head::chunked() const
{
    return lowerAscii(header("transfer-encoding")).find("chunked") != std::string::npos;
}

bool Head::close() const
{
    return lowerAscii(header("connection")).find("close") != std::string::npos;
}

bool Reader::fill()
{
    if (m_at > 0 && m_at >= m_buffer.size() / 2) {
        m_buffer.erase(0, m_at);
        m_at = 0;
    }
    char chunk[64 * 1024];
    const int got = m_stream.receive(chunk, sizeof chunk);
    if (got <= 0)
        return false;
    m_buffer.append(chunk, static_cast<std::size_t>(got));
    return true;
}

ReadResult Reader::line(std::string& out, std::size_t maxBytes)
{
    for (;;) {
        const std::size_t at = m_buffer.find("\r\n", m_at);
        if (at != std::string::npos) {
            if (at - m_at > maxBytes)
                return ReadResult::TooLarge;
            out.assign(m_buffer, m_at, at - m_at);
            m_at = at + 2;
            return ReadResult::Ok;
        }
        if (buffered() > maxBytes)
            return ReadResult::TooLarge;
        if (!fill())
            return buffered() == 0 ? ReadResult::Closed : ReadResult::Malformed;
    }
}

ReadResult Reader::head(Head& out, bool request, std::size_t maxBytes)
{
    out = {};
    std::string first;
    // A blank line or two before a request is allowed (RFC 9112, 2.2).
    for (int blanks = 0;; ++blanks) {
        const ReadResult r = line(first, maxBytes);
        if (r != ReadResult::Ok)
            return r;
        if (!first.empty())
            break;
        if (blanks >= 2)
            return ReadResult::Malformed;
    }
    std::size_t used = first.size();
    if (request) {
        const std::size_t a = first.find(' ');
        const std::size_t b = first.rfind(' ');
        if (a == std::string::npos || b == a || first.compare(b + 1, 5, "HTTP/") != 0)
            return ReadResult::Malformed;
        out.method = first.substr(0, a);
        out.target = first.substr(a + 1, b - a - 1);
        const std::size_t q = out.target.find('?');
        out.path = out.target.substr(0, q);
        if (q != std::string::npos)
            out.query = out.target.substr(q + 1);
    } else {
        if (first.compare(0, 5, "HTTP/") != 0)
            return ReadResult::Malformed;
        const std::size_t a = first.find(' ');
        std::uint64_t status = 0;
        if (a == std::string::npos || !parseDecimal(std::string_view(first).substr(a + 1, 3), status) || status < 100 || status > 999)
            return ReadResult::Malformed;
        out.status = static_cast<int>(status);
    }
    for (int count = 0; count < 100; ++count) {
        std::string header;
        const ReadResult r = line(header, maxBytes);
        if (r != ReadResult::Ok)
            return r == ReadResult::Closed ? ReadResult::Malformed : r;
        if (header.empty())
            return ReadResult::Ok;
        used += header.size() + 2;
        if (used > maxBytes)
            return ReadResult::TooLarge;
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos || colon == 0)
            return ReadResult::Malformed;
        out.headers.emplace_back(lowerAscii(std::string_view(header).substr(0, colon)),
            std::string(trim(std::string_view(header).substr(colon + 1))));
    }
    return ReadResult::TooLarge; // a hundred header lines
}

ReadResult Reader::body(const Head& head, bool request, std::uint64_t maxBytes,
    const std::function<bool(const char*, std::size_t)>& sink, std::uint64_t* bytes)
{
    std::uint64_t total = 0;
    const auto done = [&](ReadResult r) {
        if (bytes)
            *bytes = total;
        return r;
    };
    // `n` bytes to the sink; false when the stream ended first (or the sink said stop).
    const auto take = [&](std::uint64_t n, bool& stopped) {
        while (n > 0) {
            if (buffered() == 0 && !fill())
                return false;
            const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(n, buffered()));
            if (!sink(m_buffer.data() + m_at, k)) {
                stopped = true;
                return false;
            }
            m_at += k;
            n -= k;
            total += k;
        }
        return true;
    };
    bool stopped = false;
    if (!request && (head.status / 100 == 1 || head.status == 204 || head.status == 304))
        return done(ReadResult::Ok); // never a body (RFC 9112, 6.3), whatever the head says
    if (head.chunked()) {
        for (;;) {
            std::string sizeLine;
            if (line(sizeLine, 1024) != ReadResult::Ok)
                return done(ReadResult::Malformed);
            std::uint64_t n = 0;
            if (!parseHexSize(sizeLine, n))
                return done(ReadResult::Malformed);
            if (n == 0) {
                std::string trailer;
                for (int count = 0;; ++count) {
                    if (count > 32 || line(trailer, 8 * 1024) != ReadResult::Ok)
                        return done(ReadResult::Malformed);
                    if (trailer.empty())
                        return done(ReadResult::Ok);
                }
            }
            if (n > maxBytes - std::min(maxBytes, total)) {
                // What still fits goes in (an upload checks its own bounds), then no more.
                take(maxBytes - total, stopped);
                return done(ReadResult::TooLarge);
            }
            if (!take(n, stopped))
                return done(ReadResult::Malformed);
            std::string crlf;
            if (line(crlf, 2) != ReadResult::Ok || !crlf.empty())
                return done(ReadResult::Malformed);
        }
    }
    const std::string length = head.header("content-length");
    if (length.empty()) {
        if (request)
            return done(ReadResult::Ok); // a request without a length has no body
        // A response to the end of the connection.
        for (;;) {
            if (buffered() == 0 && !fill())
                return done(ReadResult::Ok);
            const std::size_t k = buffered();
            if (total + k > maxBytes)
                return done(ReadResult::TooLarge);
            if (!sink(m_buffer.data() + m_at, k))
                return done(ReadResult::Malformed);
            m_at += k;
            total += k;
        }
    }
    std::uint64_t n = 0;
    if (!parseDecimal(length, n))
        return done(ReadResult::Malformed);
    if (n > maxBytes)
        return done(ReadResult::TooLarge);
    return done(take(n, stopped) ? ReadResult::Ok : ReadResult::Malformed);
}

ReadResult Reader::bodyText(const Head& head, bool request, std::size_t maxBytes, std::string& out)
{
    out.clear();
    return body(head, request, maxBytes, [&](const char* data, std::size_t size) {
        out.append(data, size);
        return true;
    });
}

std::string reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

bool respond(Stream& stream, int status, std::string_view body, std::string_view contentType, bool close)
{
    std::string text = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) + "\r\n";
    if (!body.empty())
        text += "Content-Type: " + std::string(contentType) + "\r\n";
    if (status != 204) // a 204 has no Content-Length (RFC 9110, 8.6)
        text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    text += close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
    text += body;
    return stream.sendAll(text);
}

std::string messageJson(std::string_view message)
{
    return winshun::Json(winshun::Json::Object {{"message", std::string(message)}}).dump();
}

std::map<std::string, std::string> parseQuery(std::string_view query)
{
    std::map<std::string, std::string> out;
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            out[decode(pair.substr(0, eq))] = eq == std::string_view::npos ? std::string() : decode(pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos)
            break;
        query.remove_prefix(amp + 1);
    }
    return out;
}

std::string percentEncode(std::string_view text)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.' || u == '~') {
            out += c;
        } else {
            out += '%';
            out += kDigits[u >> 4];
            out += kDigits[u & 15];
        }
    }
    return out;
}

} // namespace localsend::http
