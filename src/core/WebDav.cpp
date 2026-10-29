#include "WebDav.h"

#include "Unc.h"
#include "Win32Util.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <evntprov.h>

#include <QString>
#include <QUrl>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ws::webdav {

namespace {

bool isDecimal(std::wstring_view s) noexcept
{
    return !s.empty() && s.size() <= 5 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

// A server in WebClient's form, its name not copied: for isWebDavPath(),
// which allocates nothing.
struct Form {
    std::wstring_view name;
    std::uint16_t port = 80;
    bool tls = false;
};

std::optional<Form> formOf(std::wstring_view host) noexcept
{
    const std::size_t at = host.find(L'@');
    if (at == std::wstring_view::npos || at == 0)
        return std::nullopt;
    Form out {host.substr(0, at)};
    bool portGiven = false;
    std::wstring_view rest = host.substr(at + 1);
    while (true) {
        const std::size_t next = rest.find(L'@');
        const std::wstring_view part = rest.substr(0, next);
        if (win32::equalsIgnoreCase(part, L"SSL") && !out.tls && !portGiven) {
            out.tls = true;
        } else if (isDecimal(part) && !portGiven) {
            unsigned value = 0;
            for (const wchar_t c : part)
                value = value * 10 + static_cast<unsigned>(c - L'0');
            if (value == 0 || value > 65535)
                return std::nullopt;
            out.port = static_cast<std::uint16_t>(value);
            portGiven = true;
        } else {
            return std::nullopt; // "user@host" and the like: not WebClient's form
        }
        if (next == std::wstring_view::npos)
            break;
        rest = rest.substr(next + 1);
    }
    if (!portGiven)
        out.port = out.tls ? 443 : 80;
    return out;
}

} // namespace

std::optional<Host> parseHost(std::wstring_view host)
{
    const std::optional<Form> form = formOf(host);
    if (!form)
        return std::nullopt;
    return Host {std::wstring(form->name), form->port, form->tls};
}

std::wstring displayHost(std::wstring_view host)
{
    const auto h = parseHost(host);
    if (!h)
        return std::wstring(host);
    const bool usual = h->port == (h->tls ? 443 : 80);
    return usual ? h->name : h->name + L':' + std::to_wstring(h->port);
}

bool isWebDavPath(std::wstring_view path) noexcept
{
    if (!unc::isUnc(path))
        return false;
    if (formOf(unc::host(path)))
        return true;
    const std::wstring_view root = unc::shareRoot(path);
    const std::size_t slash = root.rfind(L'\\');
    return slash != std::wstring_view::npos && win32::equalsIgnoreCase(root.substr(slash + 1), L"DavWWWRoot");
}

namespace {

struct WebAddress {
    QUrl url;
    bool tls = false;
    int port = 0;
    UrlProblem problem = UrlProblem::None;
};

WebAddress webAddressOf(std::wstring_view text)
{
    WebAddress a;
    // Tolerant: typed or pasted, an address may have spaces and Chinese unencoded.
    a.url = QUrl(QString::fromWCharArray(text.data(), static_cast<qsizetype>(text.size())).trimmed());
    // "davs://" and "webdavs://": what Linux file managers and some apps show for one.
    const QString scheme = a.url.scheme().toLower();
    a.tls = scheme == u"https" || scheme == u"davs" || scheme == u"webdavs";
    const bool plain = scheme == u"http" || scheme == u"dav" || scheme == u"webdav";
    if (!(a.tls || plain)) {
        a.problem = UrlProblem::Scheme;
        return a;
    }
    a.port = a.url.port(a.tls ? 443 : 80);
    if (!a.url.isValid() || a.url.host().isEmpty() || a.port <= 0 || a.port > 65535)
        a.problem = UrlProblem::Address; // "\\nas@0": no server, and no longer WebClient's form (parseHost)
    else if (a.url.host().contains(u':'))
        a.problem = UrlProblem::Ipv6; // WebClient's UNC form takes an IPv6 address only as an ipv6-literal.net name
    return a;
}

} // namespace

UrlProblem urlProblem(std::wstring_view text)
{
    return webAddressOf(text).problem;
}

std::wstring uncFromUrl(std::wstring_view text)
{
    const WebAddress a = webAddressOf(text);
    if (a.problem != UrlProblem::None)
        return {};
    const QUrl& url = a.url;
    const bool tls = a.tls;
    const int port = a.port;
    std::wstring out = L"\\\\" + url.host(QUrl::FullyDecoded).toStdWString();
    if (tls)
        out += L"@SSL";
    if (port != 443 || !tls)
        out += L'@' + std::to_wstring(port);
    std::wstring rest = url.path(QUrl::FullyDecoded).toStdWString();
    std::replace(rest.begin(), rest.end(), L'/', L'\\');
    while (!rest.empty() && rest.front() == L'\\')
        rest.erase(rest.begin());
    // The root (DavWWWRoot) for the server alone, and before a folder of
    // that name, which would read as the root (unc::canonical).
    if (rest.empty() || win32::equalsIgnoreCase(std::wstring_view(rest).substr(0, rest.find(L'\\')), L"DavWWWRoot"))
        rest.insert(0, rest.empty() ? L"DavWWWRoot" : L"DavWWWRoot\\");
    out += L'\\';
    out += rest;
    return unc::canonical(out);
}

bool rootHolds(std::wstring_view root, std::wstring_view path) noexcept
{
    const std::wstring_view server = unc::host(root);
    if (server.empty() || !formOf(server) || !win32::equalsIgnoreCase(server, unc::host(path)))
        return false;
    return win32::equalsIgnoreCase(root.substr(2 + server.size()), L"\\DavWWWRoot") && !unc::equal(root, path);
}

std::wstring urlOf(std::wstring_view path)
{
    const std::wstring c = unc::canonical(path);
    const std::wstring_view server = unc::host(c);
    const std::optional<Form> form = formOf(server);
    if (!form)
        return {}; // SMB, WSL, or a server not in WebClient's form
    std::wstring out = form->tls ? L"https://" : L"http://";
    out += form->name;
    if (form->port != (form->tls ? 443 : 80))
        out += L':' + std::to_wstring(form->port);
    std::wstring_view rest = std::wstring_view(c).substr(2 + server.size()); // "\dav\docs"
    // DavWWWRoot is WebClient's name for "/": canonical, it is there for the
    // root alone or before a folder of that name, which stays.
    constexpr std::wstring_view kRoot = L"\\DavWWWRoot";
    if (rest.size() >= kRoot.size() && win32::equalsIgnoreCase(rest.substr(0, kRoot.size()), kRoot)
        && (rest.size() == kRoot.size() || rest[kRoot.size()] == L'\\'))
        rest.remove_prefix(kRoot.size());
    if (rest.empty())
        return out + L'/';
    for (const wchar_t ch : rest) {
        switch (ch) {
        case L'\\':
            out += L'/';
            break;
        case L'%': // what uncFromUrl() would read as an escape or a fragment ('?' is in no Windows name)
            out += L"%25";
            break;
        case L'#':
            out += L"%23";
            break;
        default:
            out += ch;
        }
    }
    return out;
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxAddresses = 16; // asked at once: within select()'s 64 sockets

void put16(std::vector<unsigned char>& m, std::size_t value)
{
    m.push_back(static_cast<unsigned char>(value >> 8));
    m.push_back(static_cast<unsigned char>(value));
}

// A TLS 1.2 ClientHello: whatever a TLS server makes of it (a ServerHello,
// or an alert for a version or cipher it will not take), it answers with a
// TLS record. Only the answer counts; no connection is made.
std::vector<unsigned char> clientHello(const std::string& serverName)
{
    std::vector<unsigned char> ext;
    const auto extension = [&](unsigned type, const std::vector<unsigned char>& data) {
        put16(ext, type);
        put16(ext, data.size());
        ext.insert(ext.end(), data.begin(), data.end());
    };
    if (!serverName.empty()) {
        std::vector<unsigned char> sni;
        put16(sni, serverName.size() + 3);
        sni.push_back(0); // host_name
        put16(sni, serverName.size());
        sni.insert(sni.end(), serverName.begin(), serverName.end());
        extension(0x0000, sni);
    }
    extension(0x000A, {0x00, 0x06, 0x00, 0x1D, 0x00, 0x17, 0x00, 0x18}); // x25519, P-256, P-384
    extension(0x000B, {0x01, 0x00}); // uncompressed points
    extension(0x000D, {0x00, 0x0E, 0x04, 0x03, 0x08, 0x04, 0x04, 0x01, 0x05, 0x03, 0x08, 0x05, 0x05, 0x01, 0x02, 0x01});
    static constexpr std::array<unsigned char, 26> kSuites {0xC0, 0x2B, 0xC0, 0x2F, 0xC0, 0x2C, 0xC0, 0x30, 0xCC, 0xA9,
        0xCC, 0xA8, 0xC0, 0x13, 0xC0, 0x14, 0x00, 0x9C, 0x00, 0x9D, 0x00, 0x2F, 0x00, 0x35, 0x00, 0x0A};

    std::vector<unsigned char> body {0x03, 0x03}; // TLS 1.2
    for (int i = 0; i < 32; ++i)
        body.push_back(static_cast<unsigned char>(i * 37 + 11)); // random: nothing is agreed on
    body.push_back(0); // no session id
    put16(body, kSuites.size());
    body.insert(body.end(), kSuites.begin(), kSuites.end());
    body.insert(body.end(), {0x01, 0x00}); // no compression
    put16(body, ext.size());
    body.insert(body.end(), ext.begin(), ext.end());

    std::vector<unsigned char> m {0x16, 0x03, 0x01}; // handshake record
    put16(m, body.size() + 4);
    m.push_back(0x01); // ClientHello
    m.push_back(static_cast<unsigned char>(body.size() >> 16));
    put16(m, body.size() & 0xFFFF);
    m.insert(m.end(), body.begin(), body.end());
    return m;
}

std::vector<unsigned char> optionsRequest(const Host& h, const std::string& name)
{
    std::string request = "OPTIONS / HTTP/1.1\r\nHost: " + name;
    if (h.port != 80)
        request += ':' + std::to_string(h.port);
    request += "\r\nUser-Agent: WinShun\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    return {request.begin(), request.end()};
}

// What `h` is asked: an OPTIONS, or with TLS a ClientHello alone.
std::vector<unsigned char> requestFor(const Host& h)
{
    const std::string name = QString::fromStdWString(h.name).toStdString();
    // No server name for an address: TLS does not send one either.
    in6_addr scratch {};
    const bool literal = ::InetPtonW(AF_INET, h.name.c_str(), &scratch) == 1 || ::InetPtonW(AF_INET6, h.name.c_str(), &scratch) == 1;
    return h.tls ? clientHello(literal ? std::string() : name) : optionsRequest(h, name);
}

// Whether the first bytes back are a server's: "HTTP/", or a TLS record (handshake or alert).
bool isAnswer(const unsigned char* reply, bool tls) noexcept
{
    if (tls)
        return (reply[0] == 0x16 || reply[0] == 0x15) && reply[1] == 0x03 && reply[2] <= 0x04;
    return std::equal(reply, reply + 5, "HTTP/");
}

// The request to one address, from connecting to the first bytes back.
struct Attempt {
    enum class Step : std::uint8_t { Connecting, Sending, Receiving, Done };

    SOCKET s = INVALID_SOCKET;
    Step step = Step::Connecting;
    std::size_t sent = 0;
    unsigned char reply[5] {};
    std::size_t got = 0;
    bool yes = false; // a server's answer came back

    void finish(bool answered)
    {
        yes = answered;
        step = Step::Done;
        if (s != INVALID_SOCKET)
            ::closesocket(s);
        s = INVALID_SOCKET;
    }
};

// Moves `a` on as far as select() found it can go.
void advance(Attempt& a, const std::vector<unsigned char>& request, bool tls, fd_set& reads, fd_set& writes, fd_set& failed)
{
    using Step = Attempt::Step;
    if (a.step == Step::Connecting) {
        if (FD_ISSET(a.s, &failed))
            return a.finish(false); // refused, or no way there
        if (!FD_ISSET(a.s, &writes))
            return;
        a.step = Step::Sending;
    }
    if (a.step == Step::Sending) {
        if (!FD_ISSET(a.s, &writes))
            return;
        while (a.sent < request.size()) {
            const int n = ::send(a.s, reinterpret_cast<const char*>(request.data()) + a.sent,
                static_cast<int>(request.size() - a.sent), 0);
            if (n > 0)
                a.sent += static_cast<std::size_t>(n);
            else if (::WSAGetLastError() == WSAEWOULDBLOCK)
                return;
            else
                return a.finish(false);
        }
        a.step = Step::Receiving;
        return; // the reply comes with a later select()
    }
    if (a.step == Step::Receiving && FD_ISSET(a.s, &reads)) {
        const std::size_t want = tls ? 3 : 5;
        const int n = ::recv(a.s, reinterpret_cast<char*>(a.reply) + a.got, static_cast<int>(want - a.got), 0);
        if (n == 0 || (n < 0 && ::WSAGetLastError() != WSAEWOULDBLOCK))
            return a.finish(false);
        a.got += static_cast<std::size_t>(std::max(n, 0));
        if (a.got == want)
            a.finish(isAnswer(a.reply, tls));
    }
}

struct Address {
    sockaddr_storage storage {};
    int length = 0;
};

// The addresses of `name`, as far as Windows finds them by `deadline`: the
// name of a NAS that is off can take seconds of DNS, LLMNR and NetBIOS, and
// the time limit holds for those too (a drag asks on the UI thread), as for
// share::probe. The lookup goes on alone after, for nobody.
std::vector<Address> addressesBy(const std::wstring& name, std::uint16_t port, Clock::time_point deadline)
{
    struct Lookup {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::vector<Address> found;
    };
    const auto lookup = std::make_shared<Lookup>();
    std::thread([lookup, name, port] {
        std::vector<Address> found;
        ADDRINFOW hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        ADDRINFOW* list = nullptr;
        if (::GetAddrInfoW(name.c_str(), std::to_wstring(port).c_str(), &hints, &list) == 0) {
            for (const ADDRINFOW* a = list; a && found.size() < kMaxAddresses; a = a->ai_next) {
                if (a->ai_addrlen > sizeof(sockaddr_storage))
                    continue;
                Address& to = found.emplace_back();
                std::memcpy(&to.storage, a->ai_addr, a->ai_addrlen);
                to.length = static_cast<int>(a->ai_addrlen);
            }
            ::FreeAddrInfoW(list);
        }
        std::lock_guard lock(lookup->mutex);
        lookup->found = std::move(found);
        lookup->done = true;
        lookup->cv.notify_all();
    }).detach();
    std::unique_lock lock(lookup->mutex);
    if (!lookup->cv.wait_until(lock, deadline, [&] { return lookup->done; }))
        return {};
    return std::move(lookup->found);
}

// Whether any of `addresses` answers `request` as a server (isAnswer) by
// `deadline`. All of them asked at once, as share::probe asks for SMB
// (GitHub #116): one after the other, a first address that never answers
// (an IPv6 address the server does not listen on, one a NAS no longer has,
// and behind a proxy in TUN mode any address) took all the time, and a
// server that was there at the next was called unreachable.
bool race(const std::vector<Address>& addresses, const std::vector<unsigned char>& request, bool tls,
    Clock::time_point deadline)
{
    using Step = Attempt::Step;
    std::vector<Attempt> attempts(addresses.size()); // never moved: no copies of a socket
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        Attempt& a = attempts[i];
        const Address& to = addresses[i];
        a.s = ::socket(to.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
        u_long nonBlocking = 1;
        // No socket (IPv6 turned off, say), or a blocking one (connect() would
        // wait for Windows' own timeout): not this address.
        if (a.s == INVALID_SOCKET || ::ioctlsocket(a.s, FIONBIO, &nonBlocking) != 0) {
            a.finish(false);
            continue;
        }
        if (::connect(a.s, reinterpret_cast<const sockaddr*>(&to.storage), to.length) == 0)
            a.step = Step::Sending;
        else if (::WSAGetLastError() != WSAEWOULDBLOCK)
            a.finish(false);
    }

    bool yes = false;
    while (!yes) {
        const auto now = Clock::now();
        if (now >= deadline)
            break;
        fd_set reads;
        fd_set writes;
        fd_set failed;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_ZERO(&failed);
        bool waiting = false;
        for (const Attempt& a : attempts) {
            if (a.step == Step::Done)
                continue;
            waiting = true;
            if (a.step == Step::Receiving) {
                FD_SET(a.s, &reads);
            } else {
                FD_SET(a.s, &writes);
                if (a.step == Step::Connecting)
                    FD_SET(a.s, &failed);
            }
        }
        if (!waiting)
            break; // every address refused, hung up or answered as no server does
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        timeval wait {static_cast<long>(left.count() / 1'000'000), static_cast<long>(left.count() % 1'000'000)};
        if (::select(0, &reads, &writes, &failed, &wait) < 0)
            break;
        for (Attempt& a : attempts) {
            if (a.step != Step::Done)
                advance(a, request, tls, reads, writes, failed);
            yes = yes || a.yes;
        }
    }
    for (Attempt& a : attempts)
        a.finish(a.yes); // the sockets still open
    return yes;
}

bool winsock()
{
    static const bool started = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

bool ask(const Host& h, std::chrono::milliseconds timeout)
{
    if (!winsock())
        return false;
    const auto deadline = Clock::now() + timeout; // the name looked up within it too
    return race(addressesBy(h.name, h.port, deadline), requestFor(h), h.tls, deadline);
}

} // namespace

bool answersAt(const Host& host, const std::vector<std::wstring>& addresses, std::chrono::milliseconds timeout)
{
    if (!winsock())
        return false;
    std::vector<Address> found;
    for (std::wstring text : addresses) { // a copy: WSAStringToAddressW takes a writable string
        if (found.size() == kMaxAddresses)
            break;
        Address a;
        for (const int family : {AF_INET, AF_INET6}) {
            a.length = sizeof a.storage;
            if (::WSAStringToAddressW(text.data(), family, nullptr, reinterpret_cast<sockaddr*>(&a.storage), &a.length) == 0) {
                found.push_back(a);
                break;
            }
        }
    }
    return race(found, requestFor(host), host.tls, Clock::now() + timeout);
}

bool answers(std::wstring_view host, std::chrono::milliseconds timeout, bool fresh)
{
    const auto h = parseHost(host);
    if (!h)
        return false;
    struct Known {
        bool yes;
        Clock::time_point at;
    };
    struct Memory {
        std::mutex mutex;
        std::map<std::wstring, Known, std::less<>> known;
    };
    static auto* const memory = new Memory; // never destroyed: an asking thread can outlive main() (share::reach)
    auto& [mutex, known] = *memory;
    std::wstring key(host);
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    if (!fresh) {
        std::lock_guard lock(mutex);
        if (const auto it = known.find(key); it != known.end()) {
            const auto age = Clock::now() - it->second.at;
            if (age < (it->second.yes ? std::chrono::seconds(30) : std::chrono::seconds(10)))
                return it->second.yes;
        }
    }
    const bool yes = ask(*h, timeout);
    std::lock_guard lock(mutex);
    known[key] = {yes, Clock::now()};
    return yes;
}

std::uint64_t fileSizeLimit()
{
    static const std::uint64_t limit = [] {
        DWORD value = 0;
        DWORD bytes = sizeof value;
        if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\WebClient\\Parameters",
                L"FileSizeLimitInBytes", RRF_RT_REG_DWORD, nullptr, &value, &bytes)
            == ERROR_SUCCESS)
            return std::uint64_t {value};
        return std::uint64_t {50'000'000}; // Windows' default
    }();
    return limit;
}

