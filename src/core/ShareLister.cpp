#include "ShareLister.h"

#include "ShareReach.h"
#include "Unc.h"
#include "WebDav.h"
#include "Win32Util.h"
#include "Wsl.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <lm.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace ws::share {

namespace {

// HRESULT-style codes some SMB failures come back as (see RtlNtStatusToDosError).
constexpr unsigned long kBadSignature = 0x8009'0006; // STATUS_INVALID_SIGNATURE
constexpr unsigned long kSignInRequired = 0xC05D'0003; // STATUS_SMB_GUEST_LOGON_BLOCKED_SIGNING_REQUIRED
constexpr unsigned long kGuestNoEncryption = 0xC05D'0004; // STATUS_SMB_GUEST_ENCRYPTION_NOT_SUPPORTED

constexpr int kInARow = 3; // problems with the share before a pass gives up

} // namespace

Problem classify(unsigned long error, bool atRoot) noexcept
{
    switch (error) {
    case NO_ERROR:
        return Problem::None;
    case ERROR_BAD_NETPATH:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE:
    case ERROR_PORT_UNREACHABLE:
    case ERROR_SEM_TIMEOUT:
    case ERROR_NO_NETWORK:
    case ERROR_REM_NOT_LIST:
    case ERROR_BAD_NET_RESP:
    case ERROR_NETWORK_BUSY:
    case ERROR_CONNECTION_REFUSED:
        return Problem::Offline;
    case ERROR_NETNAME_DELETED:
    case ERROR_UNEXP_NET_ERR:
    case ERROR_CONNECTION_ABORTED:
    case ERROR_CONNECTION_INVALID:
    case ERROR_GRACEFUL_DISCONNECT:
    case ERROR_VC_DISCONNECTED:
    case ERROR_DEV_NOT_EXIST:
        return Problem::Dropped;
    case ERROR_BAD_NET_NAME:
        return Problem::NotFound;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return atRoot ? Problem::NotFound : Problem::Other; // a folder deleted since its parent was read
    case ERROR_ACCESS_DENIED:
        return atRoot ? Problem::NeedsSignIn : Problem::Denied;
    case ERROR_LOGON_FAILURE:
    case ERROR_INVALID_PASSWORD:
    case ERROR_ACCOUNT_RESTRICTION:
    case ERROR_PASSWORD_EXPIRED:
    case ERROR_ACCOUNT_DISABLED:
    case ERROR_ACCOUNT_LOCKED_OUT:
    case ERROR_SESSION_CREDENTIAL_CONFLICT:
    case ERROR_NOT_AUTHENTICATED:
    case kSignInRequired:
        return Problem::NeedsSignIn;
    case ERROR_SMB_GUEST_LOGON_BLOCKED:
    case kGuestNoEncryption:
        return Problem::GuestBlocked;
    case kBadSignature:
        return Problem::Signing;
    case ERROR_SMB1_NOT_AVAILABLE:
        return Problem::Smb1Only;
    default:
        return Problem::Other;
    }
}

bool affectsShare(Problem problem) noexcept
{
    return problem != Problem::None && problem != Problem::Denied && problem != Problem::Other;
}

DavRefusal davRefusal(std::wstring_view path, Problem problem, unsigned long error) noexcept
{
    if (problem != Problem::Other || !webdav::isWebDavPath(path))
        return DavRefusal::None;
    switch (error) {
    case ERROR_NOT_SUPPORTED:
        return DavRefusal::MaybeSignIn;
    case ERROR_CANT_ACCESS_FILE:
        return DavRefusal::Forbidden;
    case ERROR_GEN_FAILURE:
        return DavRefusal::ServerError;
    default:
        return DavRefusal::None;
    }
}

