#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// WebDAV folders as Windows' WebClient opens them (GitHub #114): the server
// in a UNC path carries its port and TLS ("\\nas@SSL@5006\dav\docs",
// "\\192.168.1.5@5244\dav", "\\server\DavWWWRoot" for the server's root,
// which may be written before any folder too: "\\server\DavWWWRoot\dav").
// Mapped drives remember the web address instead ("https://nas:5006/dav/").
namespace ws::webdav {

struct Host {
    std::wstring name; // "nas", "192.168.1.5"
    std::uint16_t port = 80;
    bool tls = false;
    bool operator==(const Host&) const = default;
};

// The server of a UNC path in WebClient's form: "nas@SSL@5006", "nas@5244",
// "nas@SSL". None for an SMB server (no '@').
std::optional<Host> parseHost(std::wstring_view host);
// "nas@SSL@5006" -> "nas:5006", "nas@SSL" -> "nas": what a user calls it.
std::wstring displayHost(std::wstring_view host);
// Whether a UNC path is a WebDAV one: its server in WebClient's form, or the
// server's root ("\\server\DavWWWRoot\...").
bool isWebDavPath(std::wstring_view path) noexcept;

// "https://nas:5006/dav/%E8%B5%84%E6%96%99/" -> "\\nas@SSL@5006\dav\资料";
// a server's root -> "\\nas@SSL\DavWWWRoot". The port is written out for
// plain http on 80 too ("\\nas@80\dav"), so the path tells it is WebDAV
// (unc::canonical). Empty unless `url` is an http(s) address with a host.
std::wstring uncFromUrl(std::wstring_view url);
// Why uncFromUrl() makes nothing of `url`, for the settings' words.
enum class UrlProblem : std::uint8_t {
    None,
    Scheme, // not http(s) (nor dav(s), webdav(s))
    Ipv6, // an IPv6 address: WebClient's UNC form takes none
    Address, // no server in it, a port out of range, not a web address at all
};
UrlProblem urlProblem(std::wstring_view url);
// Whether `root` is a WebDAV server's root ("\\nas@SSL\DavWWWRoot") and
// `path` another folder of that server ("\\nas@SSL\dav"): written without
// the root (unc::canonical), unc::contains() does not see it in there.
// Both canonical.
bool rootHolds(std::wstring_view root, std::wstring_view path) noexcept;
// The web address of a WebDAV folder, as users write one, for showing it:
// "\\nas@SSL@5006\dav\资料" -> "https://nas:5006/dav/资料", the server's root
// "\\nas@80\DavWWWRoot" -> "http://nas/". uncFromUrl() reads it back: "%"
// and "#" are escaped for that, nothing else. Empty unless `path` is a
// WebDAV one in WebClient's form (canonical or not).
std::wstring urlOf(std::wstring_view path);

// Whether a WebDAV server answers on `host` (parseHost) within `timeout`
// (the name's lookup within it too): a reply in HTTP (or, with TLS, a TLS
// record), not a connection alone, which proxies in TUN mode accept for any
// address. Every address of the name asked at once, as share::probe() asks.
// Remembered for a while as that is (shorter when nothing answered);
// `fresh` asks again.
bool answers(std::wstring_view host, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500),
    bool fresh = false);
// answers(), asked of these addresses ("127.0.0.1:8714", "[::1]:443") as if
// the name of `host` had them: all at once, as every address of a name is
// asked; nothing remembered. For tests, which cannot make up a name with
// more than one address.
bool answersAt(const Host& host, const std::vector<std::wstring>& addresses, std::chrono::milliseconds timeout);

// WebClient's limits (HKLM\SYSTEM\CurrentControlSet\Services\WebClient\Parameters):
// a file larger than FileSizeLimitInBytes cannot be opened at all (error 223).
std::uint64_t fileSizeLimit();
// Whether the WebClient service is disabled: WebDAV folders cannot be opened then.
bool clientDisabled();
// WebClient's BasicAuthLevel: 0 no password sent in the clear at all, 1
// (Windows' default, also when it is not set) over https alone, 2 over http
// too. Asked of the registry each time.
int basicAuthLevel();
// Whether `path` is a WebDAV folder at a plain http address on which
// WebClient sends no password in the clear (basicAuthLevel() < 2): signing
// in fails there whatever is typed, with a server that takes only Basic
// (most NAS), and says nothing of why (GitHub #114).
bool httpSignInBlocked(std::wstring_view path);
// Whether the server of `path`, a plain http WebDAV folder, wants a password
// it takes in the clear alone: a PROPFIND of the folder answered 401 with
// Basic as its only way in. WebClient then sends none (httpSignInBlocked)
// and says the folder or the server is not there (67, 53), not why. Asked
// of the server itself within `timeout` (the name's lookup too); false when
// it cannot tell (no answer, https), or a way Windows takes over http is
// offered too (NTLM, Negotiate, Digest).
bool wantsClearPassword(std::wstring_view path, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
// Whether the WebClient service runs, asked of Windows now. Stopped (as it
// is after a restart, until something starts it), Windows opens no WebDAV
// folder, and opening one does not start it: Windows says at once that the
// folder is not there (ERROR_PATH_NOT_FOUND, as of one deleted) or the
// server not found (ERROR_BAD_NETPATH).
bool clientRunning();
// Who wants WebClient started: a share's thread on its own, or the user (a
// file opened, 立即核对). Once a start failed, a share's thread does not try
// again for kStartAgain (each try waits up to a minute, and Windows may
// never start it), unless the network changed meanwhile (startAgain()) or
// the user asked: the user's start always tries.
enum class Starter : std::uint8_t { Background, User };
inline constexpr std::chrono::minutes kStartAgain {10};
// Whether `starter` may try now, the last start having failed at `failedAt`
// (none: none failed since one worked, or since startAgain()).
bool mayStart(Starter starter, std::optional<std::chrono::steady_clock::time_point> failedAt,
    std::chrono::steady_clock::time_point now) noexcept;
// Starts WebClient when it is stopped, as Windows' own WebDAV client does
// (the event its trigger waits for: no elevation needed), and waits up to
// `wait` for it to run, or until `stop`. Whether it runs; false at once when
// it is disabled, missing, nothing would start it, or `starter` may not try
// now (mayStart).
bool startClient(std::chrono::milliseconds wait, std::stop_token stop = {}, Starter starter = Starter::Background);
void startAgain(); // the network changed: a share's thread may try at once

} // namespace ws::webdav