int basicAuthLevel()
{
    DWORD value = 0;
    DWORD bytes = sizeof value;
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\WebClient\\Parameters",
            L"BasicAuthLevel", RRF_RT_REG_DWORD, nullptr, &value, &bytes)
        == ERROR_SUCCESS)
        return static_cast<int>(value);
    return 1; // Windows' default
}

bool httpSignInBlocked(std::wstring_view path)
{
    if (!isWebDavPath(path))
        return false;
    // "\\nas\DavWWWRoot" is plain http on 80 too (the root's own form).
    const std::optional<Host> host = parseHost(unc::host(path));
    return !(host && host->tls) && basicAuthLevel() < 2;
}

namespace {

struct Socket {
    SOCKET s = INVALID_SOCKET;
    ~Socket()
    {
        if (s != INVALID_SOCKET)
            ::closesocket(s);
    }
};

// Whether `s` can be written to (`write`) or read from before `deadline`.
bool ready(SOCKET s, bool write, Clock::time_point deadline)
{
    const auto now = Clock::now();
    if (now >= deadline)
        return false;
    fd_set set;
    fd_set failed;
    FD_ZERO(&set);
    FD_ZERO(&failed);
    FD_SET(s, &set);
    FD_SET(s, &failed);
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    timeval wait {static_cast<long>(left.count() / 1'000'000), static_cast<long>(left.count() % 1'000'000)};
    return ::select(0, write ? nullptr : &set, write ? &set : nullptr, &failed, &wait) > 0 && FD_ISSET(s, &set)
        && !FD_ISSET(s, &failed);
}

// The head of the reply to `request` (to the blank line, 16 KB at most) from
// the first of `addresses` that gives one by `deadline`; empty: none did.
std::string replyHead(const std::vector<Address>& addresses, const std::string& request, Clock::time_point deadline)
{
    for (const Address& to : addresses) {
        Socket sock {::socket(to.storage.ss_family, SOCK_STREAM, IPPROTO_TCP)};
        u_long nonBlocking = 1;
        if (sock.s == INVALID_SOCKET || ::ioctlsocket(sock.s, FIONBIO, &nonBlocking) != 0)
            continue;
        if (::connect(sock.s, reinterpret_cast<const sockaddr*>(&to.storage), to.length) != 0
            && (::WSAGetLastError() != WSAEWOULDBLOCK || !ready(sock.s, true, deadline)))
            continue; // refused, or not there in time
        std::size_t sent = 0;
        while (sent < request.size()) {
            const int n = ::send(sock.s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
            if (n > 0)
                sent += static_cast<std::size_t>(n);
            else if (::WSAGetLastError() != WSAEWOULDBLOCK || !ready(sock.s, true, deadline))
                break;
        }
        if (sent < request.size())
            continue;
        std::string head;
        char buffer[2048];
        while (head.find("\r\n\r\n") == std::string::npos && head.size() < 16 * 1024) {
            const int n = ::recv(sock.s, buffer, sizeof buffer, 0);
            if (n > 0)
                head.append(buffer, static_cast<std::size_t>(n));
            else if (n == 0 || ::WSAGetLastError() != WSAEWOULDBLOCK || !ready(sock.s, false, deadline))
                break;
        }
        if (head.starts_with("HTTP/"))
            return head;
    }
    return {};
}

std::string_view trimmed(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

// The ways in a reply's WWW-Authenticate headers offer ("basic", "digest"):
// challenges and their parameters all come between commas (outside quotes),
// a challenge's first word followed by no '='.
std::vector<std::string> schemesOffered(std::string_view head)
{
    constexpr std::string_view kName = "www-authenticate:";
    std::vector<std::string> schemes;
    for (std::size_t at = head.find("\r\n"); at != std::string_view::npos;) {
        const std::size_t next = head.find("\r\n", at + 2);
        const std::string_view line = head.substr(at + 2, next == std::string_view::npos ? next : next - at - 2);
        at = next;
        if (line.size() < kName.size()
            || !std::equal(kName.begin(), kName.end(), line.begin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); }))
            continue;
        const std::string_view value = line.substr(kName.size());
        bool quoted = false;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= value.size(); ++i) {
            if (i < value.size() && value[i] == '"')
                quoted = !quoted;
            if (i < value.size() && (quoted || value[i] != ','))
                continue;
            const std::string_view part = trimmed(value.substr(start, i - start));
            start = i + 1;
            const std::size_t end = part.find_first_of(" \t=");
            if (part.empty() || (end != std::string_view::npos && trimmed(part.substr(end)).starts_with('=')))
                continue; // a parameter of the challenge before
            std::string scheme(part.substr(0, end));
            std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            schemes.push_back(std::move(scheme));
        }
    }
    return schemes;
}

} // namespace

