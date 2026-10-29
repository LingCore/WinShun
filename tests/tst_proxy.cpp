#include "ProxyAddress.h"
#include "platform/Http.h"

#include <QTest>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

// A server on a port of 127.0.0.1 that answers every request with `body`, and
// keeps the first line of each ("GET http://127.0.0.1:1234/x HTTP/1.1").
class Server {
public:
    explicit Server(std::string body)
        : m_body(std::move(body))
    {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
        m_listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(m_listen, reinterpret_cast<sockaddr*>(&address), sizeof address);
        ::listen(m_listen, 8);
        int length = sizeof address;
        ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&address), &length);
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { serve(); });
    }
    ~Server()
    {
        m_stop = true;
        ::closesocket(m_listen);
        m_thread.join();
        ::WSACleanup();
    }
    int port() const { return m_port; }
    std::vector<std::string> requests() const
    {
        const std::lock_guard lock(m_lock);
        return m_requests;
    }

private:
    void serve()
    {
        while (!m_stop) {
            const SOCKET client = ::accept(m_listen, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                return;
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const int n = ::recv(client, buffer, sizeof buffer, 0);
                if (n <= 0)
                    break;
                request.append(buffer, static_cast<std::size_t>(n));
            }
            {
                const std::lock_guard lock(m_lock);
                m_requests.push_back(request.substr(0, request.find("\r\n")));
            }
            const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(m_body.size())
                + "\r\nConnection: close\r\n\r\n" + m_body;
            ::send(client, reply.data(), static_cast<int>(reply.size()), 0);
            ::closesocket(client);
        }
    }

    std::string m_body;
    SOCKET m_listen = INVALID_SOCKET;
    int m_port = 0;
    std::atomic<bool> m_stop = false;
    mutable std::mutex m_lock;
    std::vector<std::string> m_requests;
    std::thread m_thread;
};

QString unusedPort()
{
    WSADATA data;
    ::WSAStartup(MAKEWORD(2, 2), &data);
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof address);
    int length = sizeof address;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&address), &length);
    const int port = ntohs(address.sin_port);
    ::closesocket(s); // nobody listens on it now
    ::WSACleanup();
    return QString::number(port);
}

} // namespace

