// Network shares, WSL and WebDAV: WebDAV paths and servers, and the
// contents of remote files in the content index (GitHub #114).
#include "ChangeWatcher.h"
#include "ContentIndex.h"
#include "Crawler.h"
#include "FileIndex.h"
#include "IndexService.h"
#include "RemoteContent.h"
#include "SearchEngine.h"
#include "ShareIndexer.h"
#include "ShareLister.h"
#include "Unc.h"
#include "WebDav.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <limits>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

using namespace ws;
using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace {

// The entries a lookup says contain the phrase.
std::vector<EntryId> matchesOf(const ContentIndex& content, const QString& phrase)
{
    std::vector<EntryId> out;
    for (const std::uint32_t k : content.lookup(phrase).known) {
        if (k & ContentIndex::Lookup::kMatch)
            out.push_back(k & ~ContentIndex::Lookup::kMatch);
    }
    return out;
}

// A server on 127.0.0.1 that answers each connection with `reply` (nothing:
// reads and keeps quiet, as a proxy in TUN mode does for a server that is
// not there).
class FakeServer {
public:
    explicit FakeServer(std::string reply)
        : m_reply(std::move(reply))
    {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
        m_listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(m_listen, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(m_listen, 4);
        int length = sizeof a;
        ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&a), &length);
        m_port = ntohs(a.sin_port);
        m_thread = std::thread([this] {
            for (;;) {
                const SOCKET s = ::accept(m_listen, nullptr, nullptr);
                if (s == INVALID_SOCKET)
                    return;
                char request[1024];
                const int got = ::recv(s, request, sizeof request, 0);
                if (got > 0) {
                    m_requests.fetch_add(1);
                    const std::lock_guard lock(m_mutex);
                    m_last.assign(request, static_cast<std::size_t>(got));
                }
                if (!m_reply.empty())
                    ::send(s, m_reply.data(), static_cast<int>(m_reply.size()), 0);
                else
                    std::this_thread::sleep_for(800ms);
                ::closesocket(s);
            }
        });
    }
    ~FakeServer()
    {
        ::closesocket(m_listen);
        m_thread.join();
    }
    std::uint16_t port() const { return m_port; }
    int requests() const { return m_requests.load(); }
    std::string lastRequest() const // its first 1024 bytes
    {
        const std::lock_guard lock(m_mutex);
        return m_last;
    }

private:
    std::string m_reply;
    SOCKET m_listen = INVALID_SOCKET;
    std::uint16_t m_port = 0;
    std::atomic<int> m_requests {0};
    mutable std::mutex m_mutex;
    std::string m_last; // m_mutex
    std::thread m_thread;
};

// A port on 127.0.0.1 where connections are made (Windows completes them
// for the listening socket) and nothing ever answers: never accepted, as a
// proxy in TUN mode or an address that drops what comes in.
class Blackhole {
public:
    Blackhole()
    {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
        m_listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(m_listen, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(m_listen, 8);
        int length = sizeof a;
        ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&a), &length);
        m_port = ntohs(a.sin_port);
    }
    ~Blackhole() { ::closesocket(m_listen); }
    Blackhole(const Blackhole&) = delete;
    Blackhole& operator=(const Blackhole&) = delete;
    std::uint16_t port() const { return m_port; }

private:
    SOCKET m_listen = INVALID_SOCKET;
    std::uint16_t m_port = 0;
};

std::uint16_t closedPort()
{
    WSADATA data;
    ::WSAStartup(MAKEWORD(2, 2), &data);
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    int length = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &length);
    ::closesocket(s); // nothing listens there now
    return ntohs(a.sin_port);
}

std::wstring hostAt(std::uint16_t port, bool tls = false)
{
    return L"127.0.0.1" + std::wstring(tls ? L"@SSL" : L"") + L'@' + std::to_wstring(port);
}

// The first share's status once it is read or failed (at most 30 s), told
// in the test's output.
ShareIndexer::Status settledStatus(const ShareIndexer& shares)
{
    ShareIndexer::Status s;
    for (int i = 0; i < 300 && s.state != ShareIndexer::State::Problem && s.state != ShareIndexer::State::Online; ++i) {
        std::this_thread::sleep_for(100ms); // 30 s
        const auto all = shares.statuses();
        if (!all.empty())
            s = all.front();
    }
    qInfo("state %d, problem %d, error %lu, %zu items", int(s.state), int(s.problem), s.error, s.items);
    return s;
}

// One share at `path`, read once and tried again soon (Options::retry, short here).
ShareIndexer::Options oneShare(const std::wstring& path)
{
    ShareIndexer::Options o;
    o.shares = {{path, {}}};
    o.verifyEvery = 0min;
    o.verifyEveryUnwatched = 0min;
    o.retry.first = 300ms;
    o.retry.last = 600ms;
    return o;
}

} // namespace

class RemoteContentTest : public QObject {
    Q_OBJECT

private slots:
    void webDavHosts()
    {
        using webdav::Host;
        QCOMPARE(webdav::parseHost(L"nas@SSL@5006"), std::optional(Host {L"nas", 5006, true}));
        QCOMPARE(webdav::parseHost(L"nas@ssl"), std::optional(Host {L"nas", 443, true}));
        QCOMPARE(webdav::parseHost(L"192.168.1.5@5244"), std::optional(Host {L"192.168.1.5", 5244, false}));
        QCOMPARE(webdav::parseHost(L"nas@80"), std::optional(Host {L"nas", 80, false}));
        QVERIFY(!webdav::parseHost(L"nas"));
        QVERIFY(!webdav::parseHost(L"@SSL"));
        QVERIFY(!webdav::parseHost(L"user@nas")); // not WebClient's form
        QVERIFY(!webdav::parseHost(L"nas@0"));
        QVERIFY(!webdav::parseHost(L"nas@70000"));
        QVERIFY(!webdav::parseHost(L"nas@5006@SSL")); // the port comes last
        QCOMPARE(webdav::displayHost(L"nas@SSL@5006"), std::wstring(L"nas:5006"));
        QCOMPARE(webdav::displayHost(L"nas@SSL"), std::wstring(L"nas"));
        QCOMPARE(webdav::displayHost(L"nas"), std::wstring(L"nas"));

        QVERIFY(webdav::isWebDavPath(L"\\\\nas@SSL@5006\\dav\\docs"));
        QVERIFY(webdav::isWebDavPath(L"\\\\server\\DavWWWRoot\\a"));
        QVERIFY(webdav::isWebDavPath(L"\\\\server\\davwwwroot"));
        QVERIFY(!webdav::isWebDavPath(L"\\\\nas\\docs"));
        QVERIFY(!webdav::isWebDavPath(L"\\\\wsl.localhost\\Ubuntu\\home"));
        QVERIFY(!webdav::isWebDavPath(L"C:\\Users"));
    }

    void webAddresses()
    {
        QCOMPARE(webdav::uncFromUrl(L"https://nas:5006/dav/docs/"), std::wstring(L"\\\\nas@SSL@5006\\dav\\docs"));
        QCOMPARE(webdav::uncFromUrl(L"https://dav.jianguoyun.com/dav/"), std::wstring(L"\\\\dav.jianguoyun.com@SSL\\dav"));
        QCOMPARE(webdav::uncFromUrl(L"http://192.168.1.5:5244/dav"), std::wstring(L"\\\\192.168.1.5@5244\\dav"));
        QCOMPARE(webdav::uncFromUrl(L"http://127.0.0.1:8714/"), std::wstring(L"\\\\127.0.0.1@8714\\DavWWWRoot"));
        QCOMPARE(webdav::uncFromUrl(L"HTTP://nas/dav"), std::wstring(L"\\\\nas@80\\dav")); // port 80 written out
        QCOMPARE(webdav::uncFromUrl(L"https://nas"), std::wstring(L"\\\\nas@SSL\\DavWWWRoot"));
        QCOMPARE(webdav::uncFromUrl(L"https://nas/%E8%B5%84%E6%96%99/a%20b"), std::wstring(L"\\\\nas@SSL\\资料\\a b"));
        QCOMPARE(webdav::uncFromUrl(L"  https://nas/资料  "), std::wstring(L"\\\\nas@SSL\\资料"));
        QCOMPARE(webdav::uncFromUrl(L"davs://nas:5006/home"), std::wstring(L"\\\\nas@SSL@5006\\home"));
        QCOMPARE(webdav::uncFromUrl(L"webdav://nas/home"), std::wstring(L"\\\\nas@80\\home"));
        QVERIFY(webdav::uncFromUrl(L"ftp://nas/dav").empty());
        QVERIFY(webdav::uncFromUrl(L"http://nas:0/dav").empty()); // "\\nas@0" would not be WebClient's form
        QVERIFY(webdav::uncFromUrl(L"\\\\nas\\docs").empty());
        QVERIFY(webdav::uncFromUrl(L"https://[fe80::1]:5006/dav").empty());
        QVERIFY(webdav::uncFromUrl(L"https://").empty());

        // Why not, for the settings' words: an IPv6 address is a web address
        // all the same, only not one WebClient's UNC form takes.
        using webdav::UrlProblem;
        QCOMPARE(webdav::urlProblem(L"ftp://nas/dav"), UrlProblem::Scheme);
        QCOMPARE(webdav::urlProblem(L"\\\\nas\\docs"), UrlProblem::Scheme);
        QCOMPARE(webdav::urlProblem(L"https://[fe80::1]:5006/dav"), UrlProblem::Ipv6);
        QCOMPARE(webdav::urlProblem(L"http://[::1]/"), UrlProblem::Ipv6);
        QCOMPARE(webdav::urlProblem(L"https://:5006/x"), UrlProblem::Address);
        QCOMPARE(webdav::urlProblem(L"https://nas:99999/x"), UrlProblem::Address);
        QCOMPARE(webdav::urlProblem(L"http://nas:0/dav"), UrlProblem::Address);
        QCOMPARE(webdav::urlProblem(L"https://"), UrlProblem::Address);
        QCOMPARE(webdav::urlProblem(L"https://nas:5006/dav"), UrlProblem::None);
        QCOMPARE(webdav::urlProblem(L"dav://nas/home"), UrlProblem::None);
    }

    // WebClient that would not start is not tried again by a share's thread
    // for kStartAgain (each try waits up to a minute); the user's start
    // (a file opened, 立即核对) always tries.
    void webClientStartsBackedOff()
    {
        using webdav::Starter;
        const auto now = std::chrono::steady_clock::now();
        QVERIFY(webdav::mayStart(Starter::Background, std::nullopt, now));
        QVERIFY(!webdav::mayStart(Starter::Background, now - 1min, now));
        QVERIFY(!webdav::mayStart(Starter::Background, now - webdav::kStartAgain + 1s, now));
        QVERIFY(webdav::mayStart(Starter::Background, now - webdav::kStartAgain, now));
        QVERIFY(webdav::mayStart(Starter::Background, now - 11min, now));
        QVERIFY(webdav::mayStart(Starter::User, now - 1min, now));
    }