bool wantsClearPassword(std::wstring_view path, std::chrono::milliseconds timeout)
{
    const std::wstring url = urlOf(path);
    const std::optional<Host> h = parseHost(unc::host(unc::canonical(path)));
    if (url.empty() || !h || h->tls || !winsock())
        return false;
    const auto deadline = Clock::now() + timeout;
    // The folder as a request line has it: Chinese and spaces escaped.
    std::string target = QUrl(QString::fromStdWString(url)).path(QUrl::FullyEncoded).toStdString();
    if (target.empty())
        target = "/";
    std::string request = "PROPFIND " + target + " HTTP/1.1\r\nHost: " + QString::fromStdWString(h->name).toStdString();
    if (h->port != 80)
        request += ':' + std::to_string(h->port);
    request += "\r\nUser-Agent: WinShun\r\nDepth: 0\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const std::string head = replyHead(addressesBy(h->name, h->port, deadline), request, deadline);
    const std::size_t space = head.find(' ');
    if (space == std::string::npos || head.compare(space + 1, 3, "401") != 0)
        return false;
    const std::vector<std::string> schemes = schemesOffered(head);
    return !schemes.empty() && std::ranges::all_of(schemes, [](const std::string& s) { return s == "basic"; });
}

bool clientDisabled()
{
    // Asked by the settings page each time it shows the shares: remembered for 30 seconds.
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static Clock::time_point askedAt {};
    static bool remembered = false;
    std::lock_guard lock(mutex);
    if (askedAt != Clock::time_point {} && Clock::now() - askedAt < std::chrono::seconds(30))
        return remembered;
    askedAt = Clock::now();
    remembered = false;
    const SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return false;
    bool& disabled = remembered;
    if (const SC_HANDLE service = ::OpenServiceW(manager, L"WebClient", SERVICE_QUERY_CONFIG)) {
        DWORD needed = 0;
        ::QueryServiceConfigW(service, nullptr, 0, &needed);
        std::vector<std::byte> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (needed > 0 && ::QueryServiceConfigW(service, config, needed, &needed))
            disabled = config->dwStartType == SERVICE_DISABLED;
        ::CloseServiceHandle(service);
    }
    ::CloseServiceHandle(manager);
    return disabled;
}

