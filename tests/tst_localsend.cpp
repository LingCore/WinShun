// The LAN transfer plugin's parts (src/localsend-plugin, GitHub #113)
// without Win顺: names from other devices made safe, files written whole or
// not at all, the HTTP and JSON LocalSend speaks, and a receiver and a
// sender talking TLS over 127.0.0.1 (accept, decline, busy, PIN, text,
// cancels on both sides, tokens, sizes, names a hostile sender picks; a
// peer's bad text and fingerprints, refusals before its body, a pause after
// a no or a second give-up, devices that are not who they say, shutting
// down mid-transfer).

#include <winsock2.h>

#include "Background.h"
#include "Client.h"
#include "Discovery.h"
#include "HttpLite.h"
#include "Identity.h"
#include "Network.h"
#include "Protocol.h"
#include "RateLimits.h"
#include "Rows.h"
#include "Saver.h"
#include "Sender.h"
#include "Server.h"
#include "Tls.h"
#include "Util.h"

#include <QDir>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace localsend;
using namespace std::chrono_literals;
using namespace Qt::StringLiterals;
using winshun::Json;

namespace {

QString qs(const std::wstring& text)
{
    return QString::fromStdWString(text);
}

QString qs8(const std::string& text)
{
    return QString::fromStdString(text);
}

std::wstring native(const QString& path)
{
    return QDir::toNativeSeparators(path).toStdWString();
}

int code(http::ReadResult r)
{
    return static_cast<int>(r);
}

constexpr int kOk = static_cast<int>(http::ReadResult::Ok);
constexpr int kClosed = static_cast<int>(http::ReadResult::Closed);
constexpr int kTooLarge = static_cast<int>(http::ReadResult::TooLarge);
constexpr int kMalformed = static_cast<int>(http::ReadResult::Malformed);

std::string readFile(const std::wstring& path)
{
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeFile(const std::wstring& path, const std::string& bytes)
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// UTF-8 a host takes: Windows refuses anything else with MB_ERR_INVALID_CHARS.
bool isUtf8(const std::string& text)
{
    return text.empty()
        || ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

std::string randomBytes(std::size_t size)
{
    std::string out(size, '\0');
    ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return out;
}

// Zone.Identifier says the internet (ZoneId=3).
bool marked(const std::wstring& path)
{
    return readFile(path + L":Zone.Identifier").find("ZoneId=3") != std::string::npos;
}

// Every file under `folder` (hidden ones too), relative, sorted.
QStringList filesUnder(const std::wstring& folder)
{
    QStringList out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file())
            out.push_back(qs(std::filesystem::relative(it->path(), folder).wstring()));
    }
    out.sort();
    return out;
}

// Bytes in memory, a few at a time (heads arrive cut anywhere).
class MemoryStream final : public http::Stream {
public:
    explicit MemoryStream(std::string in, std::size_t chunk = 7) : m_in(std::move(in)), m_chunk(chunk) {}
    int receive(char* buffer, int size) override
    {
        const std::size_t n = std::min({static_cast<std::size_t>(size), m_chunk, m_in.size() - m_at});
        std::memcpy(buffer, m_in.data() + m_at, n);
        m_at += n;
        return static_cast<int>(n);
    }
    bool sendAll(const char* data, std::size_t size) override
    {
        out.append(data, size);
        return true;
    }
    std::string out;

private:
    std::string m_in;
    std::size_t m_chunk;
    std::size_t m_at = 0;
};

// A TLS connection of the test's own to the receiver, for what SendJob never does.
class Raw {
public:
    Raw(int port, Credentials& credentials)
    {
        m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(m_socket, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0)
            return;
        const DWORD ms = 15000;
        ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
        ::setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
        m_tls = std::make_unique<TlsChannel>(m_socket, credentials);
        std::string error;
        m_ok = m_tls->handshake(&error);
    }
    ~Raw() { close(); }
    Raw(const Raw&) = delete;
    Raw& operator=(const Raw&) = delete;

    bool ok() const { return m_ok; }
    const std::string& peerFingerprint() const { return m_tls->peerFingerprint(); }
    bool send(std::string_view bytes) { return m_ok && m_tls->sendAll(bytes.data(), bytes.size()); }
    // The status (0: none came), the body.
    int reply(std::string* body = nullptr)
    {
        if (!m_ok)
            return 0;
        http::Reader reader(*m_tls);
        http::Head head;
        if (reader.head(head, false) != http::ReadResult::Ok)
            return 0;
        std::string text;
        reader.bodyText(head, false, 64 * 1024, text);
        if (body)
            *body = text;
        return head.status;
    }
    void close()
    {
        m_tls.reset();
        if (m_socket != INVALID_SOCKET)
            ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        m_ok = false;
    }

private:
    SOCKET m_socket = INVALID_SOCKET;
    std::unique_ptr<TlsChannel> m_tls;
    bool m_ok = false;
};

std::string uploadHead(const std::string& sessionId, const std::string& fileId, const std::string& token, std::uint64_t length)
{
    return "POST /api/localsend/v2/upload?sessionId=" + http::percentEncode(sessionId) + "&fileId=" + http::percentEncode(fileId)
        + "&token=" + http::percentEncode(token) + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " + std::to_string(length)
        + "\r\nConnection: close\r\n\r\n";
}

FileMeta meta(const std::string& id, const std::string& name, std::uint64_t size, std::string sha256 = {})
{
    FileMeta f;
    f.id = id;
    f.fileName = name;
    f.size = size;
    f.fileType = "application/octet-stream";
    f.sha256 = std::move(sha256);
    return f;
}

// A prepare-upload as LocalSend 1.18 words it: UUIDs, a phone's photo
// names, dates in each file. `size`: each file's, as it is written.
// (No raw strings here: one that ends in a quote hides the test class from moc.)
std::string localSendList(std::size_t files, const std::string& fingerprint, const std::string& size = "2483112")
{
    std::string out = "{\"info\":{\"alias\":\"Pixel 8\",\"version\":\"2.1\",\"deviceModel\":\"Pixel 8\",\"deviceType\":\"mobile\",\"fingerprint\":\""
        + fingerprint + "\",\"port\":53317,\"protocol\":\"https\",\"download\":false},\"files\":{";
    for (std::size_t i = 0; i < files; ++i) {
        char id[64];
        std::snprintf(id, sizeof id, "%08zx-1c2d-4e5f-8a9b-0c1d2e3f4a5b", i);
        out += std::string(i ? "," : "") + "\"" + id + "\":{\"id\":\"" + id + "\",\"fileName\":\"IMG_20261028_" + std::to_string(100000 + i)
            + ".jpg\",\"size\":" + size + ",\"fileType\":\"image/jpeg\",\"sha256\":null,\"preview\":null,"
              "\"metadata\":{\"modified\":\"2026-10-28T09:41:07.123Z\",\"accessed\":\"2026-10-28T09:41:07.123Z\"}}";
    }
    return out + "}}";
}

struct Prepared {
    int status = 0;
    std::string body;
    std::string sessionId;
    std::map<std::string, std::string> tokens;
};

// A port held the way LocalSend on this PC holds 53317: every address, and
// nobody else on it. INVALID_SOCKET when it cannot be had.
SOCKET holdPort(int port)
{
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return s;
    const BOOL on = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 4) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

ReceiveOutcome receiveEnded(ReceiveOutcome::Kind kind, std::size_t saved, const std::string& from)
{
    ReceiveOutcome o;
    o.kind = kind;
    o.sender.alias = from;
    o.folder = L"C:\\收件";
    for (std::size_t i = 0; i < saved; ++i)
        o.saved.push_back(L"C:\\收件\\" + std::to_wstring(i) + L".jpg");
    return o;
}

} // namespace