    // An http WebDAV server that takes a password in the clear alone, which
    // WebClient does not send (BasicAuthLevel 1): Windows says only that the
    // folder is not there (67), the server itself why.
    void clearPasswordWanted()
    {
        std::string asked;
        const auto wants = [&](const std::string& reply) {
            FakeServer server(reply);
            const bool yes = webdav::wantsClearPassword(L"\\\\" + hostAt(server.port()) + L"\\dav\\资料 a", 3000ms);
            asked = server.lastRequest();
            return yes;
        };
        const std::string unauthorized = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n";
        QVERIFY(wants(unauthorized + "WWW-Authenticate: Basic realm=\"nas, home\", charset=\"UTF-8\"\r\n\r\n"));
        QVERIFY2(asked.starts_with("PROPFIND /dav/%E8%B5%84%E6%96%99%20a HTTP/1.1\r\n"), asked.c_str());
        QVERIFY(asked.find("\r\nDepth: 0\r\n") != std::string::npos);
        QVERIFY(asked.find("\r\nHost: 127.0.0.1:") != std::string::npos);
        QVERIFY(wants("HTTP/1.0 401 Unauthorized\r\nwww-authenticate:basic realm = x\r\n\r\n"));
        // A way Windows takes over http too: it signs in by itself, or asks.
        QVERIFY(!wants(unauthorized + "WWW-Authenticate: Basic realm=\"x\"\r\nWWW-Authenticate: NTLM\r\n\r\n"));
        QVERIFY(!wants(unauthorized + "WWW-Authenticate: Basic realm=\"x\", Negotiate\r\n\r\n"));
        QVERIFY(!wants(unauthorized + "WWW-Authenticate: Digest realm=\"x\", nonce=\"abc\", qop=\"auth\"\r\n\r\n"));
        QVERIFY(!wants(unauthorized + "\r\n")); // no way in offered
        QVERIFY(!wants("HTTP/1.1 207 Multi-Status\r\nContent-Length: 0\r\n\r\n"));
        QVERIFY(!wants("HTTP/1.1 404 Not Found\r\nWWW-Authenticate: Basic realm=x\r\n\r\n"));

        // Nobody there, or nobody answering: no, within the time.
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!webdav::wantsClearPassword(L"\\\\" + hostAt(closedPort()) + L"\\dav", 500ms));
        QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));
        const Blackhole hole;
        timer.restart();
        QVERIFY(!webdav::wantsClearPassword(L"\\\\" + hostAt(hole.port()) + L"\\dav", 500ms));
        QVERIFY2(timer.elapsed() >= 450 && timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));
        // Not asked: https (Windows sends Basic there), SMB.
        FakeServer secure(unauthorized + "WWW-Authenticate: Basic realm=x\r\n\r\n");
        QVERIFY(!webdav::wantsClearPassword(L"\\\\" + hostAt(secure.port(), true) + L"\\dav", 500ms));
        QCOMPARE(secure.requests(), 0);
        QVERIFY(!webdav::wantsClearPassword(L"\\\\nas\\docs", 500ms));
    }

    // A WebDAV root Windows turns away in a code that says nothing elsewhere
    // (GitHub #114): "not supported" (50) perhaps a sign-in, "cannot access"
    // (1920) the server's 403, "not functioning" (31) its 500. Not SMB or
    // WSL, not another error, not what is told apart already.
    void davRefusalAtWebDavRoot()
    {
        using P = share::Problem;
        using R = share::DavRefusal;
        for (const std::wstring_view path : {L"\\\\nas@5006\\dav", L"\\\\nas@SSL@5006\\dav", L"\\\\nas\\DavWWWRoot\\dav"}) {
            QCOMPARE(share::davRefusal(path, P::Other, ERROR_NOT_SUPPORTED), R::MaybeSignIn);
            QCOMPARE(share::davRefusal(path, P::Other, ERROR_CANT_ACCESS_FILE), R::Forbidden);
            QCOMPARE(share::davRefusal(path, P::Other, ERROR_GEN_FAILURE), R::ServerError);
            QCOMPARE(share::davRefusal(path, P::Other, ERROR_ACCESS_DENIED), R::None);
            QCOMPARE(share::davRefusal(path, P::NeedsSignIn, ERROR_NOT_SUPPORTED), R::None);
            QCOMPARE(share::davRefusal(path, P::Denied, ERROR_CANT_ACCESS_FILE), R::None);
        }
        for (const std::wstring_view path : {L"\\\\nas\\docs", L"\\\\wsl.localhost\\Ubuntu\\home"}) {
            QCOMPARE(share::davRefusal(path, P::Other, ERROR_NOT_SUPPORTED), R::None);
            QCOMPARE(share::davRefusal(path, P::Other, ERROR_CANT_ACCESS_FILE), R::None);
            QCOMPARE(share::davRefusal(path, P::Other, ERROR_GEN_FAILURE), R::None);
        }
    }

    // The same through Windows, where WebClient runs (WINSHUN_TEST_WEBDAV).
    // A server that wants a password even for OPTIONS, in a way Windows will
    // not or cannot use over http, is "not supported" (50) at the root
    // (measured 2026-10-29: Basic alone and Basic with Bearer; Digest and
    // NTLM come back as 5, signing in as before). Basic alone: signing in,
    // with the reason. With Bearer: perhaps signing in, tried again soon (in
    // Options::retry's time, short here) rather than in half an hour. Never
    // "online, 0 items".
    void clearPasswordWantedThroughWindows()
    {
        if (qEnvironmentVariable("WINSHUN_TEST_WEBDAV").isEmpty())
            QSKIP("no WebDAV test machine (WINSHUN_TEST_WEBDAV)");
        const std::string unauthorized = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n";
        {
            FakeServer server(unauthorized + "WWW-Authenticate: Basic realm=\"x\"\r\n\r\n");
            const std::wstring path = L"\\\\" + hostAt(server.port()) + L"\\DavWWWRoot";
            if (!webdav::httpSignInBlocked(path))
                QSKIP("this Windows sends passwords over http (BasicAuthLevel 2)");
            auto index = std::make_shared<FileIndex>();
            ShareIndexer shares({[&] { return index; }, [] { return true; }}, oneShare(path), {});
            shares.start();
            const ShareIndexer::Status s = settledStatus(shares);
            shares.stop();
            QCOMPARE(s.state, ShareIndexer::State::Problem);
            QVERIFY(s.problem == share::Problem::NeedsSignIn);
        }
        {
            FakeServer server(unauthorized + "WWW-Authenticate: Basic realm=\"x\"\r\nWWW-Authenticate: Bearer\r\n\r\n");
            const std::wstring path = L"\\\\" + hostAt(server.port()) + L"\\DavWWWRoot";
            auto index = std::make_shared<FileIndex>();
            ShareIndexer shares({[&] { return index; }, [] { return true; }}, oneShare(path), {});
            shares.start();
            const ShareIndexer::Status s = settledStatus(shares);
            QCOMPARE(s.state, ShareIndexer::State::Problem);
            QVERIFY(s.problem == share::Problem::Other && s.error == ERROR_NOT_SUPPORTED);
            QCOMPARE(share::davRefusal(path, s.problem, s.error), share::DavRefusal::MaybeSignIn);
            const int before = server.requests();
            QTRY_VERIFY_WITH_TIMEOUT(server.requests() > before, 5000); // tried again
            shares.stop();
        }
    }

    // The same for a server that answers 403 or 500, even to OPTIONS
    // (measured 2026-10-29: 1920 and 31 at the root). A problem, not
    // "online, 0 items"; the 500 tried again soon (Options::retry, short here).
    void davRefusedThroughWindows()
    {
        if (qEnvironmentVariable("WINSHUN_TEST_WEBDAV").isEmpty())
            QSKIP("no WebDAV test machine (WINSHUN_TEST_WEBDAV)");
        {
            FakeServer server("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            const std::wstring path = L"\\\\" + hostAt(server.port()) + L"\\DavWWWRoot";
            auto index = std::make_shared<FileIndex>();
            ShareIndexer shares({[&] { return index; }, [] { return true; }}, oneShare(path), {});
            shares.start();
            const ShareIndexer::Status s = settledStatus(shares);
            shares.stop();
            QCOMPARE(s.state, ShareIndexer::State::Problem);
            QVERIFY(s.problem == share::Problem::Other && s.error == ERROR_CANT_ACCESS_FILE);
            QCOMPARE(share::davRefusal(path, s.problem, s.error), share::DavRefusal::Forbidden);
        }
        {
            FakeServer server("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            const std::wstring path = L"\\\\" + hostAt(server.port()) + L"\\DavWWWRoot";
            auto index = std::make_shared<FileIndex>();
            ShareIndexer shares({[&] { return index; }, [] { return true; }}, oneShare(path), {});
            shares.start();
            const ShareIndexer::Status s = settledStatus(shares);
            QCOMPARE(s.state, ShareIndexer::State::Problem);
            QVERIFY(s.problem == share::Problem::Other && s.error == ERROR_GEN_FAILURE);
            QCOMPARE(share::davRefusal(path, s.problem, s.error), share::DavRefusal::ServerError);
            const int before = server.requests();
            QTRY_VERIFY_WITH_TIMEOUT(server.requests() > before, 5000); // tried again
            shares.stop();
        }
    }

    // One WebDAV folder, one form, however it was written: Windows itself
    // writes one two ways (net use lists "\\h@p\dav", and says
    // "\\h@p\DavWWWRoot\dav" for the drive), and a folder added already must
    // be seen so (SettingsEditor::addShare compares canonical paths).
    void canonicalForms()
    {
        // And the same again: settings are tidied each time they are read.
        const auto same = [](std::wstring_view written, std::wstring_view kept) {
            const std::wstring c = unc::canonical(written);
            const std::wstring again = unc::canonical(c);
            if (c != kept || again != c)
                qWarning("%ls -> %ls -> %ls, not %ls", std::wstring(written).c_str(), c.c_str(), again.c_str(),
                    std::wstring(kept).c_str());
            return c == kept && again == c;
        };
        const std::wstring small = L"\\\\127.0.0.1@8714\\small";
        QVERIFY(same(L"\\\\127.0.0.1@8714\\small", small));
        QVERIFY(same(L"\\\\127.0.0.1@8714\\DavWWWRoot\\small", small));
        QVERIFY(same(L"\\\\127.0.0.1@8714\\davwwwroot\\small\\", small));
        QVERIFY(same(L"\\\\127.0.0.1@8714\\\\DavWWWRoot\\\\small", small));
        QCOMPARE(webdav::uncFromUrl(L"http://127.0.0.1:8714/small/"), small);
        // In an address a folder of the root's name is a folder (WebClient's
        // name is for UNC paths alone).
        QCOMPARE(webdav::uncFromUrl(L"http://127.0.0.1:8714/DavWWWRoot/small"),
            std::wstring(L"\\\\127.0.0.1@8714\\DavWWWRoot\\DavWWWRoot\\small"));
        QVERIFY(same(L"\\\\LocalHost@8714\\small", L"\\\\localhost@8714\\small")); // the server in lower case
        // TLS: "@SSL", and its port unless 443.
        const std::wstring dav = L"\\\\nas@SSL\\dav";
        QVERIFY(same(L"\\\\nas@ssl\\dav", dav));
        QVERIFY(same(L"\\\\NAS@SSL@443\\DavWWWRoot\\dav\\", dav));
        QCOMPARE(webdav::uncFromUrl(L"https://nas:443/dav"), dav);
        QVERIFY(same(L"\\\\nas@SSL@5006\\dav", L"\\\\nas@SSL@5006\\dav"));
        QVERIFY(same(L"\\\\nas@443\\dav", L"\\\\nas@443\\dav")); // plain http on 443: as it is
        // Plain http on 80 says so; the server's root keeps DavWWWRoot; a
        // server alone is its root.
        QVERIFY(same(L"\\\\nas\\DavWWWRoot\\dav", L"\\\\nas@80\\dav"));
        QVERIFY(same(L"\\\\nas\\davwwwroot", L"\\\\nas@80\\DavWWWRoot"));
        QVERIFY(same(L"\\\\nas@80\\DavWWWRoot\\", L"\\\\nas@80\\DavWWWRoot"));
        QVERIFY(same(L"\\\\nas@SSL@5006", L"\\\\nas@SSL@5006\\DavWWWRoot"));
        QVERIFY(same(L"\\\\nas@SSL@5006\\", L"\\\\nas@SSL@5006\\DavWWWRoot"));
        QCOMPARE(webdav::uncFromUrl(L"https://nas:5006"), std::wstring(L"\\\\nas@SSL@5006\\DavWWWRoot"));
        // Its folders keep their case: a WebDAV server may tell them apart.
        QVERIFY(same(L"\\\\nas@SSL\\DavWWWRoot\\Dav\\Docs", L"\\\\nas@SSL\\Dav\\Docs"));
        // SMB and WSL: as normalize() has them.
        QVERIFY(same(L"\\\\NAS\\Docs\\", L"\\\\NAS\\Docs"));
        QVERIFY(same(L"\\\\user@nas\\docs", L"\\\\user@nas\\docs")); // not WebClient's form
        QVERIFY(same(L"\\\\wsl$\\Ubuntu\\home", L"\\\\wsl.localhost\\Ubuntu\\home"));
        // A server not in WebClient's form at the root's name: as written
        // (before, each read of the settings gave it another "@80").
        for (const wchar_t* path : {L"\\\\nas@0\\DavWWWRoot", L"\\\\user@nas\\DavWWWRoot", L"\\\\nas@5006@SSL\\DavWWWRoot",
                 L"\\\\nas@abc\\DavWWWRoot\\x", L"\\\\nas@99999\\DavWWWRoot\\dav"})
            QVERIFY(same(path, path));
        // A folder named DavWWWRoot at the top: the root stays before it,
        // which alone would read as the root.
        QVERIFY(same(L"\\\\nas@80\\DavWWWRoot\\DavWWWRoot\\x", L"\\\\nas@80\\DavWWWRoot\\DavWWWRoot\\x"));
        QVERIFY(same(L"\\\\nas\\DavWWWRoot\\DavWWWRoot", L"\\\\nas@80\\DavWWWRoot\\DavWWWRoot"));
        QCOMPARE(webdav::uncFromUrl(L"http://nas/DavWWWRoot/x"), std::wstring(L"\\\\nas@80\\DavWWWRoot\\DavWWWRoot\\x"));
        QCOMPARE(webdav::uncFromUrl(L"https://nas/davwwwroot/"), std::wstring(L"\\\\nas@SSL\\DavWWWRoot\\davwwwroot"));
        QVERIFY(unc::canonical(L"\\\\nas").empty()); // an SMB server alone: no share
        QVERIFY(unc::canonical(L"C:\\Users").empty());
        for (const wchar_t* path : {L"\\\\nas@SSL\\dav", L"\\\\nas@80\\DavWWWRoot", L"\\\\nas@SSL@5006\\a\\b"})
            QVERIFY(same(unc::canonical(path), path)); // kept as it is

        // A server's root holds its folders, which are written without it.
        QVERIFY(webdav::rootHolds(L"\\\\nas@SSL\\DavWWWRoot", L"\\\\nas@SSL\\dav\\docs"));
        QVERIFY(webdav::rootHolds(L"\\\\nas@SSL\\DavWWWRoot", L"\\\\NAS@SSL\\dav"));
        QVERIFY(!webdav::rootHolds(L"\\\\nas@SSL\\DavWWWRoot", L"\\\\nas@SSL\\DavWWWRoot")); // the same: equal()
        QVERIFY(!webdav::rootHolds(L"\\\\nas@SSL\\DavWWWRoot", L"\\\\nas@SSL@5006\\dav")); // another port
        QVERIFY(!webdav::rootHolds(L"\\\\nas@SSL\\dav", L"\\\\nas@SSL\\dav\\docs")); // contains() tells that
        QVERIFY(!webdav::rootHolds(L"\\\\nas\\DavWWWRoot", L"\\\\nas\\docs")); // not canonical; SMB
        // A root and a folder in it, from settings written before: read once.
        ShareIndexer::Options o;
        o.shares = {{L"\\\\nas@80\\DavWWWRoot", {}}, {L"\\\\nas@80\\dav", {}}, {L"\\\\nas@8080\\dav", {}}};
        const ShareIndexer once({[] { return std::shared_ptr<FileIndex>(); }, [] { return false; }}, o, {});
        const auto statuses = once.statuses();
        QCOMPARE(statuses.size(), std::size_t {2});
        QCOMPARE(statuses[1].path, std::wstring(L"\\\\nas@8080\\dav"));
    }

    // A WebDAV folder's card shows its web address, which reads back as the
    // folder (GitHub #114).
    void webDavUrls()
    {
        QCOMPARE(webdav::urlOf(L"\\\\nas@SSL@5006\\dav\\资料"), std::wstring(L"https://nas:5006/dav/资料"));
        QCOMPARE(webdav::urlOf(L"\\\\nas@SSL\\dav"), std::wstring(L"https://nas/dav"));
        QCOMPARE(webdav::urlOf(L"\\\\127.0.0.1@8714\\many"), std::wstring(L"http://127.0.0.1:8714/many"));
        QCOMPARE(webdav::urlOf(L"\\\\127.0.0.1@8714\\DavWWWRoot"), std::wstring(L"http://127.0.0.1:8714/"));
        QCOMPARE(webdav::urlOf(L"\\\\nas@80\\DavWWWRoot\\DavWWWRoot\\x"), std::wstring(L"http://nas/DavWWWRoot/x"));
        QCOMPARE(webdav::urlOf(L"\\\\nas@443\\dav"), std::wstring(L"http://nas:443/dav")); // plain http on 443
        // Not canonical: as its canonical form.
        QCOMPARE(webdav::urlOf(L"\\\\NAS\\DavWWWRoot\\Dav\\"), std::wstring(L"http://nas/Dav"));
        QCOMPARE(webdav::urlOf(L"\\\\nas@ssl@443\\DavWWWRoot\\a b"), std::wstring(L"https://nas/a b"));
        // What an address would read otherwise.
        QCOMPARE(webdav::urlOf(L"\\\\nas@8714\\50%\\#1"), std::wstring(L"http://nas:8714/50%25/%231"));
        for (const wchar_t* path : {L"\\\\nas\\docs", L"\\\\wsl.localhost\\Ubuntu\\home", L"\\\\user@nas\\docs",
                 L"\\\\nas@0\\DavWWWRoot", L"C:\\Users", L""})
            QVERIFY(webdav::urlOf(path).empty());
        // Read back as the folder it shows.
        for (const wchar_t* path : {L"\\\\nas@SSL@5006\\dav\\资料", L"\\\\nas@80\\DavWWWRoot", L"\\\\nas@SSL\\DavWWWRoot",
                 L"\\\\nas@80\\DavWWWRoot\\DavWWWRoot\\x", L"\\\\nas@SSL\\DavWWWRoot\\davwwwroot", L"\\\\nas@443\\dav",
                 L"\\\\nas@8714\\50%\\#1", L"\\\\nas@SSL@80\\a b\\c"})
            QCOMPARE(webdav::uncFromUrl(webdav::urlOf(path)), std::wstring(path));
    }

    // A reply in HTTP or TLS: an answer. A connection alone (a TUN proxy) or
    // a closed port: none, within the time given.
    void webDavServerAnswers()
    {
        const FakeServer http("HTTP/1.1 200 OK\r\nDAV: 1,2\r\nContent-Length: 0\r\n\r\n");
        QVERIFY(webdav::answers(hostAt(http.port()), 1500ms, true));
        QVERIFY(share::reachable(hostAt(http.port()), 1500ms, true)); // not the SMB probe
        QVERIFY(share::reachablePath(L"\\\\" + hostAt(http.port()) + L"\\DavWWWRoot\\a.txt", true));

        const FakeServer tls(std::string("\x15\x03\x03\x00\x02\x02\x46", 7)); // an alert: still a TLS server
        QVERIFY(webdav::answers(hostAt(tls.port(), true), 1500ms, true));
        QVERIFY(!webdav::answers(hostAt(http.port(), true), 1500ms, true)); // HTTP is no TLS reply

        const FakeServer silent({});
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!webdav::answers(hostAt(silent.port()), 500ms, true));
        QVERIFY(timer.elapsed() < 1200);
        QVERIFY(silent.requests() >= 1); // it was asked, and kept quiet

        QVERIFY(!webdav::answers(hostAt(closedPort()), 500ms, true));
        QVERIFY(!webdav::answers(L"nas", 500ms, true)); // not a WebDAV host
    }

    // Every address of a name at once (as share::probe for GitHub #116): a
    // first one that never answers does not use up the time, and the server
    // at the next one is found (it was called unreachable).
    void webDavAddressesRaced()
    {
        const FakeServer http("HTTP/1.1 200 OK\r\nDAV: 1,2\r\nContent-Length: 0\r\n\r\n");
        const FakeServer tls(std::string("\x16\x03\x03\x00\x02\x02\x00", 7));
        const Blackhole hole;
        const auto at = [](std::uint16_t port) { return L"127.0.0.1:" + std::to_wstring(port); };
        const webdav::Host plain {L"nas", 80, false};
        const webdav::Host secure {L"nas", 443, true};

        QElapsedTimer timer;
        timer.start();
        QVERIFY(webdav::answersAt(plain, {at(hole.port()), at(http.port())}, 3000ms));
        QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));
        timer.restart();
        QVERIFY(webdav::answersAt(secure, {at(hole.port()), at(closedPort()), at(tls.port())}, 3000ms));
        QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));

        // None answers: no sooner than the time given, and no later.
        timer.restart();
        QVERIFY(!webdav::answersAt(plain, {at(hole.port()), at(closedPort())}, 600ms));
        QVERIFY(timer.elapsed() >= 550 && timer.elapsed() < 1500);
        QVERIFY(!webdav::answersAt(secure, {at(http.port())}, 1500ms)); // HTTP is no TLS reply
        QVERIFY(!webdav::answersAt(plain, {}, 500ms));
        QVERIFY(!webdav::answersAt(plain, {L"not an address"}, 500ms));
    }

    void readErrors()
    {
        using remotecontent::ReadError;
        QCOMPARE(remotecontent::readErrorOf(ERROR_FILE_TOO_LARGE), ReadError::TooLarge);
        QCOMPARE(remotecontent::readErrorOf(ERROR_ACCESS_DENIED), ReadError::Denied);
        QCOMPARE(remotecontent::readErrorOf(ERROR_FILE_NOT_FOUND), ReadError::Gone);
        QCOMPARE(remotecontent::readErrorOf(ERROR_SHARING_VIOLATION), ReadError::Busy);
        QCOMPARE(remotecontent::readErrorOf(ERROR_BAD_NETPATH), ReadError::Network);
        QCOMPARE(remotecontent::readErrorOf(ERROR_NETNAME_DELETED), ReadError::Network);
        QCOMPARE(remotecontent::readErrorOf(ERROR_SEM_TIMEOUT), ReadError::Network);
        QCOMPARE(remotecontent::readErrorOf(ERROR_NOT_READY), ReadError::Network); // WebClient's
        // Signing in, as the share's check has it (share::classify): stop, it tells the user.
        QCOMPARE(remotecontent::readErrorOf(ERROR_LOGON_FAILURE), ReadError::Network);
        QCOMPARE(remotecontent::readErrorOf(ERROR_ACCOUNT_LOCKED_OUT), ReadError::Network);
        QCOMPARE(remotecontent::readErrorOf(ERROR_INVALID_DATA), ReadError::Other);
    }

    // A remote file's stamp comes from what its listing gave the index: the
    // same listing again leaves it current, a new size or time does not.
    void stampsFromTheListing()
    {
        FileIndex index;
        const EntryId root = index.addRoot("\\\\nas\\docs");
        const EntryId id = index.add(root, "a.txt", EntryFlag::Remote);
        index.setSize(id, packSize(12345));
        index.setModified(id, 1'800'000'000);
        const std::uint32_t stamp = remotecontent::stampOf(index, id);
        constexpr std::int64_t kTicks = (1'800'000'000ll + 11'644'473'600ll) * 10'000'000ll;
        QCOMPARE(stamp, ContentIndex::stampOf(12345, kTicks));
        index.setModified(id, 1'800'000'001);
        QVERIFY(remotecontent::stampOf(index, id) != stamp);
        index.setModified(id, 1'800'000'000);
        index.setSize(id, packSize(12346));
        QVERIFY(remotecontent::stampOf(index, id) != stamp);
        index.setSize(id, packSize(12345));
        QCOMPARE(remotecontent::stampOf(index, id), stamp);
    }

    // A file read through a handle that saw another size or time: the index
    // takes them while it still has what it had before the read. Changed
    // meanwhile (the share's watcher told of a later write), it stands, and
    // the text's stamp is what was read, which no longer matches: the file
    // is read again, not left as current with the older text (GitHub #114).
    void stampsAfterARead()
    {
        using remotecontent::Listed;
        FileIndex index;
        EntryId id = kNoEntry;
        {
            auto lock = index.writeLock();
            id = index.add(index.addRoot("\\\\nas\\docs"), "a.txt", EntryFlag::Remote);
            index.setSize(id, packSize(100));
            index.setModified(id, 1'800'000'000);
        }
        const auto ticks = [](std::int64_t seconds) { return (seconds + 11'644'473'600ll) * 10'000'000ll; };
        const Listed before {packSize(100), 1'800'000'000};
        auto got = remotecontent::stampAfterRead(index, id, 100, ticks(1'800'000'000), before); // as listed
        QVERIFY(got && !got->wrote);
        QCOMPARE(got->stamp, remotecontent::stampOf(index, id));
        got = remotecontent::stampAfterRead(index, id, 150, ticks(1'800'000'010), before); // written to since
        QVERIFY(got && got->wrote);
        QCOMPARE(unpackSize(index.size(id)), std::uint64_t {150});
        QCOMPARE(index.modified(id), FileTime {1'800'000'010});
        QCOMPARE(got->stamp, remotecontent::stampOf(index, id));

        const Listed during {packSize(150), 1'800'000'010};
        {
            auto lock = index.writeLock(); // what the watcher told while it was read
            index.setSize(id, packSize(300));
            index.setModified(id, 1'800'000'030);
        }
        got = remotecontent::stampAfterRead(index, id, 200, ticks(1'800'000'020), during);
        QVERIFY(got && !got->wrote);
        QCOMPARE(unpackSize(index.size(id)), std::uint64_t {300});
        QCOMPARE(index.modified(id), FileTime {1'800'000'030});
        QCOMPARE(got->stamp, ContentIndex::stampOf(200, ticks(1'800'000'020)));
        QVERIFY(got->stamp != remotecontent::stampOf(index, id));

        got = remotecontent::stampAfterRead(index, id, -1, 0, {}); // no handle: too large as listed
        QVERIFY(got && !got->wrote);
        QCOMPARE(got->stamp, remotecontent::stampOf(index, id));
        {
            auto lock = index.writeLock();
            index.remove(id);
        }
        QVERIFY(!remotecontent::stampAfterRead(index, id, 100, ticks(1'800'000'000), before));
    }

    // What a share's thread reads, found in the index alone: files without a
    // current document, those written to first, documents after text, hidden
    // ones after those; too large ones recorded unread, unsure ones confirmed.
    void planFromTheIndex()
    {
        FileIndex index;
        const EntryId root = index.addRoot("\\\\nas\\docs");
        index.setFlags(root, EntryFlag::Remote);
        const auto file = [&](EntryId parent, std::string_view name, std::uint64_t bytes, std::uint8_t flags = 0) {
            const EntryId id = index.add(parent, name, EntryFlag::Remote | flags);
            index.setSize(id, packSize(bytes));
            index.setModified(id, 1'800'000'000);
            return id;
        };
        const EntryId sub = index.add(root, "sub", EntryFlag::Remote | EntryFlag::Directory);
        const EntryId deep = file(sub, "deep.txt", 10);
        const EntryId fresh = file(root, "a.txt", 10);
        const EntryId current = file(root, "b.md", 10);
        const EntryId changed = file(root, "c.txt", 10);
        const EntryId unsure = file(root, "d.txt", 10);
        const EntryId large = file(root, "large.txt", 40ull << 20);
        const EntryId doc = file(root, "e.docx", 1000);
        const EntryId hidden = file(root, "h.txt", 10, EntryFlag::Hidden);
        file(root, "tool.exe", 10); // not of a listed type
        file(root, "lib.txt", 10, EntryFlag::LowPriority); // in a tool folder
        const EntryId removed = file(root, "gone.txt", 10);
        index.remove(removed);

        ContentIndexer::Options options;
        options.extensions = {u"txt"_s, u"md"_s};
        std::vector<ContentIndex::DocInfo> docs {
            {current, ContentIndex::Need::None, remotecontent::stampOf(index, current)},
            {changed, ContentIndex::Need::None, remotecontent::stampOf(index, changed) + 1},
            {unsure, ContentIndex::Need::Check, remotecontent::stampOf(index, unsure)},
            {removed, ContentIndex::Need::None, 7},
        };
        std::ranges::sort(docs, {}, &ContentIndex::DocInfo::entry);
        const remotecontent::Plan p
            = remotecontent::plan(index, root, remotecontent::filterOf(options), ContentSizeLimits {}, 0, docs);
        QCOMPARE(p.files, std::size_t {8}); // not tool.exe, lib.txt, gone.txt
        QCOMPARE(p.current, std::size_t {3}); // b.md, d.txt, large.txt
        QCOMPARE(p.tooLarge, std::vector<EntryId> {large});
        QCOMPARE(p.confirm, std::vector<EntryId> {unsure});
        std::vector<EntryId> order;
        for (const remotecontent::Plan::File& f : p.read)
            order.push_back(f.entry);
        QCOMPARE(order, (std::vector<EntryId> {changed, fresh, deep, doc, hidden}));
        QVERIFY(p.read.front().again);
        QVERIFY(!p.read[1].again);
        QVERIFY(p.read[3].document);

        // WebClient's size limit on top of the kinds' (FileSizeLimitInBytes).
        remotecontent::Plan capped
            = remotecontent::plan(index, root, remotecontent::filterOf(options), ContentSizeLimits {}, 100, docs);
        std::ranges::sort(capped.tooLarge);
        QCOMPARE(capped.tooLarge, (std::vector<EntryId> {std::min(large, doc), std::max(large, doc)}));

        // A text file is read whole: never one over kMaxTextBytes, whatever its kind's limit.
        ContentSizeLimits generous;
        generous.bytes.fill(4096ll << 20); // the most the settings take
        const remotecontent::Plan whole
            = remotecontent::plan(index, root, remotecontent::filterOf(options), generous, 0, docs);
        QCOMPARE(whole.tooLarge, std::vector<EntryId> {large}); // 40 MB; e.docx is read
    }

    // The most of a remote file read: the smallest of its kind's limit (the
    // user's), WebClient's and, for a text file, which is read whole,
    // kMaxTextBytes. A user's limit below 16 MB is kept to.
    void remoteLimits()
    {
        constexpr std::uint64_t kMB = 1 << 20;
        QCOMPARE(remotecontent::limitOf(2 * kMB, 0, false), 2 * kMB);
        QCOMPARE(remotecontent::limitOf(50 * kMB, 0, false), remotecontent::kMaxTextBytes);
        QCOMPARE(remotecontent::limitOf(50 * kMB, 0, true), 50 * kMB); // a document is not kept whole
        QCOMPARE(remotecontent::limitOf(100 * kMB, 50'000'000, true), std::uint64_t {50'000'000});
        QCOMPARE(remotecontent::limitOf(2 * kMB, 50'000'000, false), 2 * kMB);
        QCOMPARE(remotecontent::limitOf(0, 0, true), std::uint64_t {0}); // none

        FileIndex index;
        const EntryId root = index.addRoot("\\\\nas\\docs");
        index.setFlags(root, EntryFlag::Remote);
        const EntryId mid = index.add(root, "mid.txt", EntryFlag::Remote);
        index.setSize(mid, packSize(2 * kMB));
        index.setModified(mid, 1'800'000'000);
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s};
        ContentSizeLimits limits;
        limits.bytes[ContentSizeLimits::Text] = static_cast<std::int64_t>(kMB);
        const auto planned = [&] { return remotecontent::plan(index, root, remotecontent::filterOf(options), limits, 0, {}); };
        QCOMPARE(planned().tooLarge, std::vector<EntryId> {mid});
        limits.bytes[ContentSizeLimits::Text] = static_cast<std::int64_t>(4 * kMB);
        QCOMPARE(planned().read.size(), std::size_t {1});

        // Grown past the user's limit since it was listed: too large when read.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QFile f(tmp.filePath(u"mid.txt"_s));
        QVERIFY(f.open(QIODevice::WriteOnly) && f.write(QByteArray(static_cast<qsizetype>(2 * kMB), 'a')) == qint64(2 * kMB));
        f.close();
        const remotecontent::Read read = remotecontent::readFile(QDir::toNativeSeparators(f.fileName()).toStdWString(),
            static_cast<std::int64_t>(remotecontent::limitOf(static_cast<std::int64_t>(kMB), 0, false)), false, nullptr, {});
        QCOMPARE(read.text.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(read.text.large);
    }

    // A text file is kept with its text, decoded as a search decodes it, so
    // that searches need neither the network nor the server.
    void readTextFiles()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto write = [&](const QString& name, const QByteArray& bytes) {
            QFile f(tmp.filePath(name));
            if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
                return std::wstring();
            return QDir::toNativeSeparators(f.fileName()).toStdWString();
        };
        const std::wstring text = write(u"notes.txt"_s, "first line\r\n年度报告 second"_ba);
        const remotecontent::Read read = remotecontent::readFile(text, 1 << 20, false, nullptr, {});
        QCOMPARE(read.text.outcome, ContentIndexer::Outcome::Indexed);
        QVERIFY(!read.text.keys.empty());
        // Packed, for textOf(): as a search decodes it.
        const QByteArray unpacked = qUncompress(read.text.text);
        const std::optional<doctext::DocText> kept = doctext::deserialize(
            std::string_view(unpacked.constData(), static_cast<std::size_t>(unpacked.size())),
            std::numeric_limits<std::size_t>::max());
        QVERIFY(kept);
        QCOMPARE(kept->text, std::string("first line\n年度报告 second\n"));
        QCOMPARE(read.text.bytes, std::int64_t {31});
        QVERIFY(read.text.written > 0);

        // Grown since it was listed past what a text file is read whole to:
        // too large, not read, whatever the limit.
        const std::wstring grown
            = write(u"grown.txt"_s, QByteArray(static_cast<qsizetype>(remotecontent::kMaxTextBytes) + 1, 'a'));
        const remotecontent::Read huge = remotecontent::readFile(grown, 0, false, nullptr, {});
        QCOMPARE(huge.text.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(huge.text.large);

        const std::wstring binary = write(u"data.txt"_s, QByteArray("PK\0\0binary", 10));
        QCOMPARE(remotecontent::readFile(binary, 1 << 20, false, nullptr, {}).text.outcome,
            ContentIndexer::Outcome::Empty);
        const remotecontent::Read large = remotecontent::readFile(text, 10, false, nullptr, {});
        QCOMPARE(large.text.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(large.text.large);
        const remotecontent::Read gone
            = remotecontent::readFile(text + L".missing", 1 << 20, false, nullptr, {});
        QCOMPARE(gone.text.outcome, ContentIndexer::Outcome::Skipped);
        QCOMPARE(remotecontent::readErrorOf(gone.text.error), remotecontent::ReadError::Gone);
    }

    // A share's thread reads its files into the content index: each once,
    // again when a listing shows another size or time, with its text kept,
    // so that a search needs neither the network nor the server. (A folder
    // here stands for the share: what is read is the same.)
    void shareContentSlices()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir dir(tmp.path());
        QVERIFY(dir.mkpath(u"share/sub"_s));
        const auto write = [&](const QString& name, const QByteArray& bytes) {
            QFile f(dir.filePath(u"share/"_s + name));
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        QVERIFY(write(u"a.txt"_s, "合同条款 alpha"_ba) && write(u"b.md"_s, "季度报告"_ba)
            && write(u"sub/c.txt"_s, "deep text"_ba) && write(u"big.txt"_s, QByteArray(5000, 'x'))
            && write(u"tool.exe"_s, "MZ"_ba));
        const QString rootPath = QDir::toNativeSeparators(dir.filePath(u"share"_s));
        const std::string rootName = rootPath.toStdString();
        auto index = std::make_shared<FileIndex>();
        // As a listing has them: sizes, and times in seconds.
        const auto listed = [&](EntryId id, const QString& name) {
            const QFileInfo info(dir.filePath(u"share/"_s + name));
            index->setSize(id, packSize(static_cast<std::uint64_t>(info.size())));
            index->setModified(id, static_cast<FileTime>(info.lastModified().toSecsSinceEpoch()));
        };
        EntryId root = kNoEntry, a = kNoEntry, b = kNoEntry, c = kNoEntry, big = kNoEntry;
        {
            auto lock = index->writeLock();
            root = index->addRoot(rootName);
            index->setFlags(root, EntryFlag::Remote);
            a = index->add(root, "a.txt", EntryFlag::Remote);
            b = index->add(root, "b.md", EntryFlag::Remote);
            big = index->add(root, "big.txt", EntryFlag::Remote);
            listed(index->add(root, "tool.exe", EntryFlag::Remote), u"tool.exe"_s);
            const EntryId sub = index->add(root, "sub", EntryFlag::Remote | EntryFlag::Directory);
            c = index->add(sub, "c.txt", EntryFlag::Remote);
            listed(a, u"a.txt"_s);
            listed(b, u"b.md"_s);
            listed(big, u"big.txt"_s);
            listed(c, u"sub/c.txt"_s);
        }
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        bool followed = true; // the share answers, and no pass runs
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] {
            ContentIndexer::Volume v {rootName, !followed};
            v.remote = true;
            return std::vector<ContentIndexer::Volume> {v};
        };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s, u"md"_s};
        options.sizeLimits.bytes.fill(4096); // big.txt is over it
        ContentIndexer indexer(content, source, options, {1h, 1h, 1h, 0ms}); // no pass of its own

        qputenv("WINSHUN_USER", "present"); // half speed: a pause after each file
        int pauses = 0;
        bool goOn = true;
        bool dirty = false;
        std::vector<remotecontent::ShareContent::Progress> told;
        const remotecontent::ShareContent::Source s {index, &indexer,
            [&](remotecontent::ShareContent::Clock::duration) {
                ++pauses;
                return goOn;
            },
            [&](const remotecontent::ShareContent::Progress& p) { told.push_back(p); }, [&] { dirty = true; }};
        remotecontent::ShareContent share(rootPath.toStdWString(), 0s); // read again whenever written to (readAgainNotTooOften)
        std::stop_source stop;
        const auto nothingLeft = [&] {
            const auto lock = index->readLock();
            const remotecontent::Plan p = remotecontent::plan(*index, root, remotecontent::filterOf(indexer.options()),
                options.sizeLimits, 0, content->documents());
            return p.read.empty() && p.tooLarge.empty() && p.current == p.files;
        };

        QVERIFY(!share.slice(stop.get_token(), root, s)); // all read
        QCOMPARE(share.progress().files, std::size_t {4}); // a, b, c, big: not tool.exe
        QCOMPARE(share.progress().current, std::size_t {4}); // big.txt recorded as too large
        QCOMPARE(share.progress().indexed, std::size_t {4}); // of the content index's documents, the share's
        QVERIFY(!share.progress().reading && !share.progress().held && !share.progress().metered);
        QVERIFY(!told.empty() && told.back() == share.progress());
        QCOMPARE(pauses, 3); // after a, b and c; big.txt was not opened
        QVERIFY(content->knows(a) && content->knows(b) && content->knows(c) && content->knows(big));
        QCOMPARE(matchesOf(*content, u"合同"_s), std::vector<EntryId> {a});
        QCOMPARE(content->lookup(u"x"_s).large, std::vector<EntryId> {big});
        const std::optional<doctext::DocText> text = content->textOf(a);
        QVERIFY(text);
        QCOMPARE(text->text, std::string("合同条款 alpha\n"));
        QVERIFY(!dirty); // as listed: the index kept its sizes and times
        QVERIFY(nothingLeft()); // straight after, the stamps are the listing's

        // Written to: the next listing has another size, and it is read again.
        QVERIFY(write(u"a.txt"_s, "新的内容 beta"_ba));
        {
            auto lock = index->writeLock();
            listed(a, u"a.txt"_s);
        }
        QVERIFY(!share.slice(stop.get_token(), root, s));
        QCOMPARE(matchesOf(*content, u"内容"_s), std::vector<EntryId> {a});
        QVERIFY(matchesOf(*content, u"合同"_s).empty());
        QVERIFY(nothingLeft());

        // Listed halfway through a write: read, and the index takes the size the file has.
        QVERIFY(write(u"sub/c.txt"_s, "deeper text, longer"_ba));
        {
            auto lock = index->writeLock();
            index->setSize(c, packSize(1));
        }
        QVERIFY(!share.slice(stop.get_token(), root, s));
        QVERIFY(dirty);
        QCOMPARE(unpackSize(index->size(c)), std::uint64_t {19});
        QCOMPARE(matchesOf(*content, u"longer"_s), std::vector<EntryId> {c});
        QVERIFY(nothingLeft());

        // Not followed (offline, read again): nothing read or kept, and soon again.
        QVERIFY(write(u"b.md"_s, "新的季度报告"_ba)); // another size: within the same second, the time alone would not tell
        {
            auto lock = index->writeLock();
            listed(b, u"b.md"_s);
        }
        followed = false;
        const auto later = share.slice(stop.get_token(), root, s);
        QVERIFY(later && *later > 0s);
        QCOMPARE(matchesOf(*content, u"新的"_s), std::vector<EntryId> {a}); // not b.md yet
        followed = true;

        // The thread has something else to do: the slice ends after the file.
        goOn = false;
        const auto soon = share.slice(stop.get_token(), root, s);
        QVERIFY(soon && *soon == 0s);
        goOn = true;
        QCOMPARE(matchesOf(*content, u"季度"_s), std::vector<EntryId> {b});

        // A metered connection: names only, contents later.
        QVERIFY(write(u"b.md"_s, "再改一次"_ba));
        {
            auto lock = index->writeLock();
            listed(b, u"b.md"_s);
        }
        qputenv("WINSHUN_METERED", "1");
        const auto metered = share.slice(stop.get_token(), root, s);
        // Metered from the first look on: counted all the same, so that its
        // card says why nothing is read (not "about to start").
        remotecontent::ShareContent meteredFirst(rootPath.toStdWString());
        const auto firstLook = meteredFirst.slice(stop.get_token(), root, s);
        qunsetenv("WINSHUN_METERED");
        QVERIFY(metered && *metered > 0s);
        QVERIFY(share.progress().metered);
        QVERIFY(firstLook && *firstLook > 0s);
        QVERIFY(meteredFirst.progress().known && meteredFirst.progress().metered);
        QCOMPARE(meteredFirst.progress().files, std::size_t {4});
        QVERIFY(matchesOf(*content, u"再改"_s).empty());
        QVERIFY(!share.slice(stop.get_token(), root, s));
        QCOMPARE(matchesOf(*content, u"再改"_s), std::vector<EntryId> {b});

        // On hold (paused, here) with files left: the gates are asked first,
        // without all of the share planned again each time; once it may read,
        // counted anew and read.
        QVERIFY(write(u"b.md"_s, "季度报告 第三版"_ba) && write(u"d.txt"_s, "备忘录"_ba));
        EntryId d = kNoEntry;
        {
            auto lock = index->writeLock();
            listed(b, u"b.md"_s);
        }
        indexer.pause();
        remotecontent::ShareContent held(rootPath.toStdWString());
        const auto heldFirst = held.slice(stop.get_token(), root, s);
        QVERIFY(heldFirst && *heldFirst > 0s);
        QVERIFY(held.progress().held && held.progress().known);
        QCOMPARE(held.progress().files, std::size_t {4});
        QCOMPARE(held.progress().current, std::size_t {3}); // b.md to read again
        {
            auto lock = index->writeLock();
            d = index->add(root, "d.txt", EntryFlag::Remote);
            listed(d, u"d.txt"_s);
        }
        const auto heldAgain = held.slice(stop.get_token(), root, s);
        QVERIFY(heldAgain && *heldAgain > 0s);
        QCOMPARE(held.progress().files, std::size_t {4}); // not planned again: d.txt not counted yet
        indexer.resume();
        QVERIFY(!held.slice(stop.get_token(), root, s));
        QCOMPARE(held.progress().files, std::size_t {5});
        QCOMPARE(held.progress().current, std::size_t {5});
        QCOMPARE(held.progress().indexed, std::size_t {5}); // d.txt new; b.md read again, not counted twice
        QCOMPARE(matchesOf(*content, u"第三版"_s), std::vector<EntryId> {b});
        QCOMPARE(matchesOf(*content, u"备忘"_s), std::vector<EntryId> {d});

        // Content search off: the index is cleared, nothing is read.
        options.enabled = false;
        indexer.setOptions(options);
        QVERIFY(!share.slice(stop.get_token(), root, s));
        QCOMPARE(share.progress(), remotecontent::ShareContent::Progress {});
        QCOMPARE(content->stats().documents, std::size_t {0});
        qunsetenv("WINSHUN_USER");
    }

    // A file written to again and again (a log, one being copied in: a
    // share's watcher tells of each write) is read again at once the first
    // time, then no sooner than readAgain after: the slice says when to come
    // back for it, not "all read".
    void readAgainNotTooOften()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir dir(tmp.path());
        QVERIFY(dir.mkpath(u"share"_s));
        const QString file = dir.filePath(u"share/log.txt"_s);
        const auto write = [&](const QByteArray& bytes) {
            QFile f(file);
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        QVERIFY(write("第一行"_ba));
        const QString rootPath = QDir::toNativeSeparators(dir.filePath(u"share"_s));
        auto index = std::make_shared<FileIndex>();
        EntryId root = kNoEntry, log = kNoEntry;
        {
            auto lock = index->writeLock();
            root = index->addRoot(rootPath.toStdString());
            index->setFlags(root, EntryFlag::Remote);
            log = index->add(root, "log.txt", EntryFlag::Remote);
        }
        const auto listed = [&] { // as the watcher's change leaves it in the index
            const QFileInfo info(file);
            auto lock = index->writeLock();
            index->setSize(log, packSize(static_cast<std::uint64_t>(info.size())));
            index->setModified(log, static_cast<FileTime>(info.lastModified().toSecsSinceEpoch()));
        };
        listed();
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] {
            ContentIndexer::Volume v {rootPath.toStdString(), false};
            v.remote = true;
            return std::vector<ContentIndexer::Volume> {v};
        };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s};
        ContentIndexer indexer(content, source, options, {1h, 1h, 1h, 0ms}); // no pass of its own
        remotecontent::ShareContent::Source s;
        s.index = index;
        s.indexer = &indexer;
        std::stop_source stop;
        remotecontent::ShareContent share(rootPath.toStdWString(), 500ms);

        QVERIFY(!share.slice(stop.get_token(), root, s)); // the first time
        QCOMPARE(matchesOf(*content, u"第一"_s), std::vector<EntryId> {log});
        QVERIFY(write("第二行 longer"_ba)); // another size: the time alone may be the same second
        listed();
        QVERIFY(!share.slice(stop.get_token(), root, s)); // read again at once
        QCOMPARE(matchesOf(*content, u"第二"_s), std::vector<EntryId> {log});

        QVERIFY(write("第三行 longer still"_ba));
        listed();
        const auto later = share.slice(stop.get_token(), root, s);
        QVERIFY(later && *later > 0s && *later <= 500ms); // not yet: back when it is due
        QVERIFY(matchesOf(*content, u"第三"_s).empty());
        QCOMPARE(share.progress().current, std::size_t {0}); // its contents are behind, and say so
        QCOMPARE(share.progress().files, std::size_t {1});
        QTest::qWait(600);
        QVERIFY(!share.slice(stop.get_token(), root, s));
        QCOMPARE(matchesOf(*content, u"第三"_s), std::vector<EntryId> {log});
        QCOMPARE(share.progress().current, std::size_t {1});
    }

    // Asked for writes (a share's watcher, GitHub #114), a watcher tells of a
    // file written to (Modified); one not asked hears of names alone. Then
    // applyChanges takes the file's size and time from it, and nothing from a
    // folder's own or for a file the index has not got. With
    // WINSHUN_TEST_SMB_LOOPBACK=1, over SMB too (\\localhost\Users, this
    // computer's own share of C:\Users), written on either side.
    void writesFromTheWatcher()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::wstring root = QDir::toNativeSeparators(tmp.path()).toStdWString();
        const std::wstring file = root + L"\\log.txt";
        const auto append = [](const std::wstring& path, const QByteArray& bytes) {
            QFile f(QString::fromStdWString(path));
            return f.open(QIODevice::Append) && f.write(bytes) == bytes.size();
        };
        QVERIFY(append(file, "one"_ba));
        QVERIFY(QDir(tmp.path()).mkdir(u"sub"_s));

        std::mutex mutex;
        std::vector<FsChange> told, names;
        ChangeWatcher writes([&](std::vector<FsChange>&& c) {
            std::lock_guard lock(mutex);
            told.insert(told.end(), c.begin(), c.end());
        }, ChangeWatcher::Tell::Writes);
        ChangeWatcher plain([&](std::vector<FsChange>&& c) {
            std::lock_guard lock(mutex);
            names.insert(names.end(), c.begin(), c.end());
        });
        const auto writtenTo = [&](const std::vector<FsChange>& list, const std::wstring& path) {
            std::lock_guard lock(mutex);
            return static_cast<int>(std::ranges::count_if(
                list, [&](const FsChange& c) { return c.kind == FsChange::Kind::Modified && c.path == path; }));
        };
        writes.setRoots({root});
        plain.setRoots({root});
        QCOMPARE(writes.roots(), std::vector<std::wstring> {root});
        QVERIFY(append(file, " two"_ba));
        QTRY_VERIFY_WITH_TIMEOUT(writtenTo(told, file) > 0, 5000);
        QTest::qWait(300);
        QCOMPARE(writtenTo(names, file), 0);

        // The index's entry for it: an old size and time; the folder's own time.
        FileIndex index;
        EntryId log = kNoEntry, sub = kNoEntry;
        {
            auto lock = index.writeLock();
            EntryId parent = index.addRoot(QString::fromStdWString(root.substr(0, 2)).toStdString());
            for (const QString& name : QString::fromStdWString(root.substr(3)).split(u'\\'))
                parent = index.add(parent, name.toStdString(), EntryFlag::Directory);
            log = index.add(parent, "log.txt", 0);
            index.setSize(log, packSize(3));
            index.setModified(log, 1);
            sub = index.add(parent, "sub", EntryFlag::Directory);
            index.setModified(sub, 1);
        }
        QCOMPARE(index.findPath(file), log);
        const Crawler crawler(CrawlRules {});
        applyChanges(index,
            {{FsChange::Kind::Modified, file}, {FsChange::Kind::Modified, root + L"\\sub"},
                {FsChange::Kind::Modified, root + L"\\new.txt"}, {FsChange::Kind::Modified, file}},
            crawler, {});
        QCOMPARE(unpackSize(index.size(log)), std::uint64_t {7});
        QVERIFY(index.modified(log) > 1);
        QCOMPARE(index.modified(sub), FileTime {1}); // a folder's own: left alone
        QCOMPARE(index.findPath(root + L"\\new.txt"), kNoEntry); // its Added brings it

        // Saved by a rename (a temporary file written to, then renamed), or
        // renamed and then written to (a log rotated while written): the
        // name moved takes the file's size and time as they are now.
        const std::wstring part = root + L"\\save.part", saved = root + L"\\saved.txt", rotated = root + L"\\rotated.txt";
        QVERIFY(append(part, "12345"_ba));
        EntryId moved = kNoEntry;
        {
            auto lock = index.writeLock();
            moved = index.add(index.findPath(root), "save.part", 0);
            index.setSize(moved, packSize(1));
            index.setModified(moved, 1);
        }
        QVERIFY(QFile::rename(QString::fromStdWString(part), QString::fromStdWString(saved)));
        applyChanges(index,
            {{FsChange::Kind::Modified, part}, {FsChange::Kind::RenamedFrom, part}, {FsChange::Kind::RenamedTo, saved}},
            crawler, {});
        QCOMPARE(index.findPath(saved), moved);
        QCOMPARE(unpackSize(index.size(moved)), std::uint64_t {5});
        QVERIFY(index.modified(moved) > 1);
        QVERIFY(append(saved, "678"_ba));
        QVERIFY(QFile::rename(QString::fromStdWString(saved), QString::fromStdWString(rotated)));
        applyChanges(index,
            {{FsChange::Kind::RenamedFrom, saved}, {FsChange::Kind::RenamedTo, rotated}, {FsChange::Kind::Modified, rotated}},
            crawler, {});
        QCOMPARE(index.findPath(rotated), moved);
        QCOMPARE(unpackSize(index.size(moved)), std::uint64_t {8});
        // Stopping: nothing more is asked of the disk (or a server).
        QVERIFY(append(rotated, "9"_ba));
        std::stop_source stopping;
        stopping.request_stop();
        applyChanges(index, {{FsChange::Kind::Modified, rotated}}, crawler, stopping.get_token());
        QCOMPARE(unpackSize(index.size(moved)), std::uint64_t {8});
        applyChanges(index, {{FsChange::Kind::Modified, rotated}}, crawler, {});
        QCOMPARE(unpackSize(index.size(moved)), std::uint64_t {9});

        if (qEnvironmentVariableIntValue("WINSHUN_TEST_SMB_LOOPBACK") != 1)
            return;
        const QString users = u"C:\\Users\\"_s;
        QVERIFY2(QString::fromStdWString(root).startsWith(users, Qt::CaseInsensitive), "the temporary folder is not under C:\\Users");
        const std::wstring share = L"\\\\localhost\\Users\\" + root.substr(static_cast<std::size_t>(users.size()));
        QVERIFY(QFileInfo::exists(QString::fromStdWString(share + L"\\log.txt")));
        ChangeWatcher overSmb([&](std::vector<FsChange>&& c) {
            std::lock_guard lock(mutex);
            told.insert(told.end(), c.begin(), c.end());
        }, ChangeWatcher::Tell::Writes);
        overSmb.setRoots({share});
        QCOMPARE(overSmb.roots(), std::vector<std::wstring> {share});
        QElapsedTimer timer;
        timer.start();
        QVERIFY(append(file, " three"_ba)); // on the server's side
        QTRY_VERIFY_WITH_TIMEOUT(writtenTo(told, share + L"\\log.txt") > 0, 10000);
        qInfo("SMB: a write on the server's side told after %lld ms", timer.elapsed());
        const auto before = writtenTo(told, share + L"\\log.txt");
        timer.restart();
        QVERIFY(append(share + L"\\log.txt", " four"_ba)); // through the share
        QTRY_VERIFY_WITH_TIMEOUT(writtenTo(told, share + L"\\log.txt") > before, 10000);
        qInfo("SMB: a write through the share told after %lld ms", timer.elapsed());
    }

    // The share's own thread: reads while the share is Online, again when
    // woken (names came in), and ends when its contents are turned off.
    void shareContentThreads()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir dir(tmp.path());
        QVERIFY(dir.mkpath(u"share"_s));
        const auto write = [&](const QString& name, const QByteArray& bytes) {
            QFile f(dir.filePath(u"share/"_s + name));
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        QVERIFY(write(u"a.txt"_s, "合同条款"_ba) && write(u"b.txt"_s, "季度报告"_ba));
        const QString rootPath = QDir::toNativeSeparators(dir.filePath(u"share"_s));
        auto index = std::make_shared<FileIndex>();
        const auto add = [&](EntryId parent, const QString& name) {
            const QFileInfo info(dir.filePath(u"share/"_s + name));
            auto lock = index->writeLock();
            const EntryId id = index->add(parent, name.toStdString(), EntryFlag::Remote);
            index->setSize(id, packSize(static_cast<std::uint64_t>(info.size())));
            index->setModified(id, static_cast<FileTime>(info.lastModified().toSecsSinceEpoch()));
            return id;
        };
        EntryId root = kNoEntry;
        {
            auto lock = index->writeLock();
            root = index->addRoot(rootPath.toStdString());
            index->setFlags(root, EntryFlag::Remote);
        }
        const EntryId a = add(root, u"a.txt"_s);
        const EntryId b = add(root, u"b.txt"_s);
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        std::atomic<bool> online = false;
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] {
            ContentIndexer::Volume v {rootPath.toStdString(), !online};
            v.remote = true;
            return std::vector<ContentIndexer::Volume> {v};
        };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s};
        ContentIndexer indexer(content, source, options, {1h, 1h, 1h, 0ms}); // no pass of its own

        std::atomic<int> changes = 0;
        remotecontent::Indexer::Source s;
        s.index = [&] { return index; };
        s.indexer = &indexer;
        s.online = [&](const std::wstring& path) { return online.load() && unc::equal(path, rootPath.toStdWString()); };
        remotecontent::Indexer threads(s, {[&] { ++changes; }, [] {}}, {10ms, 300ms});
        threads.setShares({rootPath.toStdWString()});
        QCOMPARE(threads.statuses().size(), std::size_t {1});
        threads.start();
        QTest::qWait(400); // offline: nothing read, looked at again after 300 ms
        QVERIFY(!content->knows(a) && !content->knows(b));

        online = true;
        threads.wake();
        QTRY_VERIFY_WITH_TIMEOUT(content->knows(a) && content->knows(b), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!threads.statuses().front().progress.reading, 5000);
        const remotecontent::ShareContent::Progress done = threads.statuses().front().progress;
        QCOMPARE(done.files, std::size_t {2});
        QCOMPARE(done.current, std::size_t {2});
        QVERIFY(changes > 0);
        QCOMPARE(matchesOf(*content, u"合同"_s), std::vector<EntryId> {a});

        // A file that came in live: read once woken.
        QVERIFY(write(u"c.txt"_s, "会议纪要"_ba));
        const EntryId c = add(root, u"c.txt"_s);
        threads.wake();
        QTRY_VERIFY_WITH_TIMEOUT(content->knows(c), 10000);
        QCOMPARE(matchesOf(*content, u"纪要"_s), std::vector<EntryId> {c});

        // Contents turned off for the share: its thread ends.
        threads.setShares({});
        QVERIFY(threads.statuses().empty());
        threads.stop();
        threads.setShares({rootPath.toStdWString()}); // stopped: nothing starts
        QVERIFY(threads.statuses().empty());
    }

    // The share indexer's statuses change often (a check under way, a share
    // tried again while it stays offline: GitHub #116). A share's thread then
    // looks at its own share's state, and reads only when that changed (Online,
    // or no longer); nor do the nudges put off its look every so often.
    void shareThreadsNudged()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir dir(tmp.path());
        QVERIFY(dir.mkpath(u"share"_s));
        const auto write = [&](const QString& name, const QByteArray& bytes) {
            QFile f(dir.filePath(u"share/"_s + name));
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        QVERIFY(write(u"a.txt"_s, "合同条款"_ba));
        const QString rootPath = QDir::toNativeSeparators(dir.filePath(u"share"_s));
        auto index = std::make_shared<FileIndex>();
        EntryId root = kNoEntry, a = kNoEntry;
        {
            auto lock = index->writeLock();
            root = index->addRoot(rootPath.toStdString());
            index->setFlags(root, EntryFlag::Remote);
            a = index->add(root, "a.txt", EntryFlag::Remote);
        }
        // Written to, and listed so (its size and time) without a word: no
        // wake, as when a pass finds it changed and the thread is not told.
        const auto rewrite = [&](const QByteArray& bytes) {
            if (!write(u"a.txt"_s, bytes))
                return false;
            const QFileInfo info(dir.filePath(u"share/a.txt"_s));
            auto lock = index->writeLock();
            index->setSize(a, packSize(static_cast<std::uint64_t>(info.size())));
            index->setModified(a, static_cast<FileTime>(info.lastModified().toSecsSinceEpoch()));
            return true;
        };
        QVERIFY(rewrite("合同条款"_ba));
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        std::atomic<bool> online = true;
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] {
            ContentIndexer::Volume v {rootPath.toStdString(), !online};
            v.remote = true;
            return std::vector<ContentIndexer::Volume> {v};
        };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s};
        ContentIndexer indexer(content, source, options, {1h, 1h, 1h, 0ms}); // no pass of its own

        std::atomic<int> asked = 0; // a look asks first whether the share is Online
        remotecontent::Indexer::Source s;
        s.index = [&] { return index; };
        s.indexer = &indexer;
        s.online = [&](const std::wstring& path) {
            ++asked;
            return online.load() && unc::equal(path, rootPath.toStdWString());
        };
        // Until the thread has looked (and gone by what it found).
        const auto nudge = [&](remotecontent::Indexer& threads) {
            const int before = asked.load();
            threads.statusesChanged();
            return QTest::qWaitFor([&] { return asked.load() > before; }, 5000);
        };

        {
            remotecontent::Indexer threads(s, {[] {}, [] {}}, {10ms, 1h});
            threads.setShares({rootPath.toStdWString()});
            threads.start();
            QTRY_COMPARE_WITH_TIMEOUT(matchesOf(*content, u"合同"_s), std::vector<EntryId> {a}, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(!threads.statuses().front().progress.reading, 5000);

            QVERIFY(rewrite("会议纪要 第一版"_ba));
            for (int i = 0; i < 5; ++i)
                QVERIFY(nudge(threads)); // a check, a retry: nothing its share's thread goes by
            QTest::qWait(300);
            QCOMPARE(matchesOf(*content, u"合同"_s), std::vector<EntryId> {a}); // not read again

            online = false; // offline, and back: read
            QVERIFY(nudge(threads));
            online = true;
            QVERIFY(nudge(threads));
            QTRY_COMPARE_WITH_TIMEOUT(matchesOf(*content, u"纪要"_s), std::vector<EntryId> {a}, 10000);
        }

        // A write on another share (its watcher's, IndexService) wakes that
        // share's thread alone: this one sleeps on; its own wakes it.
        {
            remotecontent::Indexer threads(s, {[] {}, [] {}}, {10ms, 1h, 0s});
            threads.setShares({rootPath.toStdWString()});
            threads.start();
            QTRY_VERIFY_WITH_TIMEOUT(!threads.statuses().empty() && threads.statuses().front().progress.known
                    && threads.statuses().front().progress.current == 1 && !threads.statuses().front().progress.reading,
                5000); // its first look: all current
            QVERIFY(rewrite("修订版 第三稿"_ba));
            threads.wake(L"\\\\elsewhere\\share");
            QTest::qWait(300);
            QVERIFY(matchesOf(*content, u"第三稿"_s).empty());
            threads.wake(rootPath.toStdWString());
            QTRY_COMPARE_WITH_TIMEOUT(matchesOf(*content, u"第三稿"_s), std::vector<EntryId> {a}, 10000);
        }

        // Nudged every 100 ms, the look every 600 ms comes all the same.
        remotecontent::Indexer threads(s, {[] {}, [] {}}, {10ms, 600ms});
        threads.setShares({rootPath.toStdWString()});
        const int before = asked.load();
        threads.start();
        QTRY_VERIFY_WITH_TIMEOUT(asked.load() > before, 5000);
        QTest::qWait(200); // its first look is over: all read, the next 600 ms on
        QVERIFY(rewrite("会议纪要 第二版 新增"_ba));
        QElapsedTimer timer;
        timer.start();
        bool read = false;
        while (!read && timer.elapsed() < 5000) {
            threads.statusesChanged();
            QTest::qWait(100);
            read = matchesOf(*content, u"新增"_s) == std::vector<EntryId> {a};
        }
        QVERIFY(read);
    }

    // The local index off, a share's contents on (ContentIndexer::Options::
    // local): the local documents go, the share's stay; the share's turned
    // off too: they go at once; content search off: cleared.
    void localOffRemoteOn()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir dir(tmp.path());
        QVERIFY(dir.mkpath(u"local"_s) && dir.mkpath(u"share"_s));
        const auto write = [&](const QString& name, const QByteArray& bytes) {
            QFile f(dir.filePath(name));
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        QVERIFY(write(u"local/a.txt"_s, "本机合同"_ba) && write(u"share/b.txt"_s, "远程合同"_ba));
        const std::string localRoot = QDir::toNativeSeparators(dir.filePath(u"local"_s)).toStdString();
        const QString sharePath = QDir::toNativeSeparators(dir.filePath(u"share"_s));
        auto index = std::make_shared<FileIndex>();
        EntryId a = kNoEntry, b = kNoEntry, shareRoot = kNoEntry;
        {
            auto lock = index->writeLock();
            a = index->add(index->addRoot(localRoot), "a.txt", 0);
            shareRoot = index->addRoot(sharePath.toStdString());
            index->setFlags(shareRoot, EntryFlag::Remote);
            b = index->add(shareRoot, "b.txt", EntryFlag::Remote);
            const QFileInfo info(dir.filePath(u"share/b.txt"_s));
            index->setSize(b, packSize(static_cast<std::uint64_t>(info.size())));
            index->setModified(b, static_cast<FileTime>(info.lastModified().toSecsSinceEpoch()));
        }
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        std::atomic<bool> shareOn = true;
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] {
            std::vector<ContentIndexer::Volume> volumes {{localRoot, false}};
            if (shareOn) {
                ContentIndexer::Volume share {sharePath.toStdString(), false};
                share.remote = true;
                volumes.push_back(share);
            }
            return volumes;
        };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s};
        ContentIndexer indexer(content, source, options, {0ms, 1h, 1h, 0ms}); // woken: an hour's wait for more
        QTRY_VERIFY_WITH_TIMEOUT(content->knows(a), 10000); // the local pass
        QVERIFY(!content->knows(b)); // not the share's: its own thread reads it
        remotecontent::ShareContent share(sharePath.toStdWString());
        remotecontent::ShareContent::Source s;
        s.index = index;
        s.indexer = &indexer;
        std::stop_source stop;
        QVERIFY(!share.slice(stop.get_token(), shareRoot, s));
        QVERIFY(content->knows(b));

        options.local = false;
        indexer.setOptions(options);
        QTRY_VERIFY_WITH_TIMEOUT(!content->knows(a), 10000);
        QVERIFY(content->knows(b));
        QCOMPARE(matchesOf(*content, u"合同"_s), std::vector<EntryId> {b});

        // The share's contents turned off (IndexService::setOptions): its
        // documents go at once, not after the wait for more changes.
        shareOn = false;
        indexer.volumesChanged();
        QTRY_VERIFY_WITH_TIMEOUT(!content->knows(b), 5000);

        options.enabled = false; // as App sets it once no share has its contents on either
        indexer.setOptions(options);
        QCOMPARE(content->stats().documents, std::size_t {0});
    }

    // A file that cannot be read for itself (in use, here) is tried again
    // after failedAgain, then twice that, and after kTries it is recorded as
    // unreadable: not opened every round for ever, the count not stuck short
    // of it. Written to since, it is tried afresh.
    void unreadableFilesBackedOff()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir dir(tmp.path());
        QVERIFY(dir.mkpath(u"share"_s));
        const QString file = dir.filePath(u"share/a.txt"_s);
        const auto write = [&](const QByteArray& bytes) {
            QFile f(file);
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        QVERIFY(write("合同条款"_ba));
        const QString sharePath = QDir::toNativeSeparators(dir.filePath(u"share"_s));
        auto index = std::make_shared<FileIndex>();
        EntryId root = kNoEntry, a = kNoEntry;
        const auto listed = [&] {
            const QFileInfo info(file);
            auto lock = index->writeLock();
            index->setSize(a, packSize(static_cast<std::uint64_t>(info.size())));
            index->setModified(a, static_cast<FileTime>(info.lastModified().toSecsSinceEpoch()));
        };
        {
            auto lock = index->writeLock();
            root = index->addRoot(sharePath.toStdString());
            index->setFlags(root, EntryFlag::Remote);
            a = index->add(root, "a.txt", EntryFlag::Remote);
        }
        listed();
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] {
            ContentIndexer::Volume v {sharePath.toStdString(), false};
            v.remote = true;
            return std::vector<ContentIndexer::Volume> {v};
        };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s};
        ContentIndexer indexer(content, source, options, {1h, 1h, 1h, 0ms}); // no pass of its own
        remotecontent::ShareContent share(sharePath.toStdWString(), 0s, 200ms);
        remotecontent::ShareContent::Source s;
        s.index = index;
        s.indexer = &indexer;
        std::stop_source stop;

        // Open without sharing: any other open fails (ERROR_SHARING_VIOLATION).
        HANDLE held = ::CreateFileW(QDir::toNativeSeparators(file).toStdWString().c_str(), GENERIC_READ, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(held != INVALID_HANDLE_VALUE);
        const auto release = [&] {
            if (held != INVALID_HANDLE_VALUE)
                ::CloseHandle(std::exchange(held, INVALID_HANDLE_VALUE));
        };
        const auto closeAtEnd = qScopeGuard(release);
        const auto slice = [&] { return share.slice(stop.get_token(), root, s); };
        const auto ms = [](const std::optional<remotecontent::ShareContent::Clock::duration>& d) {
            return d ? QString::number(std::chrono::duration_cast<std::chrono::milliseconds>(*d).count()) : u"none"_s;
        };
        auto due = slice();
        QVERIFY2(due && *due > 0ms && *due <= 200ms, qPrintable(ms(due))); // back for it then
        QCOMPARE(share.progress().files, std::size_t {1});
        QCOMPARE(share.progress().current, std::size_t {0}); // not read: not counted
        QCOMPARE(share.progress().indexed, std::size_t {0});
        QVERIFY(!content->knows(a));
        due = slice(); // too soon: not opened, the wait as it was
        QVERIFY2(due && *due > 0ms && *due <= 200ms, qPrintable(ms(due)));
        QTest::qWait(250);
        due = slice(); // the second try: twice as long
        QVERIFY2(due && *due > 200ms && *due <= 400ms, qPrintable(ms(due)));
        QCOMPARE(share.progress().current, std::size_t {0});
        QTest::qWait(450);
        QVERIFY(!slice()); // the third: unreadable, all done
        QVERIFY(content->knows(a));
        QCOMPARE(share.progress().current, std::size_t {1});
        QCOMPARE(share.progress().indexed, std::size_t {1});
        QVERIFY(matchesOf(*content, u"合同"_s).empty());
        QVERIFY(!slice()); // and not tried again

        // Written to: tried afresh, and read.
        release();
        QVERIFY(write("会议纪要 第一版"_ba));
        listed();
        QVERIFY(!slice());
        QCOMPARE(matchesOf(*content, u"纪要"_s), std::vector<EntryId> {a});
        QCOMPARE(share.progress().current, std::size_t {1});
        QCOMPARE(share.progress().indexed, std::size_t {1});
    }

    // A content search over a share's files: what the content index has of
    // them answers (the file is not opened: gone from the disk, it is found
    // all the same), and those it has not read are counted, not read.
    void searchFromTheIndex()
    {
        QTemporaryDir tmp;
        QTemporaryDir store;
        QVERIFY(tmp.isValid() && store.isValid());
        QDir dir(tmp.path());
        QVERIFY(dir.mkpath(u"share"_s));
        const auto write = [&](const QString& name, const QByteArray& bytes) {
            QFile f(dir.filePath(u"share/"_s + name));
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        QVERIFY(write(u"a.txt"_s, "合同条款 alpha\n第二行 合同"_ba) && write(u"c.txt"_s, "合同 too"_ba));
        const QString sharePath = QDir::toNativeSeparators(dir.filePath(u"share"_s));
        IndexService::Options serviceOptions;
        IndexService service(store.path(), serviceOptions); // not started: its own content indexer reads nothing
        const auto index = service.index();
        const auto add = [&](EntryId parent, const QString& name) {
            const QFileInfo info(dir.filePath(u"share/"_s + name));
            auto lock = index->writeLock();
            const EntryId id = index->add(parent, name.toStdString(), EntryFlag::Remote);
            index->setSize(id, packSize(static_cast<std::uint64_t>(info.size())));
            index->setModified(id, static_cast<FileTime>(info.lastModified().toSecsSinceEpoch()));
            return id;
        };
        EntryId root = kNoEntry;
        {
            auto lock = index->writeLock();
            root = index->addRoot(sharePath.toStdString());
            index->setFlags(root, EntryFlag::Remote);
        }
        const EntryId a = add(root, u"a.txt"_s);
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] {
            ContentIndexer::Volume v {sharePath.toStdString(), false};
            v.remote = true;
            return std::vector<ContentIndexer::Volume> {v};
        };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s};
        ContentIndexer indexer(std::const_pointer_cast<ContentIndex>(service.contentIndex()), source, options, {1h, 1h, 1h, 0ms});
        remotecontent::ShareContent share(sharePath.toStdWString());
        remotecontent::ShareContent::Source reader;
        reader.index = index;
        reader.indexer = &indexer;
        std::stop_source stop;
        QVERIFY(!share.slice(stop.get_token(), root, reader));
        add(root, u"c.txt"_s); // listed since: not read yet
        QVERIFY(QFile::remove(dir.filePath(u"share/a.txt"_s)));

        SearchEngine engine(&service);
        QStringList found;
        QStringList lines;
        std::optional<ContentLeftOut> leftOut;
        bool finished = false;
        quint64 asked = 0;
        QObject receiver;
        QObject::connect(&engine, &SearchEngine::contentResults, &receiver, [&](quint64 id, const SearchResults& results) {
            if (id != asked)
                return;
            for (const SearchResult& r : results) {
                found.append(r.path);
                lines.append(QString::number(r.line));
                QVERIFY(!r.offline); // no share indexer knows it: not offline
            }
        });
        QObject::connect(&engine, &SearchEngine::contentLeftOut, &receiver,
            [&](quint64 id, const ContentLeftOut& l) { if (id == asked) leftOut = l; });
        QObject::connect(&engine, &SearchEngine::contentProgress, &receiver,
            [&](quint64 id, int, int, bool done) { if (id == asked && done) finished = true; });
        SearchEngine::Request request;
        request.text = u"合同"_s;
        request.scope = Scope::Content;
        request.contentExtensions = {u"txt"_s};
        request.contentThreads = 1;
        asked = engine.submit(std::move(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
        QCOMPARE(found, QStringList {sharePath + u"\\a.txt"_s});
        QCOMPARE(lines, QStringList {u"1"_s}); // the first line that has it
        QVERIFY(leftOut);
        QCOMPARE(leftOut->remote, 1); // c.txt

        // Listed with another size since it was read (not read again yet,
        // ShareContent::kReadAgain): its old text claims no match; it is
        // counted as not read, as c.txt is.
        {
            auto lock = index->writeLock();
            index->setSize(a, packSize(999));
        }
        found.clear();
        lines.clear();
        leftOut.reset();
        finished = false;
        SearchEngine::Request again;
        again.text = u"合同"_s;
        again.scope = Scope::Content;
        again.contentExtensions = {u"txt"_s};
        again.contentThreads = 1;
        asked = engine.submit(std::move(again));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
        QVERIFY2(found.isEmpty(), qPrintable(found.join(u", "_s)));
        QVERIFY(leftOut);
        QCOMPARE(leftOut->remote, 2); // a.txt and c.txt
    }

    // A WebDAV server for real, when one is given (WINSHUN_TEST_WEBDAV=
    // "127.0.0.1@8714": WebClient's form of its host). Its root then holds
    // hello.txt, small\ and many\ (6000 files) and two large files.
    void webDavLive()
    {
        const QString host = qEnvironmentVariable("WINSHUN_TEST_WEBDAV");
        if (host.isEmpty())
            QSKIP("no WebDAV server given (WINSHUN_TEST_WEBDAV)");
        const std::wstring path = L"\\\\" + host.toStdWString() + L"\\DavWWWRoot";
        QVERIFY(share::reachablePath(path, true));
        QVERIFY(!share::reachablePath(L"\\\\127.0.0.1@1\\DavWWWRoot", true)); // nothing there

        auto index = std::make_shared<FileIndex>();
        ShareIndexer::Options o;
        o.shares = {{path, {}}};
        o.verifyEvery = 0min;
        o.verifyEveryUnwatched = 0min;
        ShareIndexer shares({[&] { return index; }, [] { return true; }}, o, {});
        QElapsedTimer timer;
        timer.start();
        shares.start();
        ShareIndexer::Status s;
        for (int i = 0; i < 600 && s.state != ShareIndexer::State::Online; ++i) { // 60 s
            std::this_thread::sleep_for(100ms);
            const auto all = shares.statuses();
            if (!all.empty())
                s = all.front();
        }
        QCOMPARE(s.state, ShareIndexer::State::Online);
        qInfo("WebDAV: %zu items in %lld ms, watching %d", s.items, timer.elapsed(), int(s.watching));
        QVERIFY(s.items >= 7005);
        QVERIFY(!s.watching); // WebDAV tells of no changes
        {
            const auto lock = index->readLock();
            // Names as listed, without the NUL the by-handle listing ends them with.
            for (const wchar_t* name : {L"\\hello.txt", L"\\many\\m5999.txt", L"\\big50.bin"}) {
                const EntryId id = index->findPath(path + name);
                QVERIFY2(id != kNoEntry, qPrintable(QString::fromWCharArray(name)));
                QVERIFY(index->entry(id).flags & EntryFlag::Remote);
            }
            QCOMPARE(unpackSize(index->size(index->findPath(path + L"\\hello.txt"))), std::uint64_t {23});
        }
        shares.stop();

        // Not watched, a WebDAV share asks its server whether it is still
        // there now and then (Options::webDavPoll) and when the launcher
        // opens (lookedAt, at most every lookedAtAfter), reading nothing
        // while it is. Gone (the probe, faked here, says so), it is offline
        // at the next ask; back, read once (GitHub #114). The real one lists.
        {
            const std::wstring folder = L"\\\\" + host.toStdWString() + L"\\small";
            std::atomic<bool> up {true};
            std::atomic<int> asked {0};
            const auto make = [&](std::chrono::milliseconds poll, std::chrono::milliseconds after) {
                auto own = std::make_shared<FileIndex>();
                ShareIndexer::Source source {[own] { return own; }, [] { return true; }};
                source.reachable = [&](const std::wstring&, bool) {
                    ++asked;
                    return up.load();
                };
                ShareIndexer::Options po;
                po.shares = {{folder, {}}};
                po.verifyEvery = 0min;
                po.verifyEveryUnwatched = 0min;
                po.webDavPoll = poll;
                po.lookedAtAfter = after;
                return std::make_unique<ShareIndexer>(source, po, ShareIndexer::Callbacks {});
            };
            const auto stateOf = [](const ShareIndexer& s) {
                const auto all = s.statuses();
                return all.empty() ? ShareIndexer::Status {} : all.front();
            };
            const auto online = [&](ShareIndexer& s) {
                s.start();
                for (int i = 0; i < 300 && stateOf(s).state != ShareIndexer::State::Online; ++i)
                    std::this_thread::sleep_for(100ms);
                return stateOf(s).state == ShareIndexer::State::Online;
            };

            // One missed answer is not offline: asked again, fresh (here when
            // the launcher opens again, else after kDavAskAgain), before it is.
            auto looked = make(1h, 0ms);
            QVERIFY(online(*looked));
            const FileTime readAt = stateOf(*looked).lastVerified;
            up = false;
            const int first = asked.load();
            looked->lookedAt();
            QTRY_VERIFY_WITH_TIMEOUT(asked.load() > first, 5000);
            QTest::qWait(200);
            QCOMPARE(stateOf(*looked).state, ShareIndexer::State::Online);
            looked->lookedAt();
            QTRY_COMPARE_WITH_TIMEOUT(stateOf(*looked).state, ShareIndexer::State::Problem, 5000);
            QCOMPARE(stateOf(*looked).problem, share::Problem::Offline);
            // Back: as it was, not read in full again (offline for the probe
            // alone, nothing missed that the next pass would not find).
            up = true;
            looked->lookedAt();
            QTRY_COMPARE_WITH_TIMEOUT(stateOf(*looked).state, ShareIndexer::State::Online, 10000);
            QCOMPARE(stateOf(*looked).lastVerified, readAt);
            looked->stop();

            auto polled = make(300ms, 1h);
            QVERIFY(online(*polled));
            const FileTime once = stateOf(*polled).lastVerified;
            int before = asked.load();
            QTest::qWait(1500);
            QVERIFY2(asked.load() >= before + 3, qPrintable(QString::number(asked.load() - before)));
            QCOMPARE(stateOf(*polled).lastVerified, once); // nothing read meanwhile
            up = false;
            QTRY_COMPARE_WITH_TIMEOUT(stateOf(*polled).state, ShareIndexer::State::Problem, 3000);
            polled->stop();

            up = true;
            auto quiet = make(1h, 60s); // as the app has it
            QVERIFY(online(*quiet));
            before = asked.load();
            quiet->lookedAt(); // asked a moment ago, as it was read
            QTest::qWait(500);
            QCOMPARE(asked.load(), before);
            quiet->stop();
        }

        // Contents (GitHub #114), of a few of its files as listed: read through
        // WebClient, the large ones (over what a text file is read whole to,
        // one over WebClient's 50,000,000-byte limit too) recorded as too
        // large without being opened. Then a server that does not answer:
        // three network errors and the slice ends, the error kept.
        const std::wstring dead = L"\\\\127.0.0.1@1\\DavWWWRoot";
        auto files = std::make_shared<FileIndex>();
        EntryId root = kNoEntry, deadRoot = kNoEntry, hello = kNoEntry, big40 = kNoEntry, big50 = kNoEntry;
        {
            const auto from = index->readLock();
            auto lock = files->writeLock();
            root = files->addRoot(QString::fromStdWString(path).toStdString());
            files->setFlags(root, EntryFlag::Remote);
            const auto copy = [&](const wchar_t* name) {
                const EntryId listed = index->findPath(path + L"\\" + name);
                const EntryId id = files->add(root, QString::fromWCharArray(name).toStdString(), EntryFlag::Remote);
                files->setSize(id, index->size(listed));
                files->setModified(id, index->modified(listed));
                return id;
            };
            hello = copy(L"hello.txt");
            big40 = copy(L"big40.bin");
            big50 = copy(L"big50.bin");
            deadRoot = files->addRoot(QString::fromStdWString(dead).toStdString());
            files->setFlags(deadRoot, EntryFlag::Remote);
            for (const char* name : {"a.txt", "b.txt", "c.txt", "d.txt"}) {
                const EntryId id = files->add(deadRoot, name, EntryFlag::Remote);
                files->setSize(id, packSize(10));
                files->setModified(id, files->modified(hello));
            }
        }
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return files; };
        source.volumes = [&] {
            std::vector<ContentIndexer::Volume> volumes;
            for (const std::wstring& p : {path, dead}) {
                ContentIndexer::Volume v {QString::fromStdWString(p).toStdString(), false};
                v.remote = true;
                volumes.push_back(v);
            }
            return volumes;
        };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s, u"bin"_s};
        options.sizeLimits.bytes.fill(100ll << 20); // WebClient's limit is the lower one
        ContentIndexer indexer(content, source, options, {1h, 1h, 1h, 0ms}); // no pass of its own
        remotecontent::ShareContent::Source reader;
        reader.index = files;
        reader.indexer = &indexer;
        std::stop_source stop;
        remotecontent::ShareContent share(path);
        timer.restart();
        // The content indexer's own first look, just started (it has nothing
        // to read), tidies up meanwhile: the shares wait for that (held).
        std::optional<remotecontent::ShareContent::Clock::duration> rest;
        for (int i = 0; i < 50; ++i) {
            rest = share.slice(stop.get_token(), root, reader);
            if (!rest || !share.progress().held)
                break;
            QTest::qWait(100);
        }
        qInfo("WebDAV contents: %zu/%zu in %lld ms, error %lu, back in %lld ms", share.progress().current,
            share.progress().files, timer.elapsed(), share.progress().error,
            rest ? static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(*rest).count()) : -1ll);
        QVERIFY(!rest);
        QCOMPARE(share.progress().files, std::size_t {3});
        QCOMPARE(share.progress().current, std::size_t {3});
        const std::optional<doctext::DocText> text = content->textOf(hello);
        QVERIFY(text);
        QVERIFY(text->text.starts_with("hello webdav"));
        QVERIFY(content->knows(big40) && content->knows(big50));
        std::vector<EntryId> large = content->lookup(u"x"_s).large;
        std::ranges::sort(large);
        QCOMPARE(large, (std::vector<EntryId> {std::min(big40, big50), std::max(big40, big50)}));

        remotecontent::ShareContent gone(dead);
        timer.restart();
        const auto later = gone.slice(stop.get_token(), deadRoot, reader);
        qInfo("WebDAV server away: error %lu after %lld ms", gone.progress().error, timer.elapsed());
        QVERIFY(later && *later > 0s);
        QCOMPARE(remotecontent::readErrorOf(gone.progress().error), remotecontent::ReadError::Network);
        QCOMPARE(gone.progress().current, std::size_t {0});

        // A drive mapped to it (net use R: http://... /persistent:no) stands for
        // it, in the one form (Windows says "\\host\DavWWWRoot" for the drive).
        if (const QString letter = qEnvironmentVariable("WINSHUN_TEST_WEBDAV_DRIVE"); !letter.isEmpty()) {
            QCOMPARE(QString::fromStdWString(unc::fromMappedDrive(letter.toStdWString() + L"\\many")),
                QString::fromStdWString(L"\\\\" + host.toStdWString() + L"\\many"));
        }
    }
};

QTEST_GUILESS_MAIN(RemoteContentTest)
#include "tst_remotecontent.moc"