namespace {

// What starts WebClient (`sc qtriggerinfo WebClient`): an event from this ETW
// provider, which Windows' own WebDAV client writes before it opens a folder.
constexpr GUID kClientTrigger {0x22b6d684, 0xfa63, 0x4578, {0x87, 0xc9, 0xef, 0xfc, 0xbe, 0x66, 0x43, 0xc7}};

struct Client {
    DWORD state = 0; // SERVICE_RUNNING, SERVICE_STOPPED…; 0: no such service, or not asked
    std::optional<GUID> trigger; // the provider whose event starts it; none: nothing does
};

// WebClient as the service manager has it now, and with `trigger` what starts it.
Client clientNow(bool trigger)
{
    Client c;
    const SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return c;
    SC_HANDLE service = trigger ? ::OpenServiceW(manager, L"WebClient", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG) : nullptr;
    const bool config = service != nullptr;
    if (!service)
        service = ::OpenServiceW(manager, L"WebClient", SERVICE_QUERY_STATUS);
    if (service) {
        SERVICE_STATUS status {};
        if (::QueryServiceStatus(service, &status))
            c.state = status.dwCurrentState;
        if (trigger) {
            c.trigger = kClientTrigger; // its triggers not to be read: the one it has had since Windows 7
            DWORD needed = 0;
            if (config)
                ::QueryServiceConfig2W(service, SERVICE_CONFIG_TRIGGER_INFO, nullptr, 0, &needed);
            std::vector<std::byte> buffer(needed);
            if (needed > 0
                && ::QueryServiceConfig2W(service, SERVICE_CONFIG_TRIGGER_INFO, reinterpret_cast<BYTE*>(buffer.data()), needed, &needed)) {
                const auto* info = reinterpret_cast<const SERVICE_TRIGGER_INFO*>(buffer.data());
                c.trigger.reset();
                for (DWORD i = 0; i < info->cTriggers && !c.trigger; ++i) {
                    const SERVICE_TRIGGER& t = info->pTriggers[i];
                    if (t.dwTriggerType == SERVICE_TRIGGER_TYPE_CUSTOM && t.dwAction == SERVICE_TRIGGER_ACTION_SERVICE_START
                        && t.pTriggerSubtype)
                        c.trigger = *t.pTriggerSubtype;
                }
            }
        }
        ::CloseServiceHandle(service);
    }
    ::CloseServiceHandle(manager);
    return c;
}

// The event WebClient's trigger waits for (it asks for no particular one).
void fire(const GUID& provider)
{
    REGHANDLE handle = 0;
    if (::EventRegister(&provider, nullptr, nullptr, &handle) != ERROR_SUCCESS)
        return;
    const EVENT_DESCRIPTOR any {};
    ::EventWrite(handle, &any, 0, nullptr);
    ::EventUnregister(handle);
}

// When a start last failed (mayStart). Never destroyed: a check's thread
// that nothing waits for may start WebClient as Win顺 ends.
struct Starts {
    std::mutex mutex;
    std::optional<Clock::time_point> failedAt;
};
Starts& starts()
{
    static auto* const s = new Starts;
    return *s;
}

} // namespace

