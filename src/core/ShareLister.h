#pragma once

#include "Crawler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// Reading network shares: folders, whether a server answers at all, and
// what went wrong in words a user understands.
namespace ws::share {

// What stands between us and a share. The order is how bad it is to the
// user, from not at all to a fault on the server.
enum class Problem : std::uint8_t {
    None,
    Offline, // the server does not answer (off, another network, VPN down)
    Dropped, // the connection broke off while reading
    NotFound, // the server answers, the share is not there
    NeedsSignIn, // Windows has no user name and password for it (or wrong ones)
    GuestBlocked, // the server allows guests only, which Windows 11 refuses
    Signing, // the server cannot sign, which Windows 11 requires
    Smb1Only, // an old device that speaks only SMB 1, which Windows no longer does
    Denied, // a folder this user may not read: skipped, not a problem with the share
    Other,
    WslStopped,
    WslNotRegistered,
    WslUnavailable,
};

// `atRoot`: the share itself (or the folder added) could not be opened;
// "access denied" there means signing in, inside it a folder kept from us.
Problem classify(unsigned long error, bool atRoot) noexcept;
// Problems with the server or share as a whole: a pass stops at them.
bool affectsShare(Problem problem) noexcept;
// How a WebDAV server turned Windows away at the folder added, in codes that
// say nothing of a share anywhere else (Problem::Other; GitHub #114, measured
// over http 2026-10-29). Not "online, 0 items", then, but what each is.
enum class DavRefusal : std::uint8_t {
    None,
    // "Not supported" (50): WebClient took the server for none, as it does one
    // that wants a password even for OPTIONS. Perhaps a sign-in: the button,
    // and tried again within minutes rather than half an hour.
    MaybeSignIn,
    // "Cannot access the file" (1920): its 403. Not for this account: the
    // button, for another one; tried again as a sign-in is.
    Forbidden,
    // "Not functioning" (31): its 500. Tried again within minutes.
    ServerError,
};
DavRefusal davRefusal(std::wstring_view path, Problem problem, unsigned long error) noexcept;

// What answers on the SMB port (445) of a server.
enum class Answer : std::uint8_t {
    None, // nothing (off, another network), or not SMB
    Smb2, // a server Windows can use
    Smb1Only, // an old one: Windows 11 no longer connects to it (unless SMB 1 was installed again)
};

// Asks `host` within `timeout`, so an offline server costs this much rather
// than the 20-45 seconds Windows spends on it: all its addresses at once, as
// Windows does (GitHub #116: one after the other, a first address that never
// answered took all the time). Remembered for a while (shorter when nothing
// answered); `fresh` asks again.
Answer probe(std::wstring_view host, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500),
    bool fresh = false);
// probe() without what it remembers, on `port`, every address of each of
// `hosts` asked at once: for tests, with servers faked on 127.0.0.x.
Answer probeAt(std::initializer_list<std::wstring_view> hosts, std::uint16_t port, std::chrono::milliseconds timeout);
// Whether the server `host` answers: an SMB server (probe()), or a WebDAV one
// for a host in WebClient's form ("nas@SSL@5006": webdav::answers()).
bool reachable(std::wstring_view host, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500),
    bool fresh = false);

// Background only: WSL checks its running state without touching its filesystem;
// SMB and WebDAV check their server. Unlike reachable(), this takes the complete
// path, which tells a WebDAV server's root on port 80 ("\\server\DavWWWRoot").
bool reachablePath(std::wstring_view path, bool fresh = false,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));

// Opens the share (or the folder added in it) as File Explorer does: through
// Windows' own connection to the server, which gets there where the probe
// may not (GitHub #116: a NAS slow to answer). 0 when it could, else why not
// (GetLastError); kept for its server, as askWindows() keeps it. Takes as long as
// Windows does to give up on a server, up to a minute: never on the UI thread.
unsigned long openRoot(std::wstring_view path);

// A UNC path on an SMB server: not WSL's, not WebDAV.
bool smbPath(std::wstring_view path) noexcept;