class TestLocalSend : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<Identity> m_receiverIdentity;
    std::unique_ptr<Identity> m_senderIdentity;
    std::unique_ptr<Credentials> m_serverCredentials;
    std::unique_ptr<Credentials> m_clientCredentials;
    std::unique_ptr<Client> m_client;
    mutable std::mutex m_logMutex; // lines come from the server's threads
    std::vector<std::string> m_logs;

    // One receiver per test (its PIN attempts and text gaps start afresh).
    std::unique_ptr<Receiver> m_receiver;
    std::wstring m_folder;
    int m_port = 0;
    int m_round = 0;
    std::mutex m_mutex;
    std::vector<IncomingRequest> m_asked;
    std::vector<TextMessage> m_texts;
    std::vector<ReceiveOutcome> m_outcomes;
    std::vector<std::pair<DeviceInfo, bool>> m_registered;
    std::optional<bool> m_answer; // given at once when set

    DeviceInfo senderInfo() const
    {
        DeviceInfo d;
        d.alias = "发送端";
        d.deviceModel = "Windows";
        d.fingerprint = m_senderIdentity->fingerprint();
        return d;
    }
    Target target() const
    {
        Target t;
        t.ip = "127.0.0.1";
        t.port = m_port;
        t.fingerprint = m_receiverIdentity->fingerprint();
        return t;
    }

    // On `port` or the next free one after it (kPort: as the plugin does).
    bool startReceiver(const std::string& pin = {}, int port = kPort)
    {
        m_folder = native(m_dir.path() + u"/in-"_s + QString::number(++m_round));
        ensureFolder(m_folder);
        Receiver::Events events;
        events.asked = [this](const IncomingRequest& r) {
            std::optional<bool> answer;
            {
                const std::lock_guard lock(m_mutex);
                m_asked.push_back(r);
                answer = m_answer;
            }
            if (answer)
                m_receiver->decide(r.id, *answer);
        };
        events.text = [this](const TextMessage& m) {
            const std::lock_guard lock(m_mutex);
            m_texts.push_back(m);
        };
        events.finished = [this](const ReceiveOutcome& o) {
            const std::lock_guard lock(m_mutex);
            m_outcomes.push_back(o);
        };
        events.registered = [this](const DeviceInfo& d, const std::string&, bool verified) {
            const std::lock_guard lock(m_mutex);
            m_registered.emplace_back(d, verified);
        };
        m_receiver = std::make_unique<Receiver>(*m_serverCredentials, std::move(events));
        ReceiverConfig config;
        config.self.alias = "接收端";
        config.self.deviceModel = "Windows";
        config.self.fingerprint = m_receiverIdentity->fingerprint();
        config.pin = pin;
        config.folder = m_folder;
        config.spareBytes = 0; // whatever this machine's disk has left
        m_receiver->setConfig(config);
        m_port = m_receiver->listen({{"127.0.0.1", INADDR_LOOPBACK, 8}}, port);
        return m_port >= port && m_port <= kLastPort;
    }

    template <typename F>
    bool waitFor(F&& done, int ms = 15000)
    {
        return QTest::qWaitFor([&] {
            const std::lock_guard lock(m_mutex);
            return done();
        }, ms);
    }
    std::optional<ReceiveOutcome> outcome(std::size_t index = 0)
    {
        if (!waitFor([&] { return m_outcomes.size() > index; }))
            return std::nullopt;
        const std::lock_guard lock(m_mutex);
        return m_outcomes[index];
    }

    // A job's outcome, waited for.
    struct Job {
        std::mutex mutex;
        std::optional<SendOutcome> outcome;
        std::unique_ptr<SendJob> job;
    };
    std::unique_ptr<Job> send(Outgoing what, std::optional<Target> to = std::nullopt)
    {
        auto j = std::make_unique<Job>();
        Job* raw = j.get();
        j->job = std::make_unique<SendJob>(*m_client, to.value_or(target()), "接收端", senderInfo(), std::move(what),
            [raw](const SendOutcome& o) {
                const std::lock_guard lock(raw->mutex);
                raw->outcome = o;
            });
        return j;
    }
    static std::optional<SendOutcome> ended(Job& j, int ms = 20000)
    {
        (void)QTest::qWaitFor([&] {
            const std::lock_guard lock(j.mutex);
            return j.outcome.has_value();
        }, ms);
        const std::lock_guard lock(j.mutex);
        return j.outcome;
    }
    static bool viewIs(Job& j, SendView::State state, bool wrongPin = false, int ms = 15000)
    {
        return QTest::qWaitFor([&] {
            const std::optional<SendView> v = j.job->view();
            return v && v->state == state && v->wrongPin == wrongPin;
        }, ms);
    }

    // A prepare-upload as the test words it (m_answer decides it).
    Prepared prepare(const std::vector<FileMeta>& files, const std::string& query = {})
    {
        PrepareUpload p;
        p.info = senderInfo();
        p.files = files;
        const Reply r = m_client->request(target(), "POST", "/api/localsend/v2/prepare-upload" + query, toJson(p).dump(), 20s);
        Prepared out;
        out.status = r.status;
        out.body = r.body;
        if (r.status == 200) {
            const std::optional<Json> json = Json::parse(r.body);
            if (json) {
                out.sessionId = (*json)["sessionId"].toString();
                for (const auto& [id, token] : (*json)["files"].toObject())
                    out.tokens[id] = token.toString();
            }
        }
        return out;
    }
    int upload(const Prepared& p, const std::string& fileId, const std::string& bytes, const std::string* token = nullptr,
        std::string* body = nullptr)
    {
        Raw c(m_port, *m_clientCredentials);
        const auto t = p.tokens.find(fileId);
        const std::string use = token ? *token : (t == p.tokens.end() ? std::string() : t->second);
        if (!c.send(uploadHead(p.sessionId, fileId, use, bytes.size()) + bytes))
            return 0;
        return c.reply(body);
    }
    bool logsMention(const std::string& text) const
    {
        const std::lock_guard lock(m_logMutex);
        return std::any_of(m_logs.begin(), m_logs.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }

private slots:
    void initTestCase()
    {
        WSADATA wsa {};
        QCOMPARE(::WSAStartup(MAKEWORD(2, 2), &wsa), 0);
        QVERIFY(m_dir.isValid());
        setLogSink([this](const std::string& line) {
            const std::lock_guard lock(m_logMutex);
            m_logs.push_back(line);
        });
        std::string error;
        m_receiverIdentity = Identity::loadOrCreate(native(m_dir.path() + u"/id-receiver"_s), &error);
        QVERIFY2(m_receiverIdentity, error.c_str());
        m_senderIdentity = Identity::loadOrCreate(native(m_dir.path() + u"/id-sender"_s), &error);
        QVERIFY2(m_senderIdentity, error.c_str());
        QCOMPARE(m_receiverIdentity->fingerprint().size(), std::size_t(64));
        QVERIFY(m_receiverIdentity->fingerprint() != m_senderIdentity->fingerprint());
        // Loaded again: the same certificate (peers keep knowing this PC).
        const std::string fingerprint = m_receiverIdentity->fingerprint();
        m_receiverIdentity.reset();
        m_receiverIdentity = Identity::loadOrCreate(native(m_dir.path() + u"/id-receiver"_s), &error);
        QVERIFY2(m_receiverIdentity, error.c_str());
        QVERIFY(!m_receiverIdentity->created());
        QCOMPARE(m_receiverIdentity->fingerprint(), fingerprint);
        m_serverCredentials = Credentials::acquire(*m_receiverIdentity, Credentials::Role::Server, &error);
        QVERIFY2(m_serverCredentials, error.c_str());
        m_clientCredentials = Credentials::acquire(*m_senderIdentity, Credentials::Role::Client, &error);
        QVERIFY2(m_clientCredentials, error.c_str());
        m_client = std::make_unique<Client>(*m_clientCredentials);
    }

    void cleanup()
    {
        m_receiver.reset();
        const std::lock_guard lock(m_mutex);
        m_asked.clear();
        m_texts.clear();
        m_outcomes.clear();
        m_registered.clear();
        m_answer.reset();
    }

    void cleanupTestCase()
    {
        m_client.reset();
        m_serverCredentials.reset();
        m_clientCredentials.reset();
        m_receiverIdentity.reset();
        m_senderIdentity.reset();
        // The keys are the user's (named CNG keys): none may stay behind.
        QVERIFY(Identity::forget(native(m_dir.path() + u"/id-receiver"_s)));
        QVERIFY(Identity::forget(native(m_dir.path() + u"/id-sender"_s)));
        setLogSink({});
        ::WSACleanup();
    }

    // ---- names and files ----------------------------------------------

    void safeNames_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QString>("expected"); // empty: refused

        QTest::newRow("plain") << QByteArray("a.txt") << u"a.txt"_s;
        QTest::newRow("folders") << QByteArray("相册/IMG_1.jpg") << u"相册\\IMG_1.jpg"_s;
        QTest::newRow("backslashes") << QByteArray("相册\\深\\b.bin") << u"相册\\深\\b.bin"_s;
        QTest::newRow("dots and empty parts") << QByteArray("./a/./b//c.txt") << u"a\\b\\c.txt"_s;
        QTest::newRow("dot space dot dropped") << QByteArray(". ./x.txt") << u"x.txt"_s;
        QTest::newRow("up") << QByteArray("../evil.txt") << QString();
        QTest::newRow("up inside") << QByteArray("a/../../evil.txt") << QString();
        QTest::newRow("up backslash") << QByteArray("a\\..\\evil.txt") << QString();
        QTest::newRow("up with a space") << QByteArray("a/ ../evil.txt") << QString();
        QTest::newRow("up with dots") << QByteArray("a/..../evil.txt") << QString();
        QTest::newRow("drive") << QByteArray("C:/Windows/evil.dll") << QString();
        QTest::newRow("drive relative") << QByteArray("C:evil.txt") << QString();
        QTest::newRow("share") << QByteArray("\\\\server\\share\\evil.txt") << QString();
        QTest::newRow("share slashes") << QByteArray("//server/share/evil.txt") << QString();
        QTest::newRow("root") << QByteArray("/etc/passwd") << QString();
        QTest::newRow("device") << QByteArray("CON") << u"_CON"_s;
        QTest::newRow("device with extension") << QByteArray("con.txt") << u"_con.txt"_s;
        QTest::newRow("device two extensions") << QByteArray("COM1.tar.gz") << u"_COM1.tar.gz"_s;
        QTest::newRow("device in a folder") << QByteArray("a/lpt9/x.txt") << u"a\\_lpt9\\x.txt"_s;
        QTest::newRow("device with a space") << QByteArray("nul .txt") << u"_nul .txt"_s;
        QTest::newRow("console is no device") << QByteArray("console.txt") << u"console.txt"_s;
        QTest::newRow("characters") << QByteArray("a<b>c\"d|e?f*g.txt") << u"a_b_c_d_e_f_g.txt"_s;
        QTest::newRow("stream") << QByteArray("x.txt:Zone.Identifier") << u"x.txt_Zone.Identifier"_s;
        QTest::newRow("controls") << QByteArray("x\x01y\x7f" ".txt") << u"x_y_.txt"_s;
        QTest::newRow("turned around") << QByteArray("报告\xE2\x80\xAE" "fdp.exe") << u"报告_fdp.exe"_s;
        QTest::newRow("isolate") << QByteArray("a\xE2\x81\xA7" "b.txt") << u"a_b.txt"_s;
        const QByteArray family("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7.jpg");
        QTest::newRow("joiners kept") << family << QString::fromUtf8(family);
        QTest::newRow("trailing dots and spaces") << QByteArray("name. . ") << u"name"_s;
        QTest::newRow("leading spaces") << QByteArray("  lead.txt") << u"lead.txt"_s;
        QTest::newRow("nothing left") << QByteArray(" ") << u"文件"_s;
        QTest::newRow("long") << QByteArray(300, 'a') + ".jpg" << QString(196, u'a') + u".jpg"_s;
        QTest::newRow("long, a pair not cut") << QByteArray(195, 'a') + "\xF0\x9F\x98\x80" + QByteArray(10, 'b') + ".jpg"
                                             << QString(195, u'a') + u".jpg"_s;
        QByteArray deep;
        QString deepExpected;
        for (int i = 0; i < kMaxDepth - 1; ++i) {
            deep += "d/";
            deepExpected += u"d\\"_s;
        }
        QTest::newRow("deep") << deep + "f.txt" << deepExpected + u"f.txt"_s;
        QTest::newRow("too deep") << "d/" + deep + "f.txt" << QString();
    }

    void safeNames()
    {
        QFETCH(QByteArray, name);
        QFETCH(QString, expected);
        std::string why;
        const std::optional<std::wstring> safe = safeRelativePath(std::string_view(name.constData(), name.size()), &why);
        if (expected.isEmpty()) {
            QVERIFY2(!safe, qPrintable(safe ? qs(*safe) : QString()));
            QVERIFY(!why.empty());
        } else {
            QVERIFY2(safe, why.c_str());
            QCOMPARE(qs(*safe), expected);
        }
    }

    void placement()
    {
        QTemporaryDir dir;
        const std::wstring folder = native(dir.path());
        QVERIFY(writeFile(folder + L"\\a.txt", "x"));
        QVERIFY(writeFile(folder + L"\\相册\\old.jpg", "x"));
        Placement p(folder);
        QCOMPARE(qs(p.place(L"a.txt")), qs(folder + L"\\a (1).txt"));
        QCOMPARE(qs(p.place(L"a.txt")), qs(folder + L"\\a (2).txt")); // two of one transfer never meet
        QCOMPARE(qs(p.place(L"A.TXT")), qs(folder + L"\\A (3).TXT")); // as NTFS compares
        QCOMPARE(qs(p.place(L"相册\\1.jpg")), qs(folder + L"\\相册 (1)\\1.jpg"));
        QCOMPARE(qs(p.place(L"相册\\深\\2.jpg")), qs(folder + L"\\相册 (1)\\深\\2.jpg")); // renamed once for the transfer
        QCOMPARE(qs(p.place(L"新\\x.txt")), qs(folder + L"\\新\\x.txt"));
        QCOMPARE(qs(p.place(L"noext")), qs(folder + L"\\noext"));
        QCOMPARE(qs(p.place(L"noext")), qs(folder + L"\\noext (1)"));
        // Too long a path: the file's own name, straight in the folder.
        std::wstring deep;
        for (int i = 0; i < 12; ++i)
            deep += std::wstring(20, L'd') + L"\\";
        const std::wstring placed = p.place(deep + L"photo.jpg");
        QCOMPARE(qs(placed), qs(folder + L"\\photo.jpg"));
        QVERIFY(placed.size() <= kMaxPathChars);
    }

    void incomingFile()
    {
        QTemporaryDir dir;
        const std::wstring folder = native(dir.path());
        std::string why;
        auto f = IncomingFile::create(folder + L"\\one.txt", 5, sha256Hex("hello", 5), &why);
        QVERIFY2(f, why.c_str());
        const DWORD attributes = ::GetFileAttributesW((folder + L"\\one.txt.wspart").c_str());
        QVERIFY(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN));
        QVERIFY(f->write("hel", 3));
        QVERIFY(f->write("lo", 2));
        QVERIFY(!f->write("!", 1)); // more than declared
        const std::optional<std::wstring> saved = f->finish("https://127.0.0.1:53317/", &why);
        QVERIFY2(saved, why.c_str());
        QCOMPARE(qs(*saved), qs(folder + L"\\one.txt"));
        QCOMPARE(readFile(*saved), std::string("hello"));
        QVERIFY(marked(*saved));
        QVERIFY(readFile(*saved + L":Zone.Identifier").find("HostUrl=https://127.0.0.1:53317/") != std::string::npos);
        QVERIFY(!(::GetFileAttributesW(saved->c_str()) & FILE_ATTRIBUTE_HIDDEN));

        auto lowerSum = IncomingFile::create(folder + L"\\lower.txt", 5, lower(sha256Hex("hello", 5)), &why);
        QVERIFY(lowerSum && lowerSum->write("hello", 5));
        QVERIFY(lowerSum->finish("https://x/", &why)); // a checksum in lower case agrees too

        auto shortFile = IncomingFile::create(folder + L"\\short.txt", 10, {}, &why);
        QVERIFY(shortFile && shortFile->write("abc", 3));
        QVERIFY(!shortFile->finish("https://x/", &why));
        QCOMPARE(why, std::string("it came short"));

        auto wrongSum = IncomingFile::create(folder + L"\\sum.txt", 5, std::string(64, '0'), &why);
        QVERIFY(wrongSum && wrongSum->write("hello", 5));
        QVERIFY(!wrongSum->finish("https://x/", &why));
        QCOMPARE(why, std::string("its checksum does not agree"));

        // A file of that name came meanwhile: kept, the new one beside it.
        auto race = IncomingFile::create(folder + L"\\race.txt", 1, {}, &why);
        QVERIFY(race);
        QVERIFY(writeFile(folder + L"\\race.txt", "other"));
        QVERIFY(race->write("x", 1));
        const std::optional<std::wstring> beside = race->finish("https://x/", &why);
        QVERIFY2(beside, why.c_str());
        QCOMPARE(qs(*beside), qs(folder + L"\\race (1).txt"));
        QCOMPARE(readFile(folder + L"\\race.txt"), std::string("other"));

        {
            auto dropped = IncomingFile::create(folder + L"\\gone.txt", 3, {}, &why);
            QVERIFY(dropped && dropped->write("ab", 2));
        } // not finished: deleted
        QCOMPARE(filesUnder(folder), (QStringList {u"lower.txt"_s, u"one.txt"_s, u"race (1).txt"_s, u"race.txt"_s}));
    }

    // ---- HTTP and JSON ------------------------------------------------

    void httpHeads()
    {
        MemoryStream in("\r\nPOST /api/localsend/v2/upload?sessionId=a%20b&fileId=f1 HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                        "X-Mixed-Case:  V \r\n\r\nhello"
                        "GET /api/localsend/v2/info HTTP/1.1\r\nConnection: close\r\n\r\n");
        http::Reader reader(in);
        http::Head head;
        QCOMPARE(code(reader.head(head, true)), kOk);
        QCOMPARE(head.method, std::string("POST"));
        QCOMPARE(head.path, std::string("/api/localsend/v2/upload"));
        QCOMPARE(head.query, std::string("sessionId=a%20b&fileId=f1"));
        QCOMPARE(head.header("x-mixed-case"), std::string("V"));
        QCOMPARE(head.header("content-length"), std::string("5"));
        std::string body;
        QCOMPARE(code(reader.bodyText(head, true, 100, body)), kOk);
        QCOMPARE(body, std::string("hello"));
        QCOMPARE(code(reader.head(head, true)), kOk); // the next request on the connection
        QCOMPARE(head.method, std::string("GET"));
        QVERIFY(head.query.empty());
        QVERIFY(head.close());
        QCOMPARE(code(reader.bodyText(head, true, 100, body)), kOk);
        QVERIFY(body.empty()); // no length: no body
        QCOMPARE(code(reader.head(head, true)), kClosed);

        const auto read = [](const std::string& text, bool request) {
            MemoryStream s(text);
            http::Reader r(s);
            http::Head h;
            return code(r.head(h, request));
        };
        QCOMPARE(read("GARBAGE\r\n\r\n", true), kMalformed);
        QCOMPARE(read("GET / HTTP/1.1\r\nNoColon\r\n\r\n", true), kMalformed);
        QCOMPARE(read("GET / HTTP/1.1\r\n: empty name\r\n\r\n", true), kMalformed);
        QCOMPARE(read("GET / HTTP/1.1\r\nHost: x", true), kMalformed); // cut off
        QCOMPARE(read("GET / HTTP/1.1\r\nX: " + std::string(20000, 'a') + "\r\n\r\n", true), kTooLarge);
        std::string many = "GET / HTTP/1.1\r\n";
        for (int i = 0; i < 101; ++i)
            many += "X" + std::to_string(i) + ": y\r\n";
        QCOMPARE(read(many + "\r\n", true), kTooLarge);
        QCOMPARE(read("HTTP/1.1 2x0 OK\r\n\r\n", false), kMalformed);
        QCOMPARE(read("\r\n\r\n\r\nGET / HTTP/1.1\r\n\r\n", true), kMalformed); // blank lines: two at most
    }

    void httpBodies()
    {
        const auto body = [](const std::string& text, bool request, std::uint64_t max, std::string& out, std::uint64_t* bytes = nullptr) {
            MemoryStream s(text);
            http::Reader r(s);
            http::Head h;
            if (r.head(h, request) != http::ReadResult::Ok)
                return -1;
            out.clear();
            return code(r.body(h, request, max, [&](const char* data, std::size_t size) {
                out.append(data, size);
                return true;
            }, bytes));
        };
        std::string out;
        const std::string chunked = "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\n";
        std::uint64_t bytes = 0;
        QCOMPARE(body(chunked, true, 100, out, &bytes), kOk);
        QCOMPARE(out, std::string("hello world"));
        QCOMPARE(bytes, std::uint64_t(11));
        QCOMPARE(body(chunked, true, 8, out), kTooLarge);
        QCOMPARE(out, std::string("hello wo")); // what fitted went to the sink
        QCOMPARE(body("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", true, 100, out), kMalformed);
        QCOMPARE(body("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n", true, 100, out), kMalformed);
        QCOMPARE(body("POST /x HTTP/1.1\r\nContent-Length: 100\r\n\r\nabc", true, 10, out), kTooLarge);
        QVERIFY(out.empty());
        for (const char* length : {"-1", "1e3", " 12x", "99999999999999999999"})
            QCOMPARE(body(std::string("POST /x HTTP/1.1\r\nContent-Length: ") + length + "\r\n\r\nabc", true, 100, out), kMalformed);
        QCOMPARE(body("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nhello", true, 100, out), kMalformed); // the stream ended first
        QCOMPARE(body("HTTP/1.1 200 OK\r\n\r\nall of it", false, 100, out), kOk); // a response to the connection's end
        QCOMPARE(out, std::string("all of it"));
        QCOMPARE(body("HTTP/1.1 204 No Content\r\nContent-Length: 7\r\n\r\nnothing", false, 100, out), kOk);
        QVERIFY(out.empty()); // a 204 has no body, whatever its head says

        MemoryStream s("POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
        http::Reader r(s);
        http::Head h;
        QCOMPARE(code(r.head(h, true)), kOk);
        QCOMPARE(code(r.body(h, true, 100, [](const char*, std::size_t) { return false; })), kMalformed); // the sink said stop
    }

    void httpQueryAndReplies()
    {
        const auto q = http::parseQuery("sessionId=a%20b&fileId=f%2F1&token=x+y&flag&=v&pin=%E4%BD%A0&bad=%zz%4");
        QCOMPARE(q.at("sessionId"), std::string("a b"));
        QCOMPARE(q.at("fileId"), std::string("f/1"));
        QCOMPARE(q.at("token"), std::string("x y"));
        QCOMPARE(q.at("flag"), std::string());
        QCOMPARE(q.at(""), std::string("v"));
        QCOMPARE(qs8(q.at("pin")), u"你"_s);
        QCOMPARE(q.at("bad"), std::string("%zz%4"));
        const std::string odd = "a b/你~&=?";
        QCOMPARE(http::percentEncode(odd), std::string("a%20b%2F%E4%BD%A0~%26%3D%3F"));
        QCOMPARE(http::parseQuery("v=" + http::percentEncode(odd)).at("v"), odd);

        MemoryStream s("");
        QVERIFY(http::respond(s, 200, "{}"));
        QCOMPARE(s.out, std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\n{}"));
        s.out.clear();
        QVERIFY(http::respond(s, 204));
        QCOMPARE(s.out, std::string("HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\n\r\n"));
        s.out.clear();
        QVERIFY(http::respond(s, 403, http::messageJson("No \"answer\""), "application/json", true));
        QVERIFY(s.out.find("Connection: close\r\n") != std::string::npos);
        QVERIFY(s.out.find("{\"message\":\"No \\\"answer\\\"\"}") != std::string::npos);
    }

    void prepareUploadJson()
    {
        const auto parse = [](const std::string& text, std::string* why = nullptr) -> std::optional<PrepareUpload> {
            const std::optional<Json> json = Json::parse(text);
            return json ? parsePrepareUpload(*json, why) : std::nullopt;
        };
        const std::string info = R"("info":{"alias":"手机","version":"2.1","deviceModel":"Pixel","deviceType":"mobile","fingerprint":"AB12","port":53317,"protocol":"https","download":false})";
        std::string why;
        auto ok = parse("{" + info + R"(,"files":{"f1":{"id":"f1","fileName":"相册/a.jpg","size":10,"fileType":"image/jpeg","sha256":"00"},"k2":{"fileName":"b.txt","size":20,"fileType":"text/plain"}}})", &why);
        QVERIFY2(ok, why.c_str());
        QCOMPARE(qs8(ok->info.alias), u"手机"_s);
        QCOMPARE(ok->info.deviceType, std::string("mobile"));
        QCOMPARE(ok->files.size(), std::size_t(2));
        QCOMPARE(ok->files[1].id, std::string("k2")); // the key when there is no id
        QCOMPARE(ok->totalBytes(), std::uint64_t(30));
        QVERIFY(!isTextMessage(*ok));

        const auto refused = [&](const std::string& files) {
            std::string reason;
            return !parse("{" + info + ",\"files\":" + files + "}", &reason) && !reason.empty();
        };
        QVERIFY(refused("{}"));
        QVERIFY(refused("[]"));
        QVERIFY(refused(R"({"a":{"id":"x","fileName":"1","size":1},"b":{"id":"x","fileName":"2","size":1}})")); // ids repeated
        QVERIFY(refused(R"({"a":{"fileName":"","size":1}})"));
        QVERIFY(refused("{\"a\":{\"fileName\":\"" + std::string(kMaxNameBytes + 1, 'n') + "\",\"size\":1}}"));
        QVERIFY(refused(R"({"a":{"fileName":"x","size":-1}})"));
        QVERIFY(refused(R"({"a":{"fileName":"x","size":1.5}})"));
        QVERIFY(refused(R"({"a":{"fileName":"x","size":"10"}})"));
        QVERIFY(refused(R"({"a":{"fileName":"x","size":1e16}})"));
        QVERIFY(refused(R"({"a":"not an object"})"));
        QVERIFY(refused("{\"a\":{\"id\":\"" + std::string(129, 'i') + "\",\"fileName\":\"x\",\"size\":1}}"));
        std::string many = "{";
        for (std::size_t i = 0; i <= kMaxFiles; ++i)
            many += (i ? ",\"" : "\"") + std::to_string(i) + "\":{\"fileName\":\"x\",\"size\":1}";
        QVERIFY(refused(many + "}"));
        QVERIFY(!parse(R"({"files":{"a":{"fileName":"x","size":1}}})")); // no info
        QVERIFY(!parse(R"({"info":{"alias":"x"},"files":{"a":{"fileName":"x","size":1}}})")); // no fingerprint

        // A text message: one text file with its words, cut at 64 KB on a character.
        std::string words;
        for (int i = 0; i < 30000; ++i)
            words += "你";
        // (Not a raw string: one that ends in a quote, "…":")", hides this class from moc.)
        auto text = parse("{" + info + ",\"files\":{\"t\":{\"fileName\":\"t.txt\",\"size\":90000,\"fileType\":\"text/plain\",\"preview\":\"" + words
            + "\"}}}");
        QVERIFY(text);
        QVERIFY(isTextMessage(*text));
        QCOMPARE(text->files[0].preview.size(), std::size_t(65535));
        auto noWords = parse("{" + info + R"(,"files":{"t":{"fileName":"t.txt","size":3,"fileType":"text/plain"}}})");
        QVERIFY(noWords && !isTextMessage(*noWords));

        // Round trip: what the sender says is what the receiver reads.
        auto again = parse(toJson(*ok).dump());
        QVERIFY(again);
        QCOMPARE(again->files.size(), std::size_t(2));
        QCOMPARE(qs8(again->files[0].fileName), u"相册/a.jpg"_s);
        QCOMPARE(again->info.port, kPort);

        // The sum of 10,000 sizes cannot wrap around.
        PrepareUpload huge;
        huge.files = {meta("a", "a", UINT64_MAX / 2 + 1), meta("b", "b", UINT64_MAX / 2 + 1)};
        QCOMPARE(huge.totalBytes(), UINT64_MAX);
    }

    void deviceJson()
    {
        const auto device = [](const std::string& text) {
            const std::optional<Json> json = Json::parse(text);
            return json ? deviceFromJson(*json) : std::nullopt;
        };
        auto d = device(R"({"alias":"a\nb\u202Ec","fingerprint":"F","deviceType":"fridge","port":70000,"protocol":"ftp"})");
        QVERIFY(d);
        QCOMPARE(qs8(d->alias), u"a b_c"_s); // one line, nothing turning it around
        QCOMPARE(d->deviceType, std::string("desktop"));
        QCOMPARE(d->port, kPort);
        QVERIFY(d->https());
        d = device(R"({"alias":"x","fingerprint":"F","port":8080.5,"protocol":"http"})");
        QVERIFY(d);
        QCOMPARE(d->port, kPort);
        QVERIFY(!d->https());
        std::string longAlias;
        for (int i = 0; i < 100; ++i)
            longAlias += "长";
        d = device("{\"alias\":\"" + longAlias + "\",\"fingerprint\":\"F\",\"port\":53318}");
        QVERIFY(d);
        QCOMPARE(d->alias.size(), std::size_t(126)); // 128 bytes at most, not cutting a character
        QCOMPARE(d->port, 53318);
        QVERIFY(!device(R"({"alias":"","fingerprint":"F"})"));
        QVERIFY(!device(R"(["not", "an object"])"));
    }

    void peerText()
    {
        // Cut on a character, a four-byte one too; bad bytes made U+FFFD.
        const std::string smile = "\xF0\x9F\x98\x80";
        QCOMPARE(cutUtf8("a" + smile, 4), std::string("a"));
        QCOMPARE(cutUtf8("a" + smile, 5), "a" + smile);
        QCOMPARE(cutUtf8("你好", 5), std::string("你"));
        QCOMPARE(cutUtf8("abc", 0), std::string());
        QCOMPARE(validUtf8("x\xFFy"), std::string("x\xEF\xBF\xBDy"));
        QVERIFY(isUtf8(validUtf8("\xED\xA0\x80" "a\xC3"))); // a surrogate written as UTF-8, a sequence cut short
        QVERIFY(!isUtf8("\xED\xA0\x80"));

        const auto device = [](const std::string& text) {
            const std::optional<Json> json = Json::parse(text);
            return json ? deviceFromJson(*json) : std::nullopt;
        };
        // A fingerprint is what LocalSend makes (hex, or a random string), never cut: else no device.
        for (const std::string& fp : {std::string("AB 12"), std::string("AB\\\"12"), std::string("指纹"), std::string("a/b"),
                 std::string("x\\n"), std::string(kMaxFingerprintBytes + 1, 'A')})
            QVERIFY2(!device("{\"alias\":\"x\",\"fingerprint\":\"" + fp + "\"}"), fp.c_str());
        for (const std::string& fp : {std::string(kMaxFingerprintBytes, 'A'), std::string("8f3e-4a_B")})
            QVERIFY2(device("{\"alias\":\"x\",\"fingerprint\":\"" + fp + "\"}"), fp.c_str());
        // A name with bad bytes: UTF-8 all the same (a host drops a line that is not).
        std::optional<DeviceInfo> d = device("{\"alias\":\"A\xFF\xFE\",\"fingerprint\":\"F\",\"deviceModel\":\"\xC3\",\"version\":\"2\xFF\"}");
        QVERIFY(d);
        QCOMPARE(d->alias, std::string("A\xEF\xBF\xBD\xEF\xBF\xBD"));
        QVERIFY(isUtf8(d->deviceModel) && isUtf8(d->version));

        // A text message's words: bad bytes in them, a four-byte character at the cut.
        std::string words = "\xFF";
        while (words.size() < kMaxTextBytes + 100)
            words += smile;
        const std::string info = R"("info":{"alias":"x","fingerprint":"F"})";
        const std::optional<Json> json = Json::parse("{" + info + ",\"files\":{\"t\":{\"fileName\":\"t.txt\",\"size\":1,\"fileType\":\"text/plain\",\"preview\":\""
            + words + "\",\"sha256\":\"\xC3\"}}}");
        QVERIFY(json);
        std::string why;
        const std::optional<PrepareUpload> text = parsePrepareUpload(*json, &why);
        QVERIFY2(text, why.c_str());
        QVERIFY(text->files[0].preview.size() <= kMaxTextBytes);
        QVERIFY(text->files[0].preview.size() > kMaxTextBytes - 4);
        QVERIFY(isUtf8(text->files[0].preview));
        QVERIFY(isUtf8(text->files[0].sha256));
    }

    void fewValuesCounted()
    {
        // One, and one more for each ',', '[' and '{' outside strings.
        QVERIFY(fewValues("[0,0,0]", 4));
        QVERIFY(!fewValues("[0,0,0]", 3));
        QVERIFY(fewValues("[\"a,b,[{c\"]", 2));
        QVERIFY(fewValues("[\"a\\\",b\"]", 2)); // an escaped quote does not end the string
        QVERIFY(fewValues("", 1));
        std::string many = "[";
        for (std::size_t i = 0; i < kMaxValues; ++i)
            many += "0,";
        many += "0]";
        QVERIFY(!parsePeerJson(many, kMaxValues));
        QVERIFY(parsePeerJson("{\"a\":[1,2]}", kMaxDeviceValues));
        QVERIFY(!parsePeerJson("{not json", kMaxDeviceValues));
    }

    void partialsSwept()
    {
        QTemporaryDir dir;
        const std::wstring folder = native(dir.path());
        const auto hide = [](const std::wstring& path) { return ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0; };
        // Left by a run that was ended: hidden and "*.wspart". Not hidden: somebody's own file.
        QVERIFY(writeFile(folder + L"\\left.bin.wspart", "x"));
        QVERIFY(hide(folder + L"\\left.bin.wspart"));
        QVERIFY(writeFile(folder + L"\\LEFT2.WSPART", "x"));
        QVERIFY(hide(folder + L"\\LEFT2.WSPART"));
        QVERIFY(writeFile(folder + L"\\mine.wspart", "x"));
        QVERIFY(writeFile(folder + L"\\other.txt", "x"));
        QVERIFY(hide(folder + L"\\other.txt"));
        QVERIFY(writeFile(folder + L"\\sub\\deeper.wspart", "x")); // only straight in the folder
        QVERIFY(hide(folder + L"\\sub\\deeper.wspart"));
        // One being written (by another run): it is not shared for deleting, and stays.
        std::string why;
        auto writing = IncomingFile::create(folder + L"\\sub\\busy.bin", 3, {}, &why, folder);
        QVERIFY2(writing, why.c_str());
        QVERIFY(writing->write("ab", 2));
        QCOMPARE(sweepPartials(folder), std::size_t(2));
        QCOMPARE(filesUnder(folder), (QStringList {u"busy.bin.wspart"_s, u"mine.wspart"_s, u"other.txt"_s, u"sub\\deeper.wspart"_s}));
        writing.reset(); // unfinished: deleted
        QCOMPARE(sweepPartials(L""), std::size_t(0));

        // The empty folders a transfer made go; one with a file stays, so does one that is a link.
        QVERIFY(writeFile(folder + L"\\t\\full\\f.txt", "x"));
        QVERIFY(ensureFolder(folder + L"\\t\\empty\\deeper"));
        removeEmptyFolders(folder + L"\\t");
        QVERIFY(std::filesystem::exists(std::filesystem::path(folder + L"\\t\\full\\f.txt")));
        QVERIFY(!std::filesystem::exists(std::filesystem::path(folder + L"\\t\\empty")));
        removeEmptyFolders(folder + L"\\nothing here"); // gone already: nothing happens
    }

    void spoofedDevices()
    {
        // Told (传 asked again at once) only when what it lists changed.
        std::atomic<int> changes {0};
        Discovery discovery(*m_client, [&] { ++changes; });
        Interface link; // no such adapter: nothing is heard or sent on it
        link.index = 999999;
        link.name = "test";
        link.ip = "10.255.255.1";
        link.address = parseIpv4(link.ip);
        link.prefix = 24;
        discovery.setNetworks({{link, false}});
        const auto info = [](const std::string& fp, const std::string& alias, const std::string& protocol) {
            DeviceInfo d;
            d.alias = alias;
            d.fingerprint = fp;
            d.protocol = protocol;
            return d;
        };
        const std::string fp(64, 'A');
        discovery.heard(info(fp, "真的", "https"), "10.255.255.7", true);
        std::optional<Device> d = discovery.device(fp);
        QVERIFY(d && d->ip == "10.255.255.7" && d->verified);
        QCOMPARE(changes.load(), 1); // found
        // Anyone can say its fingerprint: words nobody checked do not move it, rename it or make it HTTP.
        discovery.heard(info(fp, "假的", "https"), "10.255.255.8", false);
        discovery.heard(info(fp, "假的", "http"), "10.255.255.7", false);
        discovery.heard(info(fp, "假的", "https"), "10.255.255.7", false); // where it is: only "still there"
        d = discovery.device(fp);
        QVERIFY(d);
        QCOMPARE(d->ip, std::string("10.255.255.7"));
        QCOMPARE(qs8(d->info.alias), u"真的"_s);
        QVERIFY(d->info.https() && d->verified);
        QCOMPARE(changes.load(), 1); // nothing to show changed
        // The same announcement again: nothing told.
        discovery.heard(info(fp, "真的", "https"), "10.255.255.7", true);
        QCOMPARE(changes.load(), 1);
        // Showing its certificate again, it moves.
        discovery.heard(info(fp, "真的", "https"), "10.255.255.9", true);
        QCOMPARE(discovery.device(fp)->ip, std::string("10.255.255.9"));
        QCOMPARE(changes.load(), 2);
        // Renamed there: told.
        discovery.heard(info(fp, "改了名", "https"), "10.255.255.9", true);
        QCOMPARE(qs8(discovery.device(fp)->info.alias), u"改了名"_s);
        QCOMPARE(changes.load(), 3);

        // Nobody checked this HTTPS one either: still never made HTTP.
        const std::string other(64, 'B');
        discovery.heard(info(other, "老版本", "https"), "10.255.255.20", false);
        QCOMPARE(changes.load(), 4);
        discovery.heard(info(other, "老版本", "http"), "10.255.255.21", false);
        d = discovery.device(other);
        QVERIFY(d && d->info.https() && d->ip == "10.255.255.20");
        QCOMPARE(changes.load(), 4);
        // A LocalSend set to HTTP is taken as it is, and may move (nothing better is known of it).
        const std::string plain = "c0ffee-1234_abcd";
        discovery.heard(info(plain, "HTTP 的", "http"), "10.255.255.30", false);
        d = discovery.device(plain);
        QVERIFY(d && !d->info.https() && !d->verified);
        QCOMPARE(changes.load(), 5);
        discovery.heard(info(plain, "HTTP 的", "http"), "10.255.255.31", false);
        QCOMPARE(discovery.device(plain)->ip, std::string("10.255.255.31"));
        QCOMPARE(changes.load(), 6);
        discovery.heard(info(plain, "HTTP 的", "http"), "10.255.255.31", false);
        QCOMPARE(changes.load(), 6);
        QCOMPARE(discovery.devices().size(), std::size_t(3));
    }

    // ---- what the rows say (Rows.h) -------------------------------------

    void lastReceived()
    {
        using Age = LastReceived::Age;
        using Kind = ReceiveOutcome::Kind;
        const LastReceived::Clock::time_point t0 = LastReceived::Clock::now();
        LastReceived last;
        QCOMPARE(static_cast<int>(last.row(t0).age), static_cast<int>(Age::None));
        last.finished(receiveEnded(Kind::Received, 3, "手机"), t0);
        LastReceived::Row r = last.row(t0);
        QCOMPARE(static_cast<int>(r.age), static_cast<int>(Age::Fresh));
        QCOMPARE(r.files, std::size_t(3));
        QCOMPARE(qs8(r.sender), u"手机"_s);
        QCOMPARE(qs(r.folder), u"C:\\收件"_s);
        // Nothing came of these: the row before stays (the live test of #113 lost it so).
        for (const Kind kind : {Kind::Declined, Kind::TimedOut, Kind::NoSpace, Kind::SenderCancelled, Kind::Cancelled, Kind::Failed}) {
            last.finished(receiveEnded(kind, 0, "别人"), t0 + 1s);
            r = last.row(t0 + 1s);
            QCOMPARE(r.files, std::size_t(3));
            QCOMPARE(qs8(r.sender), u"手机"_s);
            QVERIFY(r.at == t0);
        }
        // Given up by the sender after two came whole: those two.
        last.finished(receiveEnded(Kind::SenderCancelled, 2, "电脑"), t0 + 10s);
        r = last.row(t0 + 10s);
        QCOMPARE(r.files, std::size_t(2));
        QCOMPARE(qs8(r.sender), u"电脑"_s);
        QVERIFY(r.at == t0 + 10s);
        // Failed with one saved: that one.
        const LastReceived::Clock::time_point at = t0 + 20s;
        last.finished(receiveEnded(Kind::Failed, 1, "平板"), at);
        r = last.row(at);
        QCOMPARE(r.files, std::size_t(1));
        QCOMPARE(qs8(r.sender), u"平板"_s);
        // First among the rows for kFresh, then further down, gone after kKeepOutcome.
        QCOMPARE(static_cast<int>(last.row(at + kFresh - 1ms).age), static_cast<int>(Age::Fresh));
        QCOMPARE(static_cast<int>(last.row(at + kFresh).age), static_cast<int>(Age::Older));
        QCOMPARE(static_cast<int>(last.row(at + kKeepOutcome - 1ms).age), static_cast<int>(Age::Older));
        r = last.row(at + kKeepOutcome);
        QCOMPARE(static_cast<int>(r.age), static_cast<int>(Age::None));
        QVERIFY(r.folder.empty()); // Enter on a row drawn before: no folder to open
        QVERIFY(kFresh < kKeepOutcome);
    }

    void deviceAndFolderWords()
    {
        DeviceInfo d;
        d.deviceType = "headless"; // the LocalSend CLI
        d.deviceModel = "CLI";
        QCOMPARE(qs8(deviceWords(d, true)), u"命令行工具"_s);
        QCOMPARE(qs8(deviceWords(d, false)), u"Command line"_s);
        d.deviceType = "mobile";
        d.deviceModel = "Pixel 8";
        QCOMPARE(qs8(deviceWords(d, true)), u"手机 · Pixel 8"_s);
        QCOMPARE(qs8(deviceWords(d, false)), u"Phone · Pixel 8"_s);
        d.deviceModel = "  phone ";
        QCOMPARE(qs8(deviceWords(d, true)), u"手机"_s); // only the kind again
        d.deviceModel.clear();
        QCOMPARE(qs8(deviceWords(d, true)), u"手机"_s);
        d.deviceType = "desktop";
        d.deviceModel = "Windows";
        QCOMPARE(qs8(deviceWords(d, true)), u"电脑 · Windows"_s);
        d.deviceType = "web";
        d.deviceModel = "Firefox";
        QCOMPARE(qs8(deviceWords(d, true)), u"网页 · Firefox"_s);
        d.deviceType = "server";
        d.deviceModel = "Server";
        QCOMPARE(qs8(deviceWords(d, true)), u"服务器"_s);
        d.deviceType = "fridge"; // a kind LocalSend does not know: a computer, its default
        d.deviceModel = "macOS";
        QCOMPARE(qs8(deviceWords(d, true)), u"电脑 · macOS"_s);
        d.deviceModel = std::string(100, 'x');
        QCOMPARE(deviceWords(d, true).size(), std::string("电脑 · ").size() + 40); // a long model cut short

        const std::wstring downloads = L"C:\\Users\\a\\Downloads";
        QCOMPARE(qs8(folderWords(L"", downloads, true)), u"下载"_s);
        QCOMPARE(qs8(folderWords(downloads, downloads, false)), u"Downloads"_s);
        QCOMPARE(qs8(folderWords(L"c:\\users\\A\\downloads\\", downloads, true)), u"下载"_s);
        QCOMPARE(qs8(folderWords(L"D:\\收件\\手机", downloads, true)), u"手机"_s);
        QCOMPARE(qs8(folderWords(L"D:\\收件\\手机\\", downloads, true)), u"手机"_s);
        QCOMPARE(qs8(folderWords(L"D:\\", downloads, true)), u"D:"_s);
        QCOMPARE(qs8(folderWords(L"\\\\nas\\共享\\", downloads, true)), u"共享"_s);
    }

    void sentRows()
    {
        using Kind = SendOutcome::Kind;
        SendOutcome o;
        o.device = "手机";
        o.files = 3;
        o.bytes = 2048;
        const auto row = [&](Kind kind, bool chinese = true) {
            o.kind = kind;
            return sentRow(o, chinese);
        };
        std::optional<SentRow> r = row(Kind::Sent);
        QVERIFY(r);
        QCOMPARE(qs8(r->title), u"手机 收下了 3 个文件"_s);
        QVERIFY(qs8(r->subtitle).endsWith(u"Enter 再发一次"_s));
        QCOMPARE(static_cast<int>(r->enter), static_cast<int>(SentRow::Enter::Resend));
        QCOMPARE(qs8(r->enterTitle), u"再发一次"_s);
        r = row(Kind::TextSent);
        QVERIFY(r);
        QCOMPARE(qs8(r->title), u"手机 收到了文字"_s);
        QCOMPARE(static_cast<int>(r->enter), static_cast<int>(SentRow::Enter::Resend));
        // Not taken: Enter sends the same again.
        const std::map<Kind, QString> titles {
            {Kind::Declined, u"手机 没有接收"_s},
            {Kind::Busy, u"手机 正忙"_s},
            {Kind::Paused, u"手机 刚拒绝过，等一会儿再发"_s}, // Win顺's pause after a no: not "busy"
            {Kind::NoAnswer, u"手机 没有回应"_s},
            {Kind::PinRefused, u"没有发给 手机：对方要密码"_s},
            {Kind::TooManyPins, u"手机 说密码错了太多次"_s},
            {Kind::Unreachable, u"连不上 手机"_s},
            {Kind::Interrupted, u"没发完：发了 3 个就断了"_s},
            {Kind::NoRoom, u"手机 存不下"_s}, // Win顺 there: no space, or where it saves is gone
        };
        for (const auto& [kind, title] : titles) {
            r = row(kind);
            QVERIFY(r);
            QCOMPARE(qs8(r->title), title);
            QVERIFY2(qs8(r->subtitle).endsWith(u" · Enter 重发"_s), r->subtitle.c_str());
            QCOMPARE(static_cast<int>(r->enter), static_cast<int>(SentRow::Enter::Resend));
            QCOMPARE(qs8(r->enterTitle), u"重发"_s);
        }
        o.text = true; // a text a moment ago
        r = row(Kind::Paused);
        QVERIFY(r);
        QCOMPARE(qs8(r->title), u"手机 刚收到过文字，等几秒再发"_s);
        o.text = false;
        // Some other device at its address: Enter looks for devices instead.
        r = row(Kind::NotTheDevice);
        QVERIFY(r);
        QVERIFY(qs8(r->subtitle).endsWith(u"Enter 再找一次"_s));
        QCOMPARE(static_cast<int>(r->enter), static_cast<int>(SentRow::Enter::Search));
        QCOMPARE(qs8(r->enterTitle), u"再找一次"_s);
        // More files than Win顺 there takes at once: the same again would be
        // refused again, so Enter only puts the row away.
        r = row(Kind::TooMany);
        QVERIFY(r);
        QCOMPARE(qs8(r->title), u"手机 一次收不了这么多文件"_s);
        QVERIFY(qs8(r->subtitle).endsWith(u"Enter 知道了"_s));
        QCOMPARE(static_cast<int>(r->enter), static_cast<int>(SentRow::Enter::Dismiss));
        // Cancelled here, nothing to send: no row.
        QVERIFY(!row(Kind::Cancelled));
        QVERIFY(!row(Kind::Nothing));
        // English, in step.
        r = row(Kind::Declined, false);
        QVERIFY(r);
        QCOMPARE(qs8(r->title), u"手机 declined"_s);
        QCOMPARE(qs8(r->enterTitle), u"Send again"_s);
        QVERIFY(qs8(r->subtitle).endsWith(u"Enter sends it again"_s));
        r = row(Kind::Sent, false);
        QVERIFY(r);
        QCOMPARE(qs8(r->title), u"手机 took 3 files"_s);
        QCOMPARE(qs8(r->enterTitle), u"Send once more"_s);
    }

    void notificationTitles()
    {
        // Our words first, then who: a peer's name never reads as the start
        // of a message (Win顺 puts "局域网传输：" in front, and keeps 63
        // characters: the tray's title holds 64 with its NUL).
        QCOMPARE(qs8(titled("发来一段文字", "手机")), u"发来一段文字 · 手机"_s);
        QCOMPARE(qs8(titled("Sent you text", "Phone")), u"Sent you text · Phone"_s);
        // One line, nothing turning it around, no spaces kept around it.
        QCOMPARE(qs8(titled("发来一段文字", "  a\nb\xE2\x80\xAE" "c ")), u"发来一段文字 · a b_c"_s);
        // Cut to kNameInTitle characters, never inside a four-byte one.
        std::string longName;
        for (int i = 0; i < 30; ++i)
            longName += "长";
        QCOMPARE(qs8(titled("收到了 3 个文件", longName)), u"收到了 3 个文件 · "_s + QString(qsizetype(kNameInTitle), u'长') + u"…"_s);
        std::string smiles = "a";
        for (int i = 0; i < 25; ++i)
            smiles += "\xF0\x9F\x98\x80";
        const std::string cut = titled("x", smiles);
        QVERIFY(isUtf8(cut));
        QCOMPARE(qs8(cut).size(), qsizetype(4 + 1 + 18 + 1)); // "x · ", 'a', nine smiles, "…"
        // The longest words and the longest name fit in 63 with the prefix.
        QVERIFY(qs8("局域网传输：" + titled("保存位置不可用，没有接收", longName)).size() <= 63);
        QVERIFY(qs8("局域网传输：" + titled("对方一次收不了这么多文件", longName)).size() <= 63);
        QVERIFY(qs8("LAN transfer: " + titled("Save location unavailable", longName)).size() <= 63);
        QVERIFY(qs8("LAN transfer: " + titled("Wants to send 10000 files", longName)).size() <= 63);
        QVERIFY(qs8("LAN transfer: " + titled("Cancelled by the sender", longName)).size() <= 63);
    }

    void toastOpensDriveLettersOnly()
    {
        // Win顺 opens only a drive letter's path from a notification; the
        // 收到了 one for a share opens 传 instead (its row opens the folder).
        QVERIFY(toastOpens(L"C:\\Users\\a\\Downloads"));
        QVERIFY(toastOpens(L"z:/收件"));
        QVERIFY(toastOpens(L"D:\\"));
        QVERIFY(!toastOpens(L"\\\\nas\\share\\收件"));
        QVERIFY(!toastOpens(L"\\\\?\\C:\\in"));
        QVERIFY(!toastOpens(L"C:"));
        QVERIFY(!toastOpens(L"C:in"));
        QVERIFY(!toastOpens(L"1:\\in"));
        QVERIFY(!toastOpens(L"C:\\in:stream"));
        QVERIFY(!toastOpens(L""));
    }

    void clipboardTextKey()
    {
        // What the clipboard's text was when a row offered it: Ctrl+Enter
        // sends it only while it is the same, any change counts.
        QCOMPARE(textKey(""), std::string());
        QCOMPARE(textKey("晚上七点见").size(), std::size_t(16));
        QCOMPARE(textKey("晚上七点见"), textKey(std::string("晚上") + "七点见"));
        QVERIFY(textKey("晚上七点见") != textKey("晚上八点见"));
        QVERIFY(textKey("a") != textKey("a "));
    }

    void readSlots()
    {
        // File lists read at a time: one per address, two in all; given back, free again.
        ReadSlots s;
        QVERIFY(s.claim("10.0.0.1"));
        QVERIFY(!s.claim("10.0.0.1"));
        QVERIFY(s.claim("10.0.0.2"));
        QVERIFY(!s.claim("10.0.0.3")); // two in all
        QCOMPARE(s.taken(), ReadSlots::kInAll);
        s.release("10.0.0.1");
        QCOMPARE(s.taken(), std::size_t(1));
        QVERIFY(s.claim("10.0.0.3"));
        s.release("10.0.0.9"); // never claimed: nothing changes
        QCOMPARE(s.taken(), std::size_t(2));
        s.release("10.0.0.2");
        s.release("10.0.0.3");
        s.release("10.0.0.3"); // once too often: nothing below none
        QCOMPARE(s.taken(), std::size_t(0));
        QVERIFY(s.claim("10.0.0.1"));
        QVERIFY(!s.claim("10.0.0.1"));
    }

    void requestsPerSecond()
    {
        using Clock = PerSecond::Clock;
        const Clock::time_point t0 = Clock::now();
        PerSecond limit(Receiver::kRequestsPerSecond);
        for (int i = 0; i < Receiver::kRequestsPerSecond; ++i)
            QVERIFY(limit.allow("10.0.0.1", t0 + std::chrono::milliseconds(i)));
        QVERIFY(!limit.allow("10.0.0.1", t0 + 500ms)); // the 21st in that second
        QVERIFY(limit.allow("10.0.0.2", t0 + 500ms)); // each address its own
        QVERIFY(!limit.allow("10.0.0.1", t0 + 999ms));
        QVERIFY(limit.allow("10.0.0.1", t0 + 1000ms)); // a second on
        // Addresses by the hundred: the ones whose second is over make room.
        for (int i = 0; i < 300; ++i)
            limit.allow("10.1." + std::to_string(i / 256) + "." + std::to_string(i % 256), t0 + 1000ms);
        QVERIFY(limit.keys() > PerSecond::kPruneAt);
        QVERIFY(limit.allow("10.9.9.9", t0 + 2500ms));
        QCOMPARE(limit.keys(), std::size_t(1));

        // Notifications for texts from many devices: at most so many in a while, all together.
        Window w(5, 1min);
        for (int i = 0; i < 5; ++i)
            QVERIFY(w.allow(t0 + std::chrono::seconds(i)));
        QVERIFY(!w.allow(t0 + 10s));
        QVERIFY(!w.allow(t0 + 59s));
        QVERIFY(w.allow(t0 + 60s)); // the first is a minute old; refusals were not counted
        QVERIFY(!w.allow(t0 + 60s + 500ms));
        QVERIFY(w.allow(t0 + 62s));
    }

    void folderChecked()
    {
        // Where received files go, before anybody is asked: there, or to be
        // made inside a folder that is; never made while looking.
        QTemporaryDir dir;
        const std::wstring folder = native(dir.path());
        QVERIFY(folderUsable(folder));
        QVERIFY(folderUsable(folder + L"\\"));
        QVERIFY(folderUsable(folder + L"\\新的\\更深"));
        QVERIFY(!std::filesystem::exists(std::filesystem::path(folder + L"\\新的")));
        QVERIFY(folderUsable(folder.substr(0, 3))); // its drive's root
        QVERIFY(writeFile(folder + L"\\a.txt", "x"));
        QVERIFY(!folderUsable(folder + L"\\a.txt")); // a file
        QVERIFY(!folderUsable(folder + L"\\a.txt\\里面"));
        QVERIFY(!folderUsable(L""));
        // A drive that is not there (a USB disk pulled out).
        const DWORD drives = ::GetLogicalDrives();
        for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
            if (!(drives & (1u << (letter - L'A')))) {
                const std::wstring root = std::wstring(1, letter) + L":\\";
                QVERIFY(!folderUsable(root));
                QVERIFY(!folderUsable(root + L"收件"));
                QVERIFY(!folderUsable(std::wstring(1, letter) + L":"));
                break;
            }
        }
    }

    void localSendFileList()
    {
        const std::string fp(64, 'A');
        const std::string one = localSendList(1, fp);
        const std::string many = localSendList(kMaxFiles, fp);
        // 10,000 files: few enough values to parse (some 10 each), all taken.
        QVERIFY(fewValues(many, kMaxValues));
        QVERIFY(!fewValues(many, 10 * kMaxFiles));
        const std::optional<Json> json = parsePeerJson(many, kMaxValues);
        QVERIFY(json);
        std::string why;
        const std::optional<PrepareUpload> p = parsePrepareUpload(*json, &why);
        QVERIFY2(p, why.c_str());
        QCOMPARE(p->files.size(), kMaxFiles);
        // The bytes: some 280 a file, 2.8 MB for 10,000, inside what Win顺
        // reads (the rest is room for longer names).
        const std::size_t perFile = (many.size() - one.size()) / (kMaxFiles - 1);
        QVERIFY2(perFile > 250 && perFile < 320, std::to_string(perFile).c_str());
        QVERIFY2(many.size() > 2'500'000 && many.size() < Receiver::kMaxPrepareBytes, std::to_string(many.size()).c_str());
        QVERIFY(Receiver::kMaxPrepareBytes / kMaxFiles > 400); // a name of some 150 bytes each still fits
    }

    void backgroundWork()
    {
        // Made on a thread of its own: the asker waits a moment at most, and
        // hears of it later; never behind another being made.
        std::atomic<int> told {0};
        Background<int> work([&told] { ++told; });
        std::atomic<bool> go {false};
        std::atomic<bool> entered {false};
        const auto slow = [&go, &entered] {
            entered = true;
            while (!go)
                ::Sleep(5);
            return std::optional<int>(7);
        };
        auto started = std::chrono::steady_clock::now();
        QVERIFY(!work.get("a", slow, 1min, 50ms));
        QVERIFY(std::chrono::steady_clock::now() - started < 1s);
        QVERIFY(QTest::qWaitFor([&] { return entered.load(); }, 5000)); // "a" is being made
        started = std::chrono::steady_clock::now();
        QVERIFY(!work.get("b", [] { return std::optional<int>(8); }, 1min, 2000ms));
        QVERIFY(std::chrono::steady_clock::now() - started < 500ms);
        go = true;
        QVERIFY(QTest::qWaitFor([&] { return told >= 2; }, 5000));
        QCOMPARE(work.get("a", slow, 1min, 0ms).value_or(-1), 7);
        QCOMPARE(work.get("b", [] { return std::optional<int>(8); }, 1min, 0ms).value_or(-1), 8);
        // Old: the value meanwhile, made anew; the same again is not told.
        const int before = told;
        QCOMPARE(work.get("a", slow, 0ms, 0ms).value_or(-1), 7);
        QTest::qWait(150);
        QCOMPARE(told.load(), before);
        // Nothing made: not kept (asked again next time), not told, not waited for long.
        std::atomic<int> tries {0};
        const auto none = [&tries] {
            ++tries;
            return std::optional<int>();
        };
        started = std::chrono::steady_clock::now();
        QVERIFY(!work.get("c", none, 1min, 2000ms));
        QVERIFY(!work.get("c", none, 1min, 2000ms));
        QVERIFY(std::chrono::steady_clock::now() - started < 1500ms);
        QCOMPARE(tries.load(), 2);
        QCOMPARE(told.load(), before);
        // Stopped: nothing more made or told.
        work.stop();
        QVERIFY(!work.get("d", [] { return std::optional<int>(1); }, 1min, 100ms));
        QCOMPARE(told.load(), before);
    }

    // ---- a receiver and a sender over 127.0.0.1 -----------------------

    void filesAccepted()
    {
        QVERIFY(startReceiver());
        const std::wstring source = native(m_dir.path() + u"/src-files"_s);
        const std::string photo = randomBytes(300000);
        const std::string deep = randomBytes(70000);
        QVERIFY(writeFile(source + L"\\a.txt", "hello 你好"));
        QVERIFY(writeFile(source + L"\\相册\\IMG_1.jpg", photo));
        QVERIFY(writeFile(source + L"\\相册\\深\\b.bin", deep));
        QVERIFY(writeFile(source + L"\\空.txt", ""));
        Outgoing what;
        what.paths = {source + L"\\a.txt", source + L"\\相册", source + L"\\空.txt"};

        for (int round = 0; round < 2; ++round) {
            auto job = send(what);
            QVERIFY(waitFor([&] { return m_asked.size() == std::size_t(round + 1); }));
            IncomingRequest asked;
            {
                const std::lock_guard lock(m_mutex);
                asked = m_asked.back();
            }
            QCOMPARE(qs8(asked.sender.alias), u"发送端"_s);
            QVERIFY(asked.verified); // it showed the certificate its fingerprint names
            QCOMPARE(asked.files.size(), std::size_t(4));
            QCOMPARE(asked.totalBytes, std::uint64_t(12 + 300000 + 70000));
            QStringList names;
            for (const FileMeta& f : asked.files)
                names.push_back(qs8(f.fileName));
            names.sort();
            QCOMPARE(names, (QStringList {u"a.txt"_s, u"相册/IMG_1.jpg"_s, u"相册/深/b.bin"_s, u"空.txt"_s}));
            QVERIFY(m_receiver->pending().has_value());
            QVERIFY(m_receiver->decide(asked.id, true));
            QVERIFY(!m_receiver->decide(asked.id, false)); // answered once
            const std::optional<SendOutcome> sent = ended(*job);
            QVERIFY(sent);
            QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Sent));
            QCOMPARE(sent->files, std::size_t(4));
            const std::optional<ReceiveOutcome> got = outcome(round);
            QVERIFY(got);
            QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::Received));
            QCOMPARE(got->saved.size(), std::size_t(4));
            QCOMPARE(got->failed, std::size_t(0));
            for (const std::wstring& path : got->saved)
                QVERIFY2(marked(path), qPrintable(qs(path)));
        }
        QCOMPARE(filesUnder(m_folder), (QStringList {u"a (1).txt"_s, u"a.txt"_s, u"相册 (1)\\IMG_1.jpg"_s, u"相册 (1)\\深\\b.bin"_s,
                                           u"相册\\IMG_1.jpg"_s, u"相册\\深\\b.bin"_s, u"空 (1).txt"_s, u"空.txt"_s}));
        QCOMPARE(readFile(m_folder + L"\\a.txt"), std::string("hello 你好"));
        QVERIFY(readFile(m_folder + L"\\相册\\IMG_1.jpg") == photo);
        QVERIFY(readFile(m_folder + L"\\相册 (1)\\深\\b.bin") == deep);
        QVERIFY(readFile(m_folder + L"\\空.txt").empty());
        QVERIFY(!m_receiver->pending());
        QVERIFY(!m_receiver->progress());
        // The log is kept: never a file name in it.
        for (const char* name : {"IMG_1", "a.txt", "相册", "空.txt", "b.bin"})
            QVERIFY2(!logsMention(name), name);
    }

    void listenAgainOnTheSamePort()
    {
        QVERIFY(startReceiver());
        m_answer = true;
        const Prepared p = prepare({meta("f1", "x.txt", 1)});
        QCOMPARE(p.status, 200);
        QCOMPARE(upload(p, "f1", "x"), 200);
        QVERIFY(outcome());
        const int before = m_port;
        // Receiving switched off and on (or the networks changed) right after a transfer.
        m_receiver->stopListening();
        QCOMPARE(m_receiver->port(), 0);
        const int again = m_receiver->listen({{"127.0.0.1", INADDR_LOOPBACK, 8}}, kPort);
        QCOMPARE(again, before);
    }

    void declined()
    {
        QVERIFY(startReceiver());
        m_answer = false;
        const std::wstring source = native(m_dir.path() + u"/src-declined"_s);
        QVERIFY(writeFile(source + L"\\no.txt", "no"));
        Outgoing what;
        what.paths = {source + L"\\no.txt"};
        auto job = send(what);
        const std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Declined));
        const std::optional<ReceiveOutcome> got = outcome();
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::Declined));
        QVERIFY(filesUnder(m_folder).isEmpty());
    }

    void busy()
    {
        QVERIFY(startReceiver());
        const std::wstring source = native(m_dir.path() + u"/src-busy"_s);
        QVERIFY(writeFile(source + L"\\one.txt", "1"));
        Outgoing what;
        what.paths = {source + L"\\one.txt"};
        auto first = send(what);
        QVERIFY(waitFor([&] { return m_asked.size() == 1; }));
        auto second = send(what);
        const std::optional<SendOutcome> refused = ended(*second);
        QVERIFY(refused);
        QCOMPARE(static_cast<int>(refused->kind), static_cast<int>(SendOutcome::Kind::Busy));
        QCOMPARE(m_asked.size(), std::size_t(1)); // nobody was asked twice
        QVERIFY(m_receiver->decide(m_asked.front().id, false));
        QVERIFY(ended(*first));
    }

    void pin()
    {
        QVERIFY(startReceiver("4321"));
        const std::wstring source = native(m_dir.path() + u"/src-pin"_s);
        QVERIFY(writeFile(source + L"\\secret.txt", "s"));
        Outgoing what;
        what.paths = {source + L"\\secret.txt"};
        auto job = send(what);
        QVERIFY(viewIs(*job, SendView::State::NeedsPin));
        QVERIFY(m_asked.empty()); // nobody is asked before the PIN is right
        QVERIFY(job->job->givePin("0000"));
        QVERIFY(viewIs(*job, SendView::State::NeedsPin, true));
        QVERIFY(job->job->givePin("4321"));
        QVERIFY(waitFor([&] { return m_asked.size() == 1; }));
        QVERIFY(m_receiver->decide(m_asked.front().id, true));
        const std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Sent));
        QCOMPARE(filesUnder(m_folder), QStringList {u"secret.txt"_s});
    }

    void pinLockout()
    {
        QVERIFY(startReceiver("4321"));
        const std::wstring source = native(m_dir.path() + u"/src-lockout"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        auto job = send(what);
        QVERIFY(viewIs(*job, SendView::State::NeedsPin));
        for (int i = 0; i < 2; ++i) {
            QVERIFY(job->job->givePin("000" + std::to_string(i)));
            QVERIFY(viewIs(*job, SendView::State::NeedsPin, true));
        }
        QVERIFY(job->job->givePin("0009")); // the third wrong one: 429
        std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::TooManyPins));
        // Blocked for a while now, even with the right one.
        const Prepared p = prepare({meta("f1", "x.txt", 1)}, "?pin=4321");
        QCOMPARE(p.status, 429);
        QVERIFY(m_asked.empty());
    }

    void textMessage()
    {
        QVERIFY(startReceiver());
        Outgoing what;
        what.kind = Outgoing::Kind::Text;
        what.text = "你好，第二行\n\xF0\x9F\x99\x82";
        auto job = send(what);
        std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::TextSent));
        QVERIFY(waitFor([&] { return m_texts.size() == 1; }));
        QCOMPARE(qs8(m_texts.front().text), qs8(what.text));
        QCOMPARE(qs8(m_texts.front().sender.alias), u"发送端"_s);
        QVERIFY(m_asked.empty());
        QVERIFY(m_outcomes.empty());
        QVERIFY(filesUnder(m_folder).isEmpty()); // shown, never written
        QVERIFY(!logsMention("第二行"));
        // A second one at once: one per sender in five seconds.
        auto again = send(what);
        sent = ended(*again);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Paused)); // "in a few seconds"
        QVERIFY(sent->text);
        QCOMPARE(m_texts.size(), std::size_t(1));
    }

    void cancelWhileAsking()
    {
        QVERIFY(startReceiver());
        const std::wstring source = native(m_dir.path() + u"/src-asking"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};

        // This side: the sender hears no.
        auto job = send(what);
        QVERIFY(waitFor([&] { return m_asked.size() == 1; }));
        QVERIFY(m_receiver->cancel());
        std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Declined));
        std::optional<ReceiveOutcome> got = outcome(0);
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::Cancelled));

        // The sender gives up: the request is gone from the list.
        job = send(what);
        QVERIFY(waitFor([&] { return m_asked.size() == 2; }));
        job->job->cancel();
        sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Cancelled));
        got = outcome(1);
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::SenderCancelled));
        QVERIFY(!m_receiver->pending());
        QVERIFY(filesUnder(m_folder).isEmpty());
    }

    void cancelMidFile_data()
    {
        QTest::addColumn<QString>("how");
        QTest::newRow("receiver cancels") << u"receiver"_s;
        QTest::newRow("sender says cancel") << u"cancel"_s;
        QTest::newRow("sender drops the connection") << u"drop"_s;
    }

    void cancelMidFile()
    {
        QFETCH(QString, how);
        QVERIFY(startReceiver());
        m_answer = true;
        constexpr std::uint64_t size = 8 << 20;
        const Prepared p = prepare({meta("f1", "big.bin", size), meta("f2", "small.txt", 2)});
        QCOMPARE(p.status, 200);
        QCOMPARE(upload(p, "f2", "ok"), 200); // a whole file stays
        Raw c(m_port, *m_clientCredentials);
        QVERIFY(c.ok());
        QCOMPARE(qs8(c.peerFingerprint()), qs8(m_receiverIdentity->fingerprint()));
        QVERIFY(c.send(uploadHead(p.sessionId, "f1", p.tokens.at("f1"), size)));
        QVERIFY(c.send(randomBytes(1 << 20)));
        QVERIFY(QTest::qWaitFor([&] {
            const std::optional<ReceiveProgress> progress = m_receiver->progress();
            return progress && progress->receivedBytes >= (1u << 20) + 2;
        }, 10000));
        QVERIFY(filesUnder(m_folder).contains(u"big.bin.wspart"_s));
        ReceiveOutcome::Kind expected = ReceiveOutcome::Kind::Cancelled;
        if (how == u"receiver") {
            QVERIFY(m_receiver->cancel());
            QVERIFY(c.reply() != 200);
        } else if (how == u"cancel") {
            const Reply r = m_client->request(target(), "POST", "/api/localsend/v2/cancel?sessionId=" + http::percentEncode(p.sessionId), {}, 5s);
            QCOMPARE(r.status, 200);
            expected = ReceiveOutcome::Kind::SenderCancelled;
        } else {
            c.close();
            expected = ReceiveOutcome::Kind::Failed;
        }
        const std::optional<ReceiveOutcome> got = outcome();
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(expected));
        QCOMPARE(got->saved.size(), std::size_t(1));
        QCOMPARE(got->failed, std::size_t(1));
        // The partial file is gone; the whole one is kept.
        QVERIFY(QTest::qWaitFor([&] { return filesUnder(m_folder) == QStringList {u"small.txt"_s}; }, 5000));
        QVERIFY(!m_receiver->progress());
    }

    void tokensAndSizes()
    {
        QVERIFY(startReceiver());
        m_answer = true;
        const Prepared p = prepare({meta("f1", "one.txt", 5), meta("f2", "long.txt", 10), meta("f3", "chunked.txt", 10),
            meta("f4", "short.txt", 10), meta("f5", "sum.txt", 5, std::string(64, 'A'))});
        QCOMPARE(p.status, 200);
        QCOMPARE(p.tokens.size(), std::size_t(5));
        std::string body;
        QCOMPARE(upload(p, "f1", "hello"), 200);
        QCOMPARE(upload(p, "f1", "again", nullptr, &body), 409); // a token is good once
        const std::string other = p.tokens.at("f1");
        QCOMPARE(upload(p, "f2", "0123456789", &other, &body), 403); // another file's token
        QVERIFY(body.find("Invalid token") != std::string::npos);
        Prepared wrongSession = p;
        wrongSession.sessionId = "0123456789abcdef0123456789abcdef";
        QCOMPARE(upload(wrongSession, "f2", "0123456789", nullptr, &body), 403);
        QVERIFY(body.find("Invalid session") != std::string::npos);
        QCOMPARE(upload(p, "nope", "x", &other, &body), 403); // no such file

        QCOMPARE(upload(p, "f2", std::string(20, 'x'), nullptr, &body), 400); // more than declared
        QVERIFY(body.find("More than declared") != std::string::npos);
        {
            Raw c(m_port, *m_clientCredentials);
            QVERIFY(c.send("POST /api/localsend/v2/upload?sessionId=" + p.sessionId + "&fileId=f3&token=" + p.tokens.at("f3")
                + " HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nhello\r\nf\r\n0123456789abcde\r\n0\r\n\r\n"));
            QCOMPARE(c.reply(&body), 400);
            QVERIFY(body.find("More than declared") != std::string::npos);
        }
        QCOMPARE(upload(p, "f4", "short", nullptr, &body), 400); // fewer than declared
        QVERIFY2(body.find("Upload failed") != std::string::npos, body.c_str());
        const int sum = upload(p, "f5", "hello", nullptr, &body); // the checksum disagrees
        QVERIFY2(sum == 400 && body.find("Upload failed") != std::string::npos, (std::to_string(sum) + " " + body).c_str());
        const std::optional<ReceiveOutcome> got = outcome();
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::Failed));
        QCOMPARE(got->saved.size(), std::size_t(1));
        QCOMPARE(got->failed, std::size_t(4));
        QCOMPARE(filesUnder(m_folder), QStringList {u"one.txt"_s});
        QCOMPARE(readFile(m_folder + L"\\one.txt"), std::string("hello"));
        // The session is over: its tokens are worth nothing now.
        QCOMPARE(upload(p, "f2", "0123456789"), 403);
    }

    void hostileNames()
    {
        QVERIFY(startReceiver());
        m_answer = true;
        // One name that is not a name refuses the whole request: nobody is asked.
        for (const char* name : {"../evil.txt", "a/../../evil.txt", "C:/Windows/evil.dll", "C:evil.txt", "\\\\server\\share\\evil.txt",
                 "/etc/passwd", "a/ ../evil.txt"}) {
            const Prepared p = prepare({meta("ok", "fine.txt", 1), meta("bad", name, 1)});
            QVERIFY2(p.status == 400, name);
        }
        QVERIFY(m_asked.empty());

        struct Case {
            const char* sent;
            QString saved;
        };
        const std::vector<Case> cases = {
            {"CON.txt", u"_CON.txt"_s},
            {"com1", u"_com1"_s},
            {"a<b>c|d?.txt", u"a_b_c_d_.txt"_s},
            {"x.txt:Zone.Identifier", u"x.txt_Zone.Identifier"_s},
            {"ctl\x01" ".txt", u"ctl_.txt"_s},
            {"报告\xE2\x80\xAE" "fdp.exe", u"报告_fdp.exe"_s},
            {"trail. . ", u"trail"_s},
            {"dir/./sub//f.txt", u"dir\\sub\\f.txt"_s},
            {" ", u"文件"_s},
        };
        std::vector<FileMeta> files;
        for (std::size_t i = 0; i < cases.size(); ++i)
            files.push_back(meta("f" + std::to_string(i), cases[i].sent, 1));
        const Prepared p = prepare(files);
        QCOMPARE(p.status, 200);
        for (std::size_t i = 0; i < cases.size(); ++i)
            QCOMPARE(upload(p, "f" + std::to_string(i), "x"), 200);
        const std::optional<ReceiveOutcome> got = outcome();
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::Received));
        QStringList expected;
        for (const Case& c : cases)
            expected.push_back(c.saved);
        expected.sort();
        QCOMPARE(filesUnder(m_folder), expected);
        for (const std::wstring& path : got->saved) {
            QVERIFY(path.rfind(m_folder + L"\\", 0) == 0);
            QVERIFY(marked(path));
        }
        QVERIFY(!logsMention("报告"));
    }

    void limits()
    {
        QVERIFY(startReceiver());
        m_answer = true;
        std::string body;
        {
            Raw c(m_port, *m_clientCredentials);
            QVERIFY(c.send("POST /api/localsend/v2/prepare-upload HTTP/1.1\r\nContent-Length: 5000000\r\n\r\n"));
            QCOMPARE(c.reply(), 413); // more than 4 MB of JSON: not read at all
        }
        {
            Raw c(m_port, *m_clientCredentials);
            QVERIFY(c.send("POST /api/localsend/v2/register HTTP/1.1\r\nContent-Length: 70000\r\n\r\n"));
            QCOMPARE(c.reply(), 413);
        }
        {
            Raw c(m_port, *m_clientCredentials);
            QVERIFY(c.send("GET /api/localsend/v2/info HTTP/1.1\r\nX: " + std::string(17000, 'a') + "\r\n\r\n"));
            QCOMPARE(c.reply(), 400);
        }
        // More than the disk has: refused before anybody is asked.
        Prepared p = prepare({meta("f1", "huge.bin", 9000000000000000ull)});
        QCOMPARE(p.status, 403);
        QVERIFY(p.body.find("Not enough space") != std::string::npos);
        QVERIFY(m_asked.empty());
        std::optional<ReceiveOutcome> got = outcome(0);
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::NoSpace));
        // A byte, when the disk would keep less than its spare room (a gigabyte in the plugin):
        // refused too, but told once in a while (a sender asking again does not keep the list ringing).
        ReceiverConfig config = m_receiver->config();
        config.spareBytes = UINT64_MAX / 2;
        m_receiver->setConfig(config);
        p = prepare({meta("f1", "byte.bin", 1)});
        QCOMPARE(p.status, 403);
        QVERIFY(p.body.find("Not enough space") != std::string::npos);
        QTest::qWait(200);
        {
            const std::lock_guard lock(m_mutex);
            QCOMPARE(m_outcomes.size(), std::size_t(1));
        }
        QCOMPARE(ReceiverConfig().spareBytes, std::uint64_t(1) << 30);
        QCOMPARE(m_client->request(target(), "GET", "/nothing", {}, 5s).status, 404);
    }

    void registerAndInfo()
    {
        QVERIFY(startReceiver());
        Reply r = m_client->request(target(), "POST", "/api/localsend/v2/register", toJson(senderInfo(), true).dump(), 5s);
        QCOMPARE(r.status, 200);
        std::optional<Json> json = Json::parse(r.body);
        QVERIFY(json);
        QCOMPARE(qs8((*json)["alias"].toString()), u"接收端"_s);
        QCOMPARE((*json)["fingerprint"].toString(), m_receiverIdentity->fingerprint());
        QCOMPARE(r.peerFingerprint, m_receiverIdentity->fingerprint());
        QVERIFY(waitFor([&] { return m_registered.size() == 1; }));
        QCOMPARE(qs8(m_registered.front().first.alias), u"发送端"_s);
        QVERIFY(m_registered.front().second); // verified
        // A register in another's name (the certificate is not the one it names): ignored.
        DeviceInfo liar = senderInfo();
        liar.fingerprint = std::string(64, 'E');
        r = m_client->request(target(), "POST", "/api/localsend/v2/register", toJson(liar, true).dump(), 5s);
        QCOMPARE(r.status, 200);
        QTest::qWait(200);
        QCOMPARE(m_registered.size(), std::size_t(1));
        r = m_client->request(target(), "GET", "/api/localsend/v2/info", {}, 5s);
        QCOMPARE(r.status, 200);
        json = Json::parse(r.body);
        QVERIFY(json && (*json)["fingerprint"].toString() == m_receiverIdentity->fingerprint());
        QVERIFY(!(*json).has("port")); // info says no port, register's multicast does
    }

    void wrongFingerprint()
    {
        QVERIFY(startReceiver());
        Target pinned = target();
        pinned.fingerprint = std::string(64, '0');
        const Reply r = m_client->request(pinned, "POST", "/api/localsend/v2/register", toJson(senderInfo(), true).dump(), 5s);
        QCOMPARE(r.status, 0); // ended before a byte of the request went
        QVERIFY(r.wrongCertificate);
        const std::wstring source = native(m_dir.path() + u"/src-pinned"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        auto job = send(what, pinned);
        const std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        // Another device at that address now: said so, not "could not reach".
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::NotTheDevice));
        QVERIFY(m_asked.empty());
        QVERIFY(m_registered.empty());
    }

    void untrustedPeers()
    {
        QVERIFY(startReceiver());
        // Bad bytes in a register's name: the device is heard, its name UTF-8.
        const std::string fp = m_senderIdentity->fingerprint();
        Reply r = m_client->request(target(), "POST", "/api/localsend/v2/register",
            "{\"alias\":\"A\xFF\xFE\",\"fingerprint\":\"" + fp + "\",\"port\":53317,\"protocol\":\"https\"}", 5s);
        QCOMPARE(r.status, 200);
        QVERIFY(waitFor([&] { return m_registered.size() == 1; }));
        QCOMPARE(m_registered.front().first.alias, std::string("A\xEF\xBF\xBD\xEF\xBF\xBD"));
        // A fingerprint that is not one: no device, no request.
        DeviceInfo odd = senderInfo();
        odd.fingerprint = "not a fingerprint";
        r = m_client->request(target(), "POST", "/api/localsend/v2/register", toJson(odd, true).dump(), 5s);
        QCOMPARE(r.status, 200);
        PrepareUpload p;
        p.info = odd;
        p.files = {meta("f1", "x.txt", 1)};
        r = m_client->request(target(), "POST", "/api/localsend/v2/prepare-upload", toJson(p).dump(), 5s);
        QCOMPARE(r.status, 400);
        QTest::qWait(200);
        QCOMPARE(m_registered.size(), std::size_t(1));
        QVERIFY(m_asked.empty());

        // A megabyte of "[0,0,0,…]", too many values: refused, not parsed
        // (more than 4 MB is not read: limits()). The next request is fine.
        std::string many = "[";
        while (many.size() < 900'000)
            many += "0,";
        many += "0]";
        QVERIFY(many.size() < Receiver::kMaxPrepareBytes);
        r = m_client->request(target(), "POST", "/api/localsend/v2/prepare-upload", many, 20s);
        QCOMPARE(r.status, 413);
        m_answer = true;
        const Prepared ok = prepare({meta("f1", "x.txt", 1)});
        QCOMPARE(ok.status, 200);
        QCOMPARE(upload(ok, "f1", "x"), 200);
        QVERIFY(outcome());
    }

    void refusedBeforeTheBody()
    {
        // No PIN: answered at once; the four megabytes it says will come are never waited for.
        QVERIFY(startReceiver("4321"));
        const std::string head = "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\nContent-Length: 4000000\r\n\r\n";
        std::string body;
        auto started = std::chrono::steady_clock::now();
        {
            Raw c(m_port, *m_clientCredentials);
            QVERIFY(c.send(head));
            QCOMPARE(c.reply(&body), 401);
            QVERIFY(body.find("PIN required") != std::string::npos);
        }
        {
            Raw c(m_port, *m_clientCredentials);
            QVERIFY(c.send("POST /api/localsend/v2/prepare-upload?pin=0000 HTTP/1.1\r\nContent-Length: 4000000\r\n\r\n"));
            QCOMPARE(c.reply(&body), 401);
            QVERIFY(body.find("Invalid PIN") != std::string::npos);
        }
        QVERIFY(std::chrono::steady_clock::now() - started < 5s);

        // Busy: the same.
        QVERIFY(startReceiver());
        const std::wstring source = native(m_dir.path() + u"/src-unread"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        auto job = send(what);
        QVERIFY(waitFor([&] { return m_asked.size() == 1; }));
        started = std::chrono::steady_clock::now();
        {
            Raw c(m_port, *m_clientCredentials);
            QVERIFY(c.send(head));
            QCOMPARE(c.reply(&body), 409);
        }
        QVERIFY(std::chrono::steady_clock::now() - started < 5s);
        QVERIFY(m_receiver->decide(m_asked.front().id, true));
        const std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Sent));
    }

    void oneFileListAtATime()
    {
        // A peer whose file list comes slowly holds its address's one slot:
        // another prepare-upload from there is refused before its body is read.
        QVERIFY(startReceiver());
        m_answer = true;
        {
            Raw slow(m_port, *m_clientCredentials);
            QVERIFY(slow.send("POST /api/localsend/v2/prepare-upload HTTP/1.1\r\nContent-Length: 500000\r\n\r\n{\"info\":"));
            QTest::qWait(500); // read up to its body
            const auto started = std::chrono::steady_clock::now();
            const Prepared refused = prepare({meta("f1", "x.txt", 1)});
            QCOMPARE(refused.status, 409);
            QVERIFY(std::chrono::steady_clock::now() - started < 5s);
            QVERIFY(m_asked.empty());
        } // closed: its slot given back
        Prepared ok;
        QVERIFY(QTest::qWaitFor([&] {
            ok = prepare({meta("f1", "x.txt", 1)});
            return ok.status == 200;
        }, 5000));
        QCOMPARE(upload(ok, "f1", "x"), 200);
        QVERIFY(outcome());
    }

    void requestLimit()
    {
        // Any route counts, info too: asked again and again, the connection is cut off.
        QVERIFY(startReceiver());
        const std::string info = "GET /api/localsend/v2/info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        Raw c(m_port, *m_clientCredentials);
        QVERIFY(c.ok());
        int answered = 0;
        int status = 0;
        for (int i = 0; i < 200; ++i) {
            status = c.send(info) ? c.reply() : 0;
            if (status != 200)
                break;
            ++answered;
        }
        QCOMPARE(status, 429);
        QVERIFY2(answered >= Receiver::kRequestsPerSecond, std::to_string(answered).c_str());
        QCOMPARE(c.send(info) ? c.reply() : 0, 0); // closed after it
        {
            // Another connection from there within that second: the same.
            Raw again(m_port, *m_clientCredentials);
            QVERIFY(again.ok());
            QVERIFY(again.send(info));
            QCOMPARE(again.reply(), 429);
        }
        QTest::qWait(1100);
        Raw later(m_port, *m_clientCredentials);
        QVERIFY(later.send(info));
        QCOMPARE(later.reply(), 200);
    }

    void uploadsNotLimited()
    {
        // A folder of small files is many uploads a second: none cut off
        // (they belong to the transfer under way; their tokens are checked).
        QVERIFY(startReceiver());
        m_answer = true;
        constexpr int kFiles = 150;
        const std::wstring source = native(m_dir.path() + u"/src-many"_s);
        for (int i = 0; i < kFiles; ++i)
            QVERIFY(writeFile(source + L"\\" + std::to_wstring(i) + L".txt", std::to_string(i)));
        Outgoing what;
        what.paths = {source};
        auto job = send(what);
        const std::optional<SendOutcome> sent = ended(*job, 60000);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Sent));
        QCOMPARE(sent->files, std::size_t(kFiles));
        const std::optional<ReceiveOutcome> got = outcome();
        QVERIFY(got);
        QCOMPARE(got->saved.size(), std::size_t(kFiles));
        QCOMPARE(got->failed, std::size_t(0));
    }

    void noPlaceToSave()
    {
        // Where files go is on a drive that is not there (a USB disk pulled
        // out): refused before anybody is asked, else every file would fail.
        wchar_t letter = 0;
        const DWORD drives = ::GetLogicalDrives();
        for (wchar_t l = L'Z'; l >= L'D' && !letter; --l) {
            if (!(drives & (1u << (l - L'A'))))
                letter = l;
        }
        if (!letter)
            QSKIP("every drive letter is taken");
        QVERIFY(startReceiver());
        m_answer = true;
        ReceiverConfig config = m_receiver->config();
        config.folder = std::wstring(1, letter) + L":\\收件";
        m_receiver->setConfig(config);
        const Prepared p = prepare({meta("f1", "x.txt", 1)});
        QCOMPARE(p.status, 403);
        QVERIFY(p.body.find("Save location unavailable") != std::string::npos);
        QVERIFY(m_asked.empty());
        const std::optional<ReceiveOutcome> got = outcome(0);
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::NoFolder));
        // Win顺 sending there says it has no room: not "declined".
        const std::wstring source = native(m_dir.path() + u"/src-nofolder"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        auto job = send(what);
        const std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::NoRoom));
        QVERIFY(m_asked.empty());
        QTest::qWait(200);
        const std::lock_guard lock(m_mutex);
        QCOMPARE(m_outcomes.size(), std::size_t(1)); // told once in a while, not each time
    }

    void tenThousandFiles()
    {
        // LocalSend's most in one request, 10,000 photos as it words them
        // (some 2.8 MB): read, parsed and asked about. One more file: 413,
        // which another Win顺 says as "fewer at once", not "malformed".
        QVERIFY(startReceiver());
        const std::string fp = m_senderIdentity->fingerprint();
        const std::string tooMany = localSendList(kMaxFiles + 1, fp);
        QVERIFY(tooMany.size() < Receiver::kMaxPrepareBytes);
        Reply r = m_client->request(target(), "POST", "/api/localsend/v2/prepare-upload", tooMany, 30s);
        QCOMPARE(r.status, 413);
        QVERIFY(r.body.find("Too many files") != std::string::npos);
        QVERIFY(m_asked.empty());

        m_answer = true;
        const std::string list = localSendList(kMaxFiles, fp, "2483"); // 25 MB in all: room on any disk
        QVERIFY2(list.size() > 2'700'000 && list.size() < Receiver::kMaxPrepareBytes, std::to_string(list.size()).c_str());
        constexpr std::size_t kMaxAnswer = kMaxFiles * 256 + Client::kMaxReply; // as SendJob takes it
        r = m_client->request(target(), "POST", "/api/localsend/v2/prepare-upload", list, 60s, nullptr, Client::kConnectTime, kMaxAnswer);
        QCOMPARE(r.status, 200);
        const std::optional<Json> answer = Json::parse(r.body);
        QVERIFY(answer);
        QCOMPARE((*answer)["files"].toObject().size(), kMaxFiles);
        {
            const std::lock_guard lock(m_mutex);
            QCOMPARE(m_asked.size(), std::size_t(1));
            QCOMPARE(m_asked.front().files.size(), kMaxFiles);
        }
        // The sender gives up: the transfer ends, nothing written.
        r = m_client->request(target(), "POST", "/api/localsend/v2/cancel?sessionId=" + http::percentEncode((*answer)["sessionId"].toString()), {}, 5s);
        QCOMPARE(r.status, 200);
        const std::optional<ReceiveOutcome> got = outcome(0);
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::SenderCancelled));
        QVERIFY(filesUnder(m_folder).isEmpty());
    }

    void pauseAfterNo()
    {
        QVERIFY(startReceiver());
        m_answer = false;
        Prepared p = prepare({meta("f1", "x.txt", 1)});
        QCOMPARE(p.status, 403);
        // Asking again at once: nobody is asked, the list does not ring.
        p = prepare({meta("f1", "x.txt", 1)});
        QCOMPARE(p.status, 429);
        QVERIFY(p.body.find("Too many requests") != std::string::npos);
        const std::wstring source = native(m_dir.path() + u"/src-pause"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        auto job = send(what);
        std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Paused)); // "it said no just now": not "busy"
        QVERIFY(!sent->text);
        // A text message is not held back by it.
        Outgoing text;
        text.kind = Outgoing::Kind::Text;
        text.text = "为什么？";
        job = send(text);
        sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::TextSent));
        const std::lock_guard lock(m_mutex);
        QCOMPARE(m_asked.size(), std::size_t(1));
        QCOMPARE(m_outcomes.size(), std::size_t(1)); // the no
        QCOMPARE(m_texts.size(), std::size_t(1));
    }

    void oneFreeResend()
    {
        // The wrong file picked and given up: sent again at once, the person
        // is asked again. Given up a second time within the minute: a pause.
        QVERIFY(startReceiver());
        const std::wstring source = native(m_dir.path() + u"/src-resend"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        for (std::size_t round = 1; round <= 2; ++round) {
            auto job = send(what);
            QVERIFY(waitFor([&] { return m_asked.size() == round; }));
            job->job->cancel();
            const std::optional<SendOutcome> sent = ended(*job);
            QVERIFY(sent);
            QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Cancelled));
            const std::optional<ReceiveOutcome> got = outcome(round - 1);
            QVERIFY(got);
            QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::SenderCancelled));
        }
        const Prepared p = prepare({meta("f1", "x.txt", 1)});
        QCOMPARE(p.status, 429);
        QVERIFY(p.body.find("Too many requests") != std::string::npos);
        const std::lock_guard lock(m_mutex);
        QCOMPARE(m_asked.size(), std::size_t(2)); // nobody asked the third time
    }

    void cancelWhileAskedForPin()
    {
        QVERIFY(startReceiver("4321"));
        const std::wstring source = native(m_dir.path() + u"/src-pincancel"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        for (int round = 0; round < 5; ++round) {
            auto job = send(what);
            QVERIFY(viewIs(*job, SendView::State::NeedsPin));
            const auto started = std::chrono::steady_clock::now();
            job->job->cancel(); // never lost, however close to the wait it comes
            const std::optional<SendOutcome> sent = ended(*job, 5000);
            QVERIFY(sent);
            QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::Cancelled));
            QVERIFY(std::chrono::steady_clock::now() - started < 3s);
        }
    }

    void cancelledFolderGoes()
    {
        QVERIFY(startReceiver());
        m_answer = true;
        constexpr std::uint64_t size = 4 << 20;
        const Prepared p = prepare({meta("f1", "相册/深/big.bin", size)});
        QCOMPARE(p.status, 200);
        Raw c(m_port, *m_clientCredentials);
        QVERIFY(c.send(uploadHead(p.sessionId, "f1", p.tokens.at("f1"), size)));
        QVERIFY(c.send(randomBytes(1 << 20)));
        QVERIFY(QTest::qWaitFor([&] {
            const std::optional<ReceiveProgress> progress = m_receiver->progress();
            return progress && progress->receivedBytes >= (1u << 20);
        }, 10000));
        // The partial file straight in the save folder (the next start finds one left there); its folder made.
        QCOMPARE(filesUnder(m_folder), QStringList {u"big.bin.wspart"_s});
        QVERIFY(std::filesystem::is_directory(std::filesystem::path(m_folder + L"\\相册\\深")));
        QVERIFY(m_receiver->cancel());
        QVERIFY(c.reply() != 200);
        // Nothing of it stays: the folder it made goes too.
        QVERIFY(QTest::qWaitFor([&] { return !std::filesystem::exists(std::filesystem::path(m_folder + L"\\相册")); }, 5000));
        QVERIFY(filesUnder(m_folder).isEmpty());
    }

    void shuttingDown()
    {
        // The receiver: the transfer cancelled, its partial file gone before shutDown() returns.
        QVERIFY(startReceiver());
        m_answer = true;
        constexpr std::uint64_t size = 8 << 20;
        const Prepared p = prepare({meta("f1", "big.bin", size)});
        QCOMPARE(p.status, 200);
        Raw c(m_port, *m_clientCredentials);
        QVERIFY(c.send(uploadHead(p.sessionId, "f1", p.tokens.at("f1"), size)));
        QVERIFY(c.send(randomBytes(1 << 20)));
        QVERIFY(QTest::qWaitFor([&] {
            const std::optional<ReceiveProgress> progress = m_receiver->progress();
            return progress && progress->receivedBytes >= (1u << 20);
        }, 10000));
        auto started = std::chrono::steady_clock::now();
        m_receiver->shutDown(500ms);
        QVERIFY(std::chrono::steady_clock::now() - started < 1500ms);
        QVERIFY(filesUnder(m_folder).isEmpty());
        std::optional<ReceiveOutcome> got = outcome(0);
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::Cancelled));
        QCOMPARE(m_receiver->listen({{"127.0.0.1", INADDR_LOOPBACK, 8}}, kPort), 0); // nothing taken any more
        c.close();

        // A send waiting for an answer: the receiver hears it end, within the time given.
        QVERIFY(startReceiver());
        {
            const std::lock_guard lock(m_mutex);
            m_answer.reset();
            m_asked.clear();
        }
        const std::wstring source = native(m_dir.path() + u"/src-shutdown"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        auto job = send(what);
        QVERIFY(waitFor([&] { return m_asked.size() == 1; }));
        started = std::chrono::steady_clock::now();
        job->job->shutDown(700ms);
        QVERIFY(std::chrono::steady_clock::now() - started < 1500ms);
        QVERIFY(job->job->finished());
        got = outcome(1);
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::SenderCancelled));
    }

    void portReturn()
    {
        // LocalSend on this PC holds the port: the next one. Once it lets go,
        // back to it, but only between transfers (a device in the middle of
        // one knows us by the port we have).
        SOCKET hold = INVALID_SOCKET;
        int port = 0;
        for (int p = kPort; p < kLastPort && hold == INVALID_SOCKET; ++p) {
            hold = holdPort(p);
            port = p;
        }
        if (hold == INVALID_SOCKET)
            QSKIP("none of the ports 53317 to 53319 could be held for the test");
        const auto release = qScopeGuard([&] {
            if (hold != INVALID_SOCKET)
                ::closesocket(hold);
        });
        QVERIFY(startReceiver({}, port));
        QVERIFY2(m_port > port, std::to_string(m_port).c_str());
        const int later = m_port;
        const std::vector<ListenAddress> loopback {{"127.0.0.1", INADDR_LOOPBACK, 8}};
        QCOMPARE(m_receiver->listen(loopback, port, true), later); // still held
        ::closesocket(hold);
        hold = INVALID_SOCKET;
        QCOMPARE(m_receiver->listen(loopback, port, false), later); // free, but a send of ours goes on
        QVERIFY(!logsMention("free again"));

        // A request waiting for an answer here: kept.
        const std::wstring source = native(m_dir.path() + u"/src-port"_s);
        QVERIFY(writeFile(source + L"\\x.txt", "x"));
        Outgoing what;
        what.paths = {source + L"\\x.txt"};
        auto job = send(what);
        QVERIFY(waitFor([&] { return m_asked.size() == 1; }));
        QCOMPARE(m_receiver->listen(loopback, port, true), later);
        job->job->cancel();
        QVERIFY(ended(*job));
        std::optional<ReceiveOutcome> got = outcome(0);
        QVERIFY(got);
        QCOMPARE(static_cast<int>(got->kind), static_cast<int>(ReceiveOutcome::Kind::SenderCancelled));

        // Nothing under way: back.
        QCOMPARE(m_receiver->listen(loopback, port, true), port);
        QCOMPARE(m_receiver->port(), port);
        QVERIFY(logsMention("port " + std::to_string(port) + " free again: back to it"));
        QCOMPARE(m_receiver->config().self.port, port); // what peers are told
        m_port = port;
        Outgoing text;
        text.kind = Outgoing::Kind::Text;
        text.text = "又回来了";
        job = send(text);
        const std::optional<SendOutcome> sent = ended(*job);
        QVERIFY(sent);
        QCOMPARE(static_cast<int>(sent->kind), static_cast<int>(SendOutcome::Kind::TextSent));
        // And there it stays.
        QCOMPARE(m_receiver->listen(loopback, port, true), port);
    }
};

QTEST_GUILESS_MAIN(TestLocalSend)
#include "tst_localsend.moc"