bool mayStart(Starter starter, std::optional<Clock::time_point> failedAt, Clock::time_point now) noexcept
{
    return starter == Starter::User || !failedAt || now - *failedAt >= kStartAgain;
}

void startAgain()
{
    std::lock_guard lock(starts().mutex);
    starts().failedAt.reset();
}

bool clientRunning()
{
    return clientNow(false).state == SERVICE_RUNNING;
}

bool startClient(std::chrono::milliseconds wait, std::stop_token stop, Starter starter)
{
    const auto started = [] {
        std::lock_guard lock(starts().mutex);
        starts().failedAt.reset();
        return true;
    };
    Client c = clientNow(false);
    if (c.state == SERVICE_RUNNING)
        return started();
    if (c.state == 0 || clientDisabled())
        return false; // not on this Windows (or not to be asked), or turned off
    {
        std::lock_guard lock(starts().mutex);
        if (!mayStart(starter, starts().failedAt, Clock::now()))
            return false;
    }
    c = clientNow(true);
    if (!c.trigger)
        return false;
    // Started by its trigger in 64-72 ms here (2026-10-28), its first access
    // then 0.1 s. One event at a time, whoever asks: others wait for that start.
    static std::atomic<Clock::rep> firedAt {0};
    const auto until = Clock::now() + wait;
    for (;;) {
        if (c.state == SERVICE_RUNNING)
            return started();
        const auto now = Clock::now();
        if (c.state == SERVICE_STOPPED) {
            Clock::rep last = firedAt.load();
            if (now.time_since_epoch() - Clock::duration(last) >= std::chrono::seconds(1)
                && firedAt.compare_exchange_strong(last, now.time_since_epoch().count()))
                fire(*c.trigger);
        }
        if (stop.stop_requested())
            return false;
        if (now >= until) {
            std::lock_guard lock(starts().mutex);
            starts().failedAt = now;
            return false;
        }
        std::this_thread::sleep_for(std::min<Clock::duration>(std::chrono::milliseconds(100), until - now));
        c.state = clientNow(false).state;
    }
}

} // namespace ws::webdav