namespace {

using Clock = std::chrono::steady_clock;

// The last word on each server, for lastHeard(): by whom reachablePath()
// asks (serverOf).
Hearsay& hearsay()
{
    static auto* const h = new Hearsay; // never destroyed: a probe's thread can outlive main()
    return *h;
}

void heard(std::wstring_view server, bool yes)
{
    hearsay().probeSaid(server, yes);
}

// Whom reachablePath() asks about `path`: its host; "server@80" for
// WebDAV's "\\server\DavWWWRoot", not the SMB server of that name.
std::wstring serverOf(std::wstring_view path)
{
    const std::wstring_view host = unc::host(path);
    if (!webdav::parseHost(host) && webdav::isWebDavPath(path))
        return std::wstring(host) + L"@80";
    return std::wstring(host);
}

constexpr std::size_t kMaxAddresses = 16; // asked at once, two sockets each at most: within select()'s 64

// The opening request of a connection (NEGOTIATE), behind its NetBIOS
// session header. What Windows sends first is SMB 1 framing that offers
// SMB 1 ("NT LM 0.12") and SMB 2 ("SMB 2.002", "SMB 2.???"): a server that
// speaks SMB 2 answers in SMB 2, an old one in SMB 1. `smb2` is an SMB 2
// NEGOTIATE instead (dialects 2.0.2 to 3.0.2), for servers that close on
// SMB 1 framing, or keep quiet to it.
std::vector<unsigned char> negotiateRequest(bool smb2)
{
    std::vector<unsigned char> m(4); // NetBIOS: type 0, length (24 bits, big-endian) set below
    if (smb2) {
        m.resize(4 + 64 + 36 + 8);
        unsigned char* header = m.data() + 4;
        header[0] = 0xFE; // "\xFESMB"
        header[1] = 'S';
        header[2] = 'M';
        header[3] = 'B';
        header[4] = 64; // StructureSize; Command 0 = NEGOTIATE
        header[14] = 1; // CreditRequest
        unsigned char* negotiate = header + 64;
        negotiate[0] = 36; // StructureSize
        negotiate[2] = 4; // DialectCount
        negotiate[4] = 1; // SecurityMode: signing enabled
        const std::uint16_t dialects[] = {0x0202, 0x0210, 0x0300, 0x0302};
        for (int i = 0; i < 4; ++i) {
            negotiate[36 + 2 * i] = static_cast<unsigned char>(dialects[i] & 0xFF);
            negotiate[37 + 2 * i] = static_cast<unsigned char>(dialects[i] >> 8);
        }
    } else {
        m.resize(4 + 32 + 3);
        unsigned char* header = m.data() + 4;
        header[0] = 0xFF; // "\xFFSMB"
        header[1] = 'S';
        header[2] = 'M';
        header[3] = 'B';
        header[4] = 0x72; // Command: NEGOTIATE
        header[9] = 0x18; // Flags: case-insensitive, canonical paths
        header[10] = 0x01; // Flags2 0xC801: long names, extended security, NT status, Unicode
        header[11] = 0xC8;
        // WordCount 0, then ByteCount and the dialects, each 0x02 "name" NUL.
        for (const std::string_view dialect : {"NT LM 0.12", "SMB 2.002", "SMB 2.???"}) {
            m.push_back(0x02);
            m.insert(m.end(), dialect.begin(), dialect.end());
            m.push_back(0);
        }
        const std::size_t bytes = m.size() - (4 + 32 + 3);
        m[4 + 32 + 1] = static_cast<unsigned char>(bytes & 0xFF);
        m[4 + 32 + 2] = static_cast<unsigned char>(bytes >> 8);
    }
    const std::size_t length = m.size() - 4;
    m[1] = static_cast<unsigned char>(length >> 16);
    m[2] = static_cast<unsigned char>(length >> 8);
    m[3] = static_cast<unsigned char>(length);
    return m;
}

enum class Reply { None, Closed, Smb2, Smb1 };

// What a server's first 8 bytes say: SMB 2 or SMB 1 behind the NetBIOS
// session header, or something else altogether.
Reply replyOf(const unsigned char (&reply)[8])
{
    if (reply[5] != 'S' || reply[6] != 'M' || reply[7] != 'B')
        return Reply::None;
    return reply[4] == 0xFE ? Reply::Smb2 : reply[4] == 0xFF ? Reply::Smb1 : Reply::None;
}

struct Address {
    sockaddr_storage storage {};
    int length = 0;
};

// The addresses of `host` (at most kMaxAddresses), in the order Windows gives them.
std::vector<Address> addressesOf(std::wstring_view host, std::uint16_t port)
{
    std::vector<Address> out;
    ADDRINFOW hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* found = nullptr;
    if (::GetAddrInfoW(std::wstring(host).c_str(), std::to_wstring(port).c_str(), &hints, &found) != 0)
        return out;
    for (const ADDRINFOW* a = found; a && out.size() < kMaxAddresses; a = a->ai_next) {
        if (a->ai_addrlen > sizeof(sockaddr_storage))
            continue;
        Address& to = out.emplace_back();
        std::memcpy(&to.storage, a->ai_addr, a->ai_addrlen);
        to.length = static_cast<int>(a->ai_addrlen);
    }
    ::FreeAddrInfoW(found);
    return out;
}

// addressesOf(), as far as Windows finds them by `deadline`: the name of a
// NAS that is off can take seconds of DNS, LLMNR and NetBIOS, and the
// probe's time limit holds for those too (a drag asks on the UI thread).
// The lookup goes on alone after, for nobody.
std::vector<Address> addressesBy(std::wstring_view host, std::uint16_t port, Clock::time_point deadline)
{
    struct Lookup {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::vector<Address> found;
    };
    const auto lookup = std::make_shared<Lookup>();
    std::thread([lookup, name = std::wstring(host), port] {
        std::vector<Address> found = addressesOf(name, port);
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

// One NEGOTIATE to one address, from connecting to the reply.
struct Attempt {
    enum class Step : std::uint8_t { Connecting, Sending, Receiving, Done };

    std::size_t address = 0;
    bool smb2 = false; // SMB 2 framing
    SOCKET s = INVALID_SOCKET;
    Step step = Step::Connecting;
    std::vector<unsigned char> request;
    std::size_t sent = 0;
    unsigned char reply[8] {};
    std::size_t got = 0;
    Clock::time_point sentAt {};
    Reply result = Reply::None;

    void finish(Reply r)
    {
        result = r;
        step = Step::Done;
        if (s != INVALID_SOCKET)
            ::closesocket(s);
        s = INVALID_SOCKET;
    }
};

// Moves `a` on as far as select() found it can go.
void advance(Attempt& a, fd_set& reads, fd_set& writes, fd_set& failed)
{
    using Step = Attempt::Step;
    if (a.step == Step::Connecting) {
        if (FD_ISSET(a.s, &failed))
            return a.finish(Reply::None); // refused, or no way there
        if (!FD_ISSET(a.s, &writes))
            return;
        a.step = Step::Sending;
    }
    if (a.step == Step::Sending) {
        if (!FD_ISSET(a.s, &writes))
            return;
        while (a.sent < a.request.size()) {
            const int n = ::send(a.s, reinterpret_cast<const char*>(a.request.data()) + a.sent,
                static_cast<int>(a.request.size() - a.sent), 0);
            if (n > 0)
                a.sent += static_cast<std::size_t>(n);
            else if (::WSAGetLastError() == WSAEWOULDBLOCK)
                return;
            else
                return a.finish(Reply::Closed);
        }
        a.step = Step::Receiving;
        a.sentAt = Clock::now();
        return; // its reply comes with a later select()
    }
    if (a.step == Step::Receiving && FD_ISSET(a.s, &reads)) {
        const int n = ::recv(a.s, reinterpret_cast<char*>(a.reply) + a.got, static_cast<int>(sizeof a.reply - a.got), 0);
        if (n == 0 || (n < 0 && ::WSAGetLastError() != WSAEWOULDBLOCK))
            return a.finish(Reply::Closed);
        a.got += static_cast<std::size_t>(std::max(n, 0));
        if (a.got == sizeof a.reply)
            a.finish(replyOf(a.reply));
    }
}

// What the first of `addresses` to answer replies to a NEGOTIATE, within
// `timeout`. Asked as Windows asks first (SMB 1 framing offering SMB 2); in
// SMB 2 too where that is hung up on, or met with silence for a moment.
//
// A connection alone proves nothing: proxies in TUN mode (Clash and the
// like) and some VPNs accept every connection themselves and only then find
// the server is not there. So each address is asked in full, all of them at
// once, as Windows' own SMB client does: asked one after the other, a first
// address that never answers (one a NAS no longer has after a restart, an
// IPv6 address it does not listen on, and behind such a proxy any address)
// took all the time, and the next was never asked (GitHub #116).
Answer race(const std::vector<Address>& addresses, std::chrono::milliseconds timeout)
{
    using Step = Attempt::Step;
    const auto deadline = Clock::now() + timeout;
    const Clock::duration quiet = std::min<Clock::duration>(std::chrono::milliseconds(500), timeout / 3);
    std::vector<Attempt> attempts;
    attempts.reserve(2 * addresses.size()); // never moved: no copies of a socket
    std::vector<bool> inSmb2(addresses.size(), false);
    const auto start = [&](std::size_t address, bool smb2) {
        Attempt& a = attempts.emplace_back();
        a.address = address;
        a.smb2 = smb2;
        a.request = negotiateRequest(smb2);
        const Address& to = addresses[address];
        a.s = ::socket(to.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
        u_long nonBlocking = 1;
        if (a.s == INVALID_SOCKET || ::ioctlsocket(a.s, FIONBIO, &nonBlocking) != 0)
            return a.finish(Reply::None); // blocking, connect() would wait for Windows' own timeout
        if (::connect(a.s, reinterpret_cast<const sockaddr*>(&to.storage), to.length) == 0)
            a.step = Step::Sending;
        else if (::WSAGetLastError() != WSAEWOULDBLOCK)
            a.finish(Reply::None);
    };
    for (std::size_t i = 0; i < addresses.size(); ++i)
        start(i, false);

    Reply best = Reply::None;
    while (best == Reply::None) {
        const auto now = Clock::now();
        if (now >= deadline)
            break;
        for (std::size_t i = 0, n = attempts.size(); i < n; ++i) {
            const Attempt& a = attempts[i];
            if (!a.smb2 && !inSmb2[a.address]
                && (a.result == Reply::Closed || (a.step == Step::Receiving && now - a.sentAt >= quiet))) {
                inSmb2[a.address] = true;
                start(a.address, true);
            }
        }
        fd_set reads;
        fd_set writes;
        fd_set failed;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_ZERO(&failed);
        auto wake = deadline;
        bool waiting = false;
        for (const Attempt& a : attempts) {
            if (a.step == Step::Done)
                continue;
            waiting = true;
            if (a.step == Step::Receiving) {
                FD_SET(a.s, &reads);
                if (!a.smb2 && !inSmb2[a.address])
                    wake = std::min(wake, a.sentAt + quiet);
            } else {
                FD_SET(a.s, &writes);
                if (a.step == Step::Connecting)
                    FD_SET(a.s, &failed);
            }
        }
        if (!waiting)
            break; // every address refused, hung up or is not SMB
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(std::max(wake - now, Clock::duration::zero()));
        timeval wait {static_cast<long>(left.count() / 1'000'000), static_cast<long>(left.count() % 1'000'000)};
        if (::select(0, &reads, &writes, &failed, &wait) < 0)
            break;
        for (Attempt& a : attempts) {
            if (a.step != Step::Done)
                advance(a, reads, writes, failed);
            if (a.result == Reply::Smb2 || (a.result == Reply::Smb1 && best == Reply::None))
                best = a.result;
        }
    }
    for (Attempt& a : attempts)
        a.finish(a.result);
    return best == Reply::Smb2 ? Answer::Smb2 : best == Reply::Smb1 ? Answer::Smb1Only : Answer::None;
}

bool winsock()
{
    static const bool started = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

} // namespace

Answer probeAt(std::initializer_list<std::wstring_view> hosts, std::uint16_t port, std::chrono::milliseconds timeout)
{
    if (!winsock())
        return Answer::None;
    const auto deadline = Clock::now() + timeout; // the names looked up within it too
    std::vector<Address> addresses;
    for (const std::wstring_view host : hosts) {
        for (const Address& a : addressesBy(host, port, deadline)) {
            if (addresses.size() < kMaxAddresses)
                addresses.push_back(a);
        }
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return addresses.empty() ? Answer::None : race(addresses, std::max(left, std::chrono::milliseconds::zero()));
}

Answer probe(std::wstring_view host, std::chrono::milliseconds timeout, bool fresh)
{
    struct Known {
        Answer answer;
        Clock::time_point at;
    };
    struct Memory {
        std::mutex mutex;
        std::map<std::wstring, Known, std::less<>> known;
    };
    static auto* const memory = new Memory; // never destroyed: a probe's thread can outlive main()
    auto& [mutex, known] = *memory;
    std::wstring key(host);
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    if (!fresh) {
        std::lock_guard lock(mutex);
        if (const auto it = known.find(key); it != known.end()) {
            const auto age = Clock::now() - it->second.at;
            if (age < (it->second.answer != Answer::None ? std::chrono::seconds(30) : std::chrono::seconds(10)))
                return it->second.answer;
        }
    }
    const Answer answer = probeAt({key}, 445, timeout);
    heard(key, answer != Answer::None);
    std::lock_guard lock(mutex);
    known[key] = {answer, Clock::now()};
    return answer;
}

std::vector<std::wstring> listShares(std::wstring_view host, unsigned long* error)
{
    std::vector<std::wstring> shares;
    const std::wstring server(host);
    DWORD_PTR resume = 0;
    NET_API_STATUS status = 0;
    do {
        LPBYTE buffer = nullptr;
        DWORD read = 0;
        DWORD total = 0;
        status = ::NetShareEnum(const_cast<LPWSTR>(server.c_str()), 1, &buffer, MAX_PREFERRED_LENGTH, &read, &total,
            reinterpret_cast<LPDWORD>(&resume));
        if (status == NERR_Success || status == ERROR_MORE_DATA) {
            const auto* info = reinterpret_cast<const SHARE_INFO_1*>(buffer);
            for (DWORD i = 0; i < read; ++i) {
                const std::wstring_view name(info[i].shi1_netname);
                if ((info[i].shi1_type & STYPE_MASK) == STYPE_DISKTREE && !(info[i].shi1_type & STYPE_SPECIAL)
                    && !name.ends_with(L'$'))
                    shares.emplace_back(name);
            }
        }
        if (buffer)
            ::NetApiBufferFree(buffer);
    } while (status == ERROR_MORE_DATA);
    if (status != NERR_Success && classify(status, true) != Problem::NeedsSignIn) {
        // NetShareEnum calls a guest refused "not found" (53); the server's
        // IPC$ tells why, as opening a share would (1272, 1326, ...).
        const std::wstring ipc = L"\\\\" + server + L"\\IPC$";
        if (::GetFileAttributesW(ipc.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const unsigned long why = ::GetLastError();
            const Problem p = classify(why, true);
            if (p != Problem::Offline && p != Problem::Dropped && p != Problem::Other)
                status = why;
            else if (probe(host) == Answer::Smb1Only)
                status = ERROR_SMB1_NOT_AVAILABLE;
        }
    }
    if (error)
        *error = status == NERR_Success ? 0 : status;
    std::sort(shares.begin(), shares.end(),
        [](const std::wstring& a, const std::wstring& b) { return ::lstrcmpiW(a.c_str(), b.c_str()) < 0; });
    return shares;
}

Lister::Lister(const Crawler& crawler, std::wstring root, Options options, std::stop_source stopPass)
    : m_crawler(crawler)
    , m_root(std::move(root))
    , m_options(std::move(options))
    , m_stopPass(std::move(stopPass))
{
}

Problem Lister::problem() const noexcept
{
    return classify(m_firstError.load(), m_rootFailed.load());
}

bool Lister::excludedFile(std::wstring_view name) const noexcept
{
    for (const std::wstring& pattern : m_options.excludedFiles) {
        if (pattern.ends_with(L'*')) {
            const std::wstring_view prefix = std::wstring_view(pattern).substr(0, pattern.size() - 1);
            if (name.size() >= prefix.size() && win32::equalsIgnoreCase(name.substr(0, prefix.size()), prefix))
                return true;
        } else if (win32::equalsIgnoreCase(name, pattern)) {
            return true;
        }
    }
    return false;
}

bool Lister::fail(unsigned long error, bool atRoot)
{
    const Problem p = classify(error, atRoot);
    if (p == Problem::Denied) {
        m_denied.fetch_add(1, std::memory_order_relaxed); // as in Explorer: it is not there for this user
        return false;
    }
    // At a WebDAV root, these (davRefusal) are the share not there for
    // Windows, not a folder to leave as it was ("online, 0 items").
    const bool davRefused = atRoot && davRefusal(m_root, p, error) != DavRefusal::None;
    if (!affectsShare(p) && !davRefused)
        return false; // that folder stays as it was in the index
    unsigned long none = 0;
    if (m_firstError.compare_exchange_strong(none, error))
        m_rootFailed = atRoot;
    if (atRoot || m_inARow.fetch_add(1) + 1 >= kInARow) {
        m_stopped = true;
        m_stopPass.request_stop();
    }
    return false;
}

bool reachable(std::wstring_view host, std::chrono::milliseconds timeout, bool fresh)
{
    if (webdav::parseHost(host)) {
        const bool yes = webdav::answers(host, timeout, fresh);
        if (fresh) // else perhaps what it remembers: not news (the probe tells its own)
            heard(host, yes);
        return yes;
    }
    return probe(host, timeout, fresh) != Answer::None;
}

bool reachablePath(std::wstring_view path, bool fresh, std::chrono::milliseconds timeout)
{
    if (!unc::wslDistribution(path).empty())
        return wsl::probe(path, fresh) == wsl::State::Running;
    if (!unc::isUnc(path))
        return true;
    const std::wstring_view host = unc::host(path);
    if (!webdav::parseHost(host) && webdav::isWebDavPath(path)) {
        const std::wstring server = serverOf(path); // "\\server\DavWWWRoot": "server@80"
        const bool yes = webdav::answers(server, timeout, fresh);
        if (fresh)
            heard(server, yes);
        return yes;
    }
    return reachable(host, timeout, fresh);
}

std::optional<bool> lastHeard(std::wstring_view path, std::chrono::milliseconds within,
    std::optional<std::chrono::milliseconds> windowsYes)
{
    if (!unc::isUnc(path))
        return true;
    if (!unc::wslDistribution(path).empty())
        return std::nullopt;
    return hearsay().last(serverOf(path), within, windowsYes);
}

namespace {

WindowsMemory& windowsMemory()
{
    static auto* const memory = new WindowsMemory; // never destroyed: an ask still under way at exit uses it
    return *memory;
}

// What Windows says of `path`: 0 when it is there, else why not.
unsigned long attributesError(const std::wstring& path)
{
    return ::GetFileAttributesW(win32::longPath(path).c_str()) != INVALID_FILE_ATTRIBUTES ? 0 : ::GetLastError();
}

// What Windows said of something on `host` (0, or why not), for whoever asks next.
void windowsSaid(std::wstring_view host, unsigned long error)
{
    windowsMemory().said(host, error); // the launcher's quick checks take its word too
    hearsay().windowsSaid(host, serverAnswered(error));
}

// askWindows() one at a time for a server.
OneAtATime& asking()
{
    static auto* const a = new OneAtATime; // never destroyed: an ask still under way at exit uses it
    return *a;
}

} // namespace

unsigned long openRoot(std::wstring_view path)
{
    // As Lister opens it: "\\?\UNC\host\share\" is the share itself.
    const std::wstring root = win32::longPath(path) + L'\\';
    const win32::UniqueHandle h(::CreateFileW(root.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    const unsigned long error = h.valid() ? 0 : ::GetLastError();
    if (smbPath(path))
        windowsSaid(unc::host(path), error);
    return error;
}

bool smbPath(std::wstring_view path) noexcept
{
    return unc::isUnc(path) && unc::wslDistribution(path).empty() && !webdav::isWebDavPath(path);
}

unsigned long askWindows(std::wstring_view path)
{
    const std::wstring file(path);
    if (!smbPath(path))
        return attributesError(file);
    const std::wstring_view host = unc::host(path);
    return asking().ask(host, [&] {
        const unsigned long error = attributesError(file);
        windowsSaid(host, error);
        return error;
    });
}

bool answersNow(std::wstring_view path, bool fresh, std::chrono::milliseconds windowsWait)
{
    if (smbPath(path)) {
        const std::wstring file(path);
        const auto ask = [file] { return askWindows(file); }; // one at a time for the server, and heard
        if (const std::optional<bool> windows = windowsMemory().quick(unc::host(path), fresh, windowsWait, ask))
            return *windows;
    }
    return reachablePath(path, fresh);
}

bool Lister::listWithFind(const Crawler::Root& dir, DirListing& out, bool wsl)
{
    // The 9P provider need not implement the SMB directory information classes,
    // and WebDAV's counts the closing null in the name's length ("a.txt\0", ".\0").
    // FindFirstFile also supplies reparse tags, so Linux symlink loops are not followed.
    const bool atRoot = unc::equal(dir.path, m_root);
    WIN32_FIND_DATAW data {};
    const win32::UniqueFind find(::FindFirstFileExW((win32::longPath(dir.path) + L"\\*").c_str(),
        FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0));
    if (!find.valid()) {
        const auto error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || fail(error, atRoot);
    }
    do {
        if (m_stopPass.stop_requested())
            return false;
        const std::wstring_view name(data.cFileName);
        const bool isDir = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        if (name != L"." && name != L".." && (isDir || !excludedFile(name))) {
            const bool systemMount = wsl && unc::equal(dir.path, unc::shareRoot(dir.path))
                && (name == L"proc" || name == L"sys" || name == L"dev" || name == L"run" || name == L"mnt");
            m_crawler.addToListing(out, dir, {}, name, Crawler::attributeFlags(data.dwFileAttributes),
                !systemMount && Crawler::shouldDescend(data.dwFileAttributes, data.dwReserved0), 0,
                fileTimeOf(win32::ticks(data.ftLastWriteTime)),
                packSize((std::uint64_t {data.nFileSizeHigh} << 32) | data.nFileSizeLow),
                fileTimeOf(win32::ticks(data.ftCreationTime)));
        }
    } while (::FindNextFileW(find.get(), &data));
    if (const auto error = ::GetLastError(); error != ERROR_NO_MORE_FILES)
        return fail(error, atRoot);
    m_inARow = 0;
    m_items.fetch_add(out.items.size(), std::memory_order_relaxed);
    if (m_options.progress)
        m_options.progress->fetch_add(out.items.size(), std::memory_order_relaxed);
    return true;
}

bool Lister::operator()(const Crawler::Root& dir, DirListing& out)
{
    out.clear();
    if (m_stopPass.stop_requested())
        return false; // no new wait on the server once the pass is stopped (WinShun quitting)
    if (!unc::wslDistribution(m_root).empty())
        return listWithFind(dir, out, true);
    if (webdav::isWebDavPath(m_root))
        return listWithFind(dir, out, false);
    const bool atRoot = win32::equalsIgnoreCase(dir.path, m_root);
    std::wstring path = win32::longPath(dir.path);
    if (atRoot)
        path.push_back(L'\\'); // "\\?\UNC\host\share\": the share itself
    const win32::UniqueHandle h(::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!h.valid())
        return fail(::GetLastError(), atRoot);
    out.readBuffer.resize((m_options.bufferBytes + 7) / sizeof(std::uint64_t));
    const auto bytes = static_cast<DWORD>(out.readBuffer.size() * sizeof(std::uint64_t));

    // Without short (8.3) names: a Samba server makes one up for every name
    // asked for. No file ids either: they mean nothing on a share.
    FILE_INFO_BY_HANDLE_CLASS request = m_options.shortNames ? FileIdBothDirectoryRestartInfo : FileFullDirectoryRestartInfo;
    for (;;) {
        if (m_stopPass.stop_requested())
            return false;
        if (!::GetFileInformationByHandleEx(h.get(), request, out.readBuffer.data(), bytes)) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_NO_MORE_FILES)
                return fail(error, atRoot);
            break;
        }
        request = m_options.shortNames ? FileIdBothDirectoryInfo : FileFullDirectoryInfo;
        const auto* p = reinterpret_cast<const std::byte*>(out.readBuffer.data());
        for (;;) {
            // Both start alike; only the name sits elsewhere.
            const auto* info = reinterpret_cast<const FILE_FULL_DIR_INFO*>(p);
            std::wstring_view name = m_options.shortNames
                ? std::wstring_view(reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p)->FileName,
                      info->FileNameLength / sizeof(wchar_t))
                : std::wstring_view(info->FileName, info->FileNameLength / sizeof(wchar_t));
            while (!name.empty() && name.back() == L'\0')
                name.remove_suffix(1); // WebDAV's (a server not told by its path): "a.txt\0", ".\0"
            const bool isDir = info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            if (name != L"." && name != L".." && (isDir || !excludedFile(name))) {
                // For reparse points, EaSize holds the reparse tag.
                m_crawler.addToListing(out, dir, {}, name, Crawler::attributeFlags(info->FileAttributes),
                    Crawler::shouldDescend(info->FileAttributes, info->EaSize), 0,
                    fileTimeOf(info->LastWriteTime.QuadPart), packSize(static_cast<std::uint64_t>(info->EndOfFile.QuadPart)),
                    fileTimeOf(info->CreationTime.QuadPart));
            }
            if (info->NextEntryOffset == 0)
                break;
            p += info->NextEntryOffset;
        }
    }
    m_inARow = 0;
    m_items.fetch_add(out.items.size(), std::memory_order_relaxed);
    if (m_options.progress)
        m_options.progress->fetch_add(out.items.size(), std::memory_order_relaxed);
    return true;
}

} // namespace ws::share