// Asks Windows itself about `path` (its attributes), as File Explorer gets
// there: 0 when it is there, else why not (GetLastError). On an SMB server,
// what it says is kept for answersNow() and lastHeard(), and one is asked
// at a time (OneAtATime: an ask under way answers for the next when the
// server did not answer it, and it began after the next was asked). As slow
// as openRoot(): never on the UI thread.
unsigned long askWindows(std::wstring_view path);

// Whether the server of `path` answers, for a thread the user waits on (a
// folder typed, listed by the search): the probe (reachablePath); but on a
// server Windows itself got to (askWindows, openRoot: GitHub #116, the probe
// can be wrong there), Windows asked about `path` afresh, for at most
// `windowsWait` (no answer in time is a no, and the probe decides again
// until Windows gets there). `fresh` false takes an answer of the last
// seconds. Up to a second and a half: not on the UI thread. True for a local
// path; WSL and WebDAV as reachablePath().
bool answersNow(std::wstring_view path, bool fresh = true,
    std::chrono::milliseconds windowsWait = std::chrono::milliseconds(1500));

// The last word on the server of `path`, when it came within `within`: the
// probe, reachablePath() asked afresh, Windows (askWindows, openRoot; its
// yes outranks a later no from the probe, see Hearsay). For the UI thread,
// which cannot wait (a drag starts at once): a server that just did not
// answer is not asked again at each try. Nothing lately: nullopt. Never
// touches the network. True for a local path; nullopt for WSL. Windows'
// yes stands for `windowsYes` when given (longer: the probe can say no
// where Windows gets there, GitHub #116).
std::optional<bool> lastHeard(std::wstring_view path, std::chrono::milliseconds within,
    std::optional<std::chrono::milliseconds> windowsYes = std::nullopt);

// The folders `host` shares (NetShareEnum): disk shares only, without the
// hidden ones ("C$"). Slow when the server is: call after reachable().
// `error` is why the server refused, where NetShareEnum does not say (it
// calls a guest refused "not found").
std::vector<std::wstring> listShares(std::wstring_view host, unsigned long* error = nullptr);

// Reads folders on a share for Crawler::sync, on any number of threads.
// File names are filtered by `excludedFiles` ("._*", ".DS_Store": what Macs
// leave on a NAS); folders by the crawler's rules. Three problems with the
// share in a row (or one at the folder added) stop the pass: the rest would
// each wait for Windows to time out.
class Lister {
public:
    struct Options {
        std::vector<std::wstring> excludedFiles; // a name, or "prefix*"
        bool shortNames = false; // FileIdBothDirectoryInfo instead of FileFullDirectoryInfo (for wsbench)
        std::size_t bufferBytes = 64 * 1024;
        std::atomic<std::size_t>* progress = nullptr; // counts the items read, when set
    };
    Lister(const Crawler& crawler, std::wstring root, Options options, std::stop_source stopPass);

    bool operator()(const Crawler::Root& dir, DirListing& out);

    // What happened, once the pass is over (or while it runs, roughly).
    unsigned long firstError() const noexcept { return m_firstError.load(); } // of the share as a whole; 0: none
    Problem problem() const noexcept;
    bool stoppedPass() const noexcept { return m_stopped.load(); }
    std::size_t items() const noexcept { return m_items.load(std::memory_order_relaxed); }
    std::size_t deniedFolders() const noexcept { return m_denied.load(std::memory_order_relaxed); }

private:
    // With FindFirstFile: for WSL and WebDAV (`wsl` false), whose providers
    // need not implement the SMB directory information classes.
    bool listWithFind(const Crawler::Root& dir, DirListing& out, bool wsl);
    bool fail(unsigned long error, bool atRoot);
    bool excludedFile(std::wstring_view name) const noexcept;

    const Crawler& m_crawler;
    std::wstring m_root;
    Options m_options;
    std::stop_source m_stopPass;
    std::atomic<unsigned long> m_firstError {0};
    std::atomic<bool> m_rootFailed {false};
    std::atomic<bool> m_stopped {false};
    std::atomic<int> m_inARow {0};
    std::atomic<std::size_t> m_items {0};
    std::atomic<std::size_t> m_denied {0};
};

} // namespace ws::share