// The proxy setting (GitHub #84): what the address field accepts, and that
// WinHTTP really goes where the setting says, changed between two requests.
class TestProxy : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { http::setProxy(http::Proxy::System); }

    void addressesAreChecked_data()
    {
        using P = proxy::Problem;
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("problem");
        QTest::addColumn<QString>("server");
        QTest::newRow("ip and port") << u"127.0.0.1:7890"_s << int(P::None) << u"127.0.0.1:7890"_s;
        QTest::newRow("spaces round") << u"  127.0.0.1:7890  "_s << int(P::None) << u"127.0.0.1:7890"_s;
        QTest::newRow("http scheme") << u"http://127.0.0.1:7890"_s << int(P::None) << u"127.0.0.1:7890"_s;
        QTest::newRow("HTTP scheme, slash") << u"HTTP://Proxy.Example.com:8080/"_s << int(P::None) << u"proxy.example.com:8080"_s;
        QTest::newRow("name") << u"localhost:1080"_s << int(P::None) << u"localhost:1080"_s;
        QTest::newRow("ipv6") << u"[::1]:7890"_s << int(P::None) << u"[::1]:7890"_s;
        QTest::newRow("highest port") << u"a.b:65535"_s << int(P::None) << u"a.b:65535"_s;
        QTest::newRow("empty") << QString() << int(P::Empty) << QString();
        QTest::newRow("blank") << u"   "_s << int(P::Empty) << QString();
        QTest::newRow("no port") << u"127.0.0.1"_s << int(P::NoPort) << QString();
        QTest::newRow("empty port") << u"127.0.0.1:"_s << int(P::NoPort) << QString();
        QTest::newRow("ipv6 no port") << u"[::1]"_s << int(P::NoPort) << QString();
        QTest::newRow("port zero") << u"127.0.0.1:0"_s << int(P::BadPort) << QString();
        QTest::newRow("port too big") << u"127.0.0.1:65536"_s << int(P::BadPort) << QString();
        QTest::newRow("port text") << u"127.0.0.1:abc"_s << int(P::BadPort) << QString();
        QTest::newRow("port signed") << u"127.0.0.1:-80"_s << int(P::BadPort) << QString();
        QTest::newRow("socks5") << u"socks5://127.0.0.1:7890"_s << int(P::Socks) << QString();
        QTest::newRow("socks4") << u"SOCKS4://127.0.0.1:7890"_s << int(P::Socks) << QString();
        QTest::newRow("https") << u"https://127.0.0.1:7890"_s << int(P::OtherScheme) << QString();
        QTest::newRow("ftp") << u"ftp://127.0.0.1:21"_s << int(P::OtherScheme) << QString();
        QTest::newRow("credentials") << u"user:pass@127.0.0.1:7890"_s << int(P::Credentials) << QString();
        QTest::newRow("a path") << u"127.0.0.1:7890/pac"_s << int(P::BadAddress) << QString();
        QTest::newRow("spaces inside") << u"127.0.0.1 7890"_s << int(P::BadAddress) << QString();
        QTest::newRow("bare ipv6") << u"::1:7890"_s << int(P::BadAddress) << QString();
        QTest::newRow("junk host") << u"he llo!:80"_s << int(P::BadAddress) << QString();
        QTest::newRow("chinese host") << u"代理:80"_s << int(P::BadAddress) << QString();
        QTest::newRow("empty label") << u"a..b:80"_s << int(P::BadAddress) << QString();
        QTest::newRow("only a scheme") << u"http://"_s << int(P::BadAddress) << QString();
    }
    void addressesAreChecked()
    {
        QFETCH(QString, text);
        QFETCH(int, problem);
        QFETCH(QString, server);
        const proxy::Address address = proxy::parse(text);
        QCOMPARE(int(address.problem), problem);
        QCOMPARE(address.server, server);
        QCOMPARE(address.ok(), problem == int(proxy::Problem::None));
    }

    void namedProxyCarriesTheRequestAndTheChangeNeedsNoRestart_data()
    {
        QTest::addColumn<bool>("shared");
        QTest::newRow("own session") << false;
        QTest::newRow("shared session") << true;
    }
    void namedProxyCarriesTheRequestAndTheChangeNeedsNoRestart()
    {
        QFETCH(bool, shared);
        const http::Session session = shared ? http::Session::Shared : http::Session::Own;
        Server proxyServer("from-proxy");
        // A name that exists nowhere: only a proxy can answer for it. (WinHTTP
        // never sends a loopback address to a proxy, so 127.0.0.1 would prove nothing.)
        const QString url = u"http://origin.winshun-test.example:8080/some/path?x=1"_s;

        // None: direct, and there is no such host.
        http::setProxy(http::Proxy::None);
        http::Response r = http::get(url, {}, 1500, 0, session);
        QCOMPARE(r.status, 0);
        QVERIFY(r.error != 0);
        QVERIFY(proxyServer.requests().empty());

        // Named: the next request goes through it, with the whole address as the request line.
        http::setProxy(http::Proxy::Named, u"127.0.0.1:%1"_s.arg(proxyServer.port()));
        r = http::get(url, {}, 1500, 0, session);
        QCOMPARE(r.status, 200);
        QCOMPARE(r.body, QByteArray("from-proxy"));
        QCOMPARE(proxyServer.requests().size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(proxyServer.requests()[0]), u"GET %1 HTTP/1.1"_s.arg(url));
        QCOMPARE(http::proxyServer(), u"127.0.0.1:%1"_s.arg(proxyServer.port()));

        // And back, in the same run: direct again, not through the proxy.
        http::setProxy(http::Proxy::None);
        QVERIFY(http::proxyServer().isEmpty());
        r = http::get(url, {}, 1500, 0, session);
        QCOMPARE(r.status, 0);
        QCOMPARE(proxyServer.requests().size(), std::size_t(1)); // not asked again

        // A host without a dot is the machine's own network's: never through the proxy ("<local>").
        http::setProxy(http::Proxy::Named, u"127.0.0.1:%1"_s.arg(proxyServer.port()));
        r = http::get(u"http://winshun-test-intranet:8080/"_s, {}, 1500, 0, session);
        QCOMPARE(r.status, 0);
        QCOMPARE(proxyServer.requests().size(), std::size_t(1));
    }

    void aProxyThatIsNotThereIsAnErrorNotADirectRequest()
    {
        // The name has no host: a direct request would fail otherwise than the proxy's refusal.
        http::setProxy(http::Proxy::Named, u"127.0.0.1:%1"_s.arg(unusedPort()));
        const http::Response r = http::get(u"http://origin.winshun-test.example:8080/"_s, {}, 4000);
        QCOMPARE(r.status, 0);
        QCOMPARE(r.error, DWORD(12029)); // ERROR_WINHTTP_CANNOT_CONNECT: to the proxy
    }

    void namedWithoutAnAddressFollowsTheSystem()
    {
        http::setProxy(http::Proxy::Named, {});
        QVERIFY(http::proxyServer().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestProxy)
#include "tst_proxy.moc"
