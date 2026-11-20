// winshun.sync (src/sync-plugin): ShunSync v1, the clipboard shared with Mac顺.
// First the shared test vectors (docs/shunsync-test-vectors.json), byte for
// byte: what passes them computes what Mac顺 computes. Then three engines
// on 127.0.0.1 (no beacons), as three devices: pairing, an invite, text,
// pictures and files to the others (one device pulling them over its sync
// connection, as when no file connection can be made), nothing echoed back,
// a device removed.

#include "Engine.h"
#include "Crypto.h"
#include "Protocol.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <mutex>

using namespace shunsync;

namespace {

Bytes hexBytes(const QJsonValue& v)
{
    const std::string text = v.toString().toStdString();
    const auto bytes = fromHex(text);
    if (!bytes)
        qFatal("not hex: %s", text.c_str());
    return *bytes;
}

std::string hexOf(const Bytes& b) { return hexLower(b); }

// What a device's clipboard was given, and the clipboard watcher: what is
// written there comes back as a copy at once, as on a real PC.
class RecordingSink : public ClipboardSink {
public:
    Engine* watcher = nullptr;

    bool putText(const std::string& text, const std::string& origin) override
    {
        {
            const std::lock_guard lock(m_mutex);
            m_texts.emplace_back(text, origin);
        }
        if (watcher)
            watcher->localText(text);
        return true;
    }
    bool putImage(const Bytes& png, const std::string&) override
    {
        {
            const std::lock_guard lock(m_mutex);
            m_images.push_back(png);
        }
        if (watcher)
            watcher->localImage(png, 2, 2);
        return true;
    }
    bool putFiles(const std::vector<std::wstring>& paths, const std::string&) override
    {
        {
            const std::lock_guard lock(m_mutex);
            m_files.push_back(paths);
        }
        if (watcher)
            watcher->localFiles(paths);
        return true;
    }
    std::size_t texts() const
    {
        const std::lock_guard lock(m_mutex);
        return m_texts.size();
    }
    std::pair<std::string, std::string> lastText() const
    {
        const std::lock_guard lock(m_mutex);
        return m_texts.empty() ? std::pair<std::string, std::string>() : m_texts.back();
    }
    std::size_t images() const
    {
        const std::lock_guard lock(m_mutex);
        return m_images.size();
    }
    Bytes lastImage() const
    {
        const std::lock_guard lock(m_mutex);
        return m_images.empty() ? Bytes() : m_images.back();
    }
    std::size_t files() const
    {
        const std::lock_guard lock(m_mutex);
        return m_files.size();
    }
    std::vector<std::wstring> lastFiles() const
    {
        const std::lock_guard lock(m_mutex);
        return m_files.empty() ? std::vector<std::wstring>() : m_files.back();
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::pair<std::string, std::string>> m_texts;
    std::vector<Bytes> m_images;
    std::vector<std::vector<std::wstring>> m_files;
};

struct Device {
    QTemporaryDir dir;
    RecordingSink sink;
    std::unique_ptr<Engine> engine;

    Device(const char* label, const char* name, const char* platform, bool directFiles = true,
        std::uint64_t prefetch = 20ull * 1024 * 1024, bool acceptPairing = true)
    {
        Options options;
        options.prefetchBytes = prefetch; // the plugin's default is 0 (asMacShun)
        options.acceptPairing = acceptPairing;
        options.deviceName = name;
        options.platform = platform;
        options.dataDir = QDir::toNativeSeparators(dir.path()).toStdWString();
        options.bindAddress = "127.0.0.1";
        options.preferredPort = 0;
        options.discovery = false;
        options.directFiles = directFiles;
        Events events;
        if (qEnvironmentVariableIsSet("SHUNSYNC_LOG")) {
            const std::string who = label; // ASCII: the test log is not UTF-8
            events.log = [who](const std::string& line) { qInfo("[%s] %s", who.c_str(), line.c_str()); };
        }
        engine = std::make_unique<Engine>(options, sink, events);
        sink.watcher = engine.get();
    }
    ~Device()
    {
        sink.watcher = nullptr;
        engine->stop();
    }
    std::string id() const { return engine->deviceId(); }
    std::string address() const { return "127.0.0.1:" + std::to_string(engine->port()); }
    bool onlineWith(const std::string& other) const
    {
        for (const MemberView& m : engine->snapshot().members) {
            if (m.deviceId == other)
                return m.state == PeerState::Online;
        }
        return false;
    }
    PairView pair(std::uint64_t id) const
    {
        for (const PairView& p : engine->snapshot().pairs) {
            if (p.id == id)
                return p;
        }
        return {};
    }
    std::optional<PairView> asking() const
    {
        for (const PairView& p : engine->snapshot().pairs) {
            if (p.incoming && p.stage == PairView::Stage::Confirm)
                return p;
        }
        return std::nullopt;
    }
};

QByteArray readAll(const std::wstring& path)
{
    QFile file(QString::fromStdWString(path));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

class TestShunSync : public QObject {
    Q_OBJECT

    QJsonObject m_vectors;

private slots:
    void initTestCase()
    {
        QFile file(QStringLiteral(SHUNSYNC_VECTORS));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        m_vectors = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(m_vectors.value(QStringLiteral("version")).toInt(), 1);
    }

    void vectors()
    {
        const QJsonObject in = m_vectors[QStringLiteral("inputs")].toObject();
        const QJsonObject expected = m_vectors[QStringLiteral("expected")].toObject();
        const Bytes groupKey = hexBytes(in[QStringLiteral("groupKeyHex")]);
        const Bytes clientPriv = hexBytes(in[QStringLiteral("clientEphPrivHex")]);
        const Bytes serverPriv = hexBytes(in[QStringLiteral("serverEphPrivHex")]);
        const Bytes clientPub = hexBytes(in[QStringLiteral("clientEphPubHex")]);
        const Bytes serverPub = hexBytes(in[QStringLiteral("serverEphPubHex")]);
        const Bytes clientNonce = hexBytes(in[QStringLiteral("clientNonceHex")]);
        const Bytes serverNonce = hexBytes(in[QStringLiteral("serverNonceHex")]);
        const std::string clientId = in[QStringLiteral("clientDeviceId")].toString().toStdString();
        const std::string serverId = in[QStringLiteral("serverDeviceId")].toString().toStdString();

        QCOMPARE(groupIdOf(groupKey), expected[QStringLiteral("groupId")].toString().toStdString());
        QCOMPARE(inviteCode(groupKey, "192.168.1.10:37219"), expected[QStringLiteral("inviteCode")].toString().toStdString());
        const auto invite = parseInvite(expected[QStringLiteral("inviteCode")].toString().toStdString());
        QVERIFY(invite);
        QVERIFY(invite->groupKey == groupKey);
        QCOMPARE(invite->hostPort, std::string("192.168.1.10:37219"));

        // RFC 7748's keys: the public halves, and the secret both ends reach.
        QCOMPARE(hexOf(x25519Public(clientPriv)), hexOf(clientPub));
        QCOMPARE(hexOf(x25519Public(serverPriv)), hexOf(serverPub));
        const Bytes shared = x25519(clientPriv, serverPub);
        QCOMPARE(hexOf(shared), expected[QStringLiteral("sharedSecretHex")].toString().toStdString());
        QCOMPARE(hexOf(x25519(serverPriv, clientPub)), hexOf(shared));

        const QJsonObject sync = expected[QStringLiteral("sync")].toObject();
        const Bytes syncTranscript = transcriptOf("sync", clientNonce, serverNonce, clientPub, serverPub, clientId, serverId);
        QCOMPARE(hexOf(syncTranscript), sync[QStringLiteral("transcriptHex")].toString().toStdString());
        const SyncKeys keys = syncKeys(shared, groupKey, syncTranscript);
        QCOMPARE(hexOf(keys.serverProof), sync[QStringLiteral("serverProofHex")].toString().toStdString());
        QCOMPARE(hexOf(keys.clientProof), sync[QStringLiteral("clientProofHex")].toString().toStdString());
        QCOMPARE(hexOf(keys.c2s), sync[QStringLiteral("c2sKeyHex")].toString().toStdString());
        QCOMPARE(hexOf(keys.s2c), sync[QStringLiteral("s2cKeyHex")].toString().toStdString());

        const QJsonObject pair = expected[QStringLiteral("pair")].toObject();
        const Bytes pairTranscript = transcriptOf("pair", clientNonce, serverNonce, clientPub, serverPub, clientId, serverId);
        QCOMPARE(hexOf(pairTranscript), pair[QStringLiteral("transcriptHex")].toString().toStdString());
        const PairKeys pk = pairKeys(shared, pairTranscript);
        QCOMPARE(pk.sas, pair[QStringLiteral("sasCode")].toString().toStdString());
        QCOMPARE(hexOf(pk.c2s), pair[QStringLiteral("c2sKeyHex")].toString().toStdString());
        QCOMPARE(hexOf(pk.s2c), pair[QStringLiteral("s2cKeyHex")].toString().toStdString());

        // The frames: sealed in order under the c2s key, and opened back the same way.
        FrameSealer sealer(keys.c2s);
        FrameOpener opener(keys.c2s);
        for (const QJsonValue& value : expected[QStringLiteral("frames")].toArray()) {
            const QJsonObject frame = value.toObject();
            QCOMPARE(frame[QStringLiteral("keyName")].toString(), QStringLiteral("sync.c2sKeyHex"));
            QCOMPARE(static_cast<std::uint64_t>(frame[QStringLiteral("seq")].toInt()), sealer.next());
            const Bytes plain = hexBytes(frame[QStringLiteral("plaintextHex")]);
            const Bytes sealed = sealer.seal(plain);
            QCOMPARE(hexOf(sealed), frame[QStringLiteral("ciphertextAndTagHex")].toString().toStdString());
            const auto opened = opener.open(sealed);
            QVERIFY(opened);
            QVERIFY(*opened == plain);
        }
    }

    // A frame out of order, changed or replayed does not open.
    void framesInOrderOnly()
    {
        const Bytes key = randomBytes(32);
        FrameSealer sealer(key);
        const Bytes first = sealer.seal("\x01{\"type\":\"ping\"}");
        const Bytes second = sealer.seal("\x01{\"type\":\"pong\"}");
        {
            FrameOpener opener(key);
            QVERIFY(!opener.open(second)); // seq 1 where 0 is due
            QVERIFY(opener.open(first));
            QVERIFY(!opener.open(first)); // replayed
            QVERIFY(opener.open(second));
        }
        {
            FrameOpener opener(key);
            Bytes changed = first;
            changed[3] = static_cast<char>(changed[3] ^ 1);
            QVERIFY(!opener.open(changed));
            QVERIFY(!FrameOpener(randomBytes(32)).open(first)); // another key
        }
    }

    void inviteForms()
    {
        const Bytes key = randomBytes(32);
        QVERIFY(parseInvite(base64(key))->groupKey == key); // bare, padded
        QVERIFY(parseInvite("  " + base64UrlNoPad(key) + "\n")->groupKey == key);
        QVERIFY(parseInvite(inviteCode(key))->hostPort.empty());
        const auto v6 = parseInvite(inviteCode(key, "[fe80::1]:40000"));
        QVERIFY(v6);
        QCOMPARE(v6->hostPort, std::string("[fe80::1]:40000"));
        QVERIFY(!parseInvite("shunsync://join?k=AAEC"));
        QVERIFY(!parseInvite("hello"));
        QVERIFY(!parseInvite(base64(randomBytes(16))));
        const auto badPlace = parseInvite(inviteCode(key, "a b:1"));
        QVERIFY(badPlace && badPlace->hostPort.empty());
    }

    void relativePaths()
    {
        QVERIFY(validRelativePath("项目资料"));
        QVERIFY(validRelativePath("项目资料/方案.pdf"));
        QVERIFY(!validRelativePath(""));
        QVERIFY(!validRelativePath("/etc/passwd"));
        QVERIFY(!validRelativePath("a/../b"));
        QVERIFY(!validRelativePath(".."));
        QVERIFY(!validRelativePath("a//b"));
        QVERIFY(!validRelativePath("a/"));
        QVERIFY(!validRelativePath("C:/x"));
        QVERIFY(!validRelativePath("a\\b"));
    }

    void hostPorts()
    {
        QCOMPARE(parseHostPort("100.64.0.2:37219")->port, std::uint16_t(37219));
        QCOMPARE(parseHostPort("mac.local", kTcpPort)->port, kTcpPort);
        QVERIFY(!parseHostPort("mac.local"));
        QCOMPARE(parseHostPort("[::1]:5")->host, std::string("::1"));
        QVERIFY(!parseHostPort("1.2.3.4:0"));
        QVERIFY(!parseHostPort("1.2.3.4:70000"));
        QVERIFY(!parseHostPort(":80"));
    }

    // As Mac顺 (its reference, 2026-11-20): pairing asked for is answered
    // without the 2-minute window, once per device in 8 s; a device of another
    // group is told "wrong_group" (Mac顺 then asks to pair instead); files copied
    // elsewhere wait to be received by hand, onto the clipboard or (存到下载) not.
    void asMacShun()
    {
        Device a("A", "书房 PC", "windows", true, 0);
        Device b("B", "MacBook", "macos", true, 0);
        Device c("C", "笔记本", "windows", true, 0);
        for (Device* d : {&a, &b, &c}) {
            std::string problem;
            QVERIFY2(d->engine->start(&problem), problem.c_str());
        }
        QVERIFY(a.engine->snapshot().acceptingPairing);
        QCOMPARE(a.engine->snapshot().pairingLeftMs, std::int64_t(0));

        // No window opened on A: B's request is answered, and declined there.
        const std::uint64_t asked = b.engine->pairWith(a.address());
        QTRY_VERIFY_WITH_TIMEOUT(a.asking().has_value(), 10000);
        QCOMPARE(a.asking()->sas, b.pair(asked).sas);
        a.engine->answerPair(a.asking()->id, false);
        QTRY_VERIFY_WITH_TIMEOUT(b.pair(asked).stage == PairView::Stage::Failed, 10000);
        // Asked again at once: too soon.
        const std::uint64_t again = b.engine->pairWith(a.address());
        QTRY_VERIFY_WITH_TIMEOUT(b.pair(again).stage == PairView::Stage::Failed, 10000);
        QCOMPARE(b.pair(again).problem, std::string("pairing_rate_limited"));

        // A device of another group dialing A to sync: "wrong_group".
        WSADATA wsa;
        QCOMPARE(::WSAStartup(MAKEWORD(2, 2), &wsa), 0);
        const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        QVERIFY(s != INVALID_SOCKET);
        sockaddr_in to {};
        to.sin_family = AF_INET;
        to.sin_port = ::htons(a.engine->port());
        to.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        QCOMPARE(::connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof to), 0);
        QJsonObject helloJson;
        helloJson.insert(QStringLiteral("type"), QStringLiteral("hello"));
        helloJson.insert(QStringLiteral("proto"), 1);
        helloJson.insert(QStringLiteral("mode"), QStringLiteral("sync"));
        helloJson.insert(QStringLiteral("deviceId"), QStringLiteral("6f1c2a4e-2b7d-4c1e-9a3f-1d2e3f4a5b6c"));
        helloJson.insert(QStringLiteral("deviceName"), QStringLiteral("外人"));
        helloJson.insert(QStringLiteral("platform"), QStringLiteral("macos"));
        helloJson.insert(QStringLiteral("groupId"), QString::fromStdString(groupIdOf(randomBytes(32))));
        helloJson.insert(QStringLiteral("ephPub"), QString::fromStdString(base64(randomBytes(32))));
        helloJson.insert(QStringLiteral("nonce"), QString::fromStdString(base64(randomBytes(16))));
        helloJson.insert(QStringLiteral("listenPort"), 1);
        const QByteArray hello = QJsonDocument(helloJson).toJson(QJsonDocument::Compact);
        QByteArray framed(4, '\0');
        qToBigEndian<quint32>(static_cast<quint32>(hello.size()), framed.data());
        framed += hello;
        QCOMPARE(::send(s, framed.constData(), static_cast<int>(framed.size()), 0), static_cast<int>(framed.size()));
        QByteArray answer;
        char buffer[4096];
        for (int got = 0; (got = ::recv(s, buffer, sizeof buffer, 0)) > 0;)
            answer.append(buffer, got);
        ::closesocket(s);
        ::WSACleanup();
        QVERIFY(answer.size() > 4);
        const QJsonObject ack = QJsonDocument::fromJson(answer.mid(4)).object();
        QCOMPARE(ack.value(QStringLiteral("type")).toString(), QStringLiteral("hello_ack"));
        QCOMPARE(ack.value(QStringLiteral("accepted")).toBool(true), false);
        QCOMPARE(ack.value(QStringLiteral("error")).toString(), QStringLiteral("wrong_group"));

        // Files: offered, not pulled; pulled without the clipboard (存到下载), then onto it.
        QVERIFY(c.engine->joinWithInvite(a.engine->invite()));
        QTRY_VERIFY_WITH_TIMEOUT(c.onlineWith(a.id()) && a.onlineWith(c.id()), 15000);
        QTemporaryDir source;
        QVERIFY(writeFile(source.filePath(QStringLiteral("小.txt")), "small"));
        a.engine->localFiles({QDir::toNativeSeparators(source.filePath(QStringLiteral("小.txt"))).toStdWString()});
        QTRY_COMPARE_WITH_TIMEOUT(c.engine->snapshot().files.size(), std::size_t(1), 10000);
        const FilesView offered = c.engine->snapshot().files.front();
        QVERIFY(offered.stage == FilesView::Stage::Offered);
        QTest::qWait(500);
        QCOMPARE(c.sink.files(), std::size_t(0));
        c.engine->pullFiles(offered.transferId, false);
        QTRY_VERIFY_WITH_TIMEOUT(c.engine->snapshot().files.front().stage == FilesView::Stage::Ready, 10000);
        QTest::qWait(300);
        QCOMPARE(c.sink.files(), std::size_t(0));
        c.engine->pullFiles(offered.transferId);
        QTRY_COMPARE_WITH_TIMEOUT(c.sink.files(), std::size_t(1), 10000);
        QCOMPARE(readAll(c.sink.lastFiles().front()), QByteArray("small"));
    }

    void threeDevices()
    {
        Device a("A", "书房 PC", "windows", true, 20ull * 1024 * 1024, false); // only in its window
        Device b("B", "MacBook", "macos");
        Device c("C", "笔记本", "windows", false); // its pulls as when A is behind NAT: over the sync link
        for (Device* d : {&a, &b, &c}) {
            std::string problem;
            QVERIFY2(d->engine->start(&problem), problem.c_str());
        }

        // A has not opened its window: B is turned away.
        const std::uint64_t refused = b.engine->pairWith(a.address());
        QVERIFY(refused);
        QTRY_VERIFY_WITH_TIMEOUT(b.pair(refused).stage == PairView::Stage::Failed, 10000);
        QCOMPARE(b.pair(refused).problem, std::string("pairing_closed"));

        // B asks to join A: both show the same six digits, A agrees.
        a.engine->openPairing();
        const std::uint64_t asked = b.engine->pairWith(a.address());
        QTRY_VERIFY_WITH_TIMEOUT(b.pair(asked).stage == PairView::Stage::Confirm, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(a.asking().has_value(), 10000);
        const PairView incoming = *a.asking();
        QCOMPARE(incoming.sas.size(), std::size_t(6));
        QCOMPARE(incoming.sas, b.pair(asked).sas);
        QCOMPARE(incoming.name, std::string("MacBook"));
        a.engine->answerPair(incoming.id, true);
        QTRY_VERIFY_WITH_TIMEOUT(b.pair(asked).stage == PairView::Stage::Done, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(a.onlineWith(b.id()) && b.onlineWith(a.id()), 15000);
        QCOMPARE(a.engine->snapshot().groupId, b.engine->snapshot().groupId);
        QCOMPARE(a.engine->snapshot().pairingLeftMs, 0); // one device let in: the window closed

        // C joins with A's invite and finds B by what A tells it.
        QVERIFY(c.engine->joinWithInvite(a.engine->invite()));
        QTRY_VERIFY_WITH_TIMEOUT(c.onlineWith(a.id()) && c.onlineWith(b.id()) && b.onlineWith(c.id()), 20000);
        QCOMPARE(c.engine->snapshot().members.size(), std::size_t(2));

        // Text to all: once each, '\n' line ends; the write that comes back as
        // a copy on B and C is not sent again.
        a.engine->localText("第一行\r\n第二行");
        QTRY_VERIFY_WITH_TIMEOUT(b.sink.texts() == 1 && c.sink.texts() == 1, 10000);
        QCOMPARE(b.sink.lastText().first, std::string("第一行\n第二行"));
        QCOMPARE(b.sink.lastText().second, a.id());
        QTest::qWait(600);
        QCOMPARE(a.sink.texts(), std::size_t(0));
        QCOMPARE(b.sink.texts(), std::size_t(1));
        QCOMPARE(c.sink.texts(), std::size_t(1));

        // To one member only.
        b.engine->localText("只给笔记本", c.id());
        QTRY_COMPARE_WITH_TIMEOUT(c.sink.texts(), std::size_t(2), 10000);
        QCOMPARE(c.sink.lastText().first, std::string("只给笔记本"));
        QTest::qWait(400);
        QCOMPARE(a.sink.texts(), std::size_t(0));

        // A picture.
        Bytes png("\x89PNG\r\n\x1a\n", 8);
        png += randomBytes(50'000);
        c.engine->localImage(png, 2, 2);
        QTRY_VERIFY_WITH_TIMEOUT(a.sink.images() == 1 && b.sink.images() == 1, 10000);
        QVERIFY(a.sink.lastImage() == png);
        QTest::qWait(400);
        QCOMPARE(c.sink.images(), std::size_t(0));

        // A folder (an empty one in it, a file of three chunks) and a file.
        QTemporaryDir source;
        const QString folder = source.filePath(QStringLiteral("项目"));
        const QByteArray big = QByteArray::fromStdString(randomBytes(700 * 1024));
        QVERIFY(writeFile(folder + QStringLiteral("/说明.txt"), "hello"));
        QVERIFY(writeFile(folder + QStringLiteral("/sub/big.bin"), big));
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/空文件夹")));
        QVERIFY(writeFile(source.filePath(QStringLiteral("单个.txt")), QStringLiteral("单个").toUtf8()));
        a.engine->localFiles({QDir::toNativeSeparators(folder).toStdWString(),
            QDir::toNativeSeparators(source.filePath(QStringLiteral("单个.txt"))).toStdWString()});
        QTRY_VERIFY_WITH_TIMEOUT(b.sink.files() == 1 && c.sink.files() == 1, 20000);
        for (Device* d : {&b, &c}) {
            const std::vector<std::wstring> got = d->sink.lastFiles();
            QCOMPARE(got.size(), std::size_t(2));
            QCOMPARE(QFileInfo(QString::fromStdWString(got[0])).fileName(), QStringLiteral("项目"));
            QCOMPARE(readAll(got[0] + L"\\sub\\big.bin"), big);
            QCOMPARE(readAll(got[0] + L"\\说明.txt"), QByteArray("hello"));
            QVERIFY(QFileInfo(QString::fromStdWString(got[0] + L"\\空文件夹")).isDir());
            QCOMPARE(readAll(got[1]), QStringLiteral("单个").toUtf8());
            QVERIFY(QString::fromStdWString(got[0]).startsWith(QDir::toNativeSeparators(d->dir.path()), Qt::CaseInsensitive));
        }
        QTest::qWait(600);
        QCOMPARE(a.sink.files(), std::size_t(0)); // nothing sent back

        // C removed: A and B move to a new key, C is left on its own.
        const std::string oldGroup = a.engine->snapshot().groupId;
        a.engine->removeDevice(c.id());
        QTRY_VERIFY_WITH_TIMEOUT(a.engine->snapshot().groupId != oldGroup
                && b.engine->snapshot().groupId == a.engine->snapshot().groupId && a.onlineWith(b.id()) && b.onlineWith(a.id()),
            20000);
        QTRY_VERIFY_WITH_TIMEOUT(!c.onlineWith(a.id()) && !c.onlineWith(b.id()), 10000);
        const std::size_t cBefore = c.sink.texts();
        const std::size_t bBefore = b.sink.texts();
        a.engine->localText("C 收不到");
        QTRY_COMPARE_WITH_TIMEOUT(b.sink.texts(), bBefore + 1, 10000);
        QTest::qWait(3000); // C keeps trying the old addresses: turned away
        QCOMPARE(c.sink.texts(), cBefore);
        QVERIFY(!c.onlineWith(a.id()) && !c.onlineWith(b.id()));
        for (const MemberView& m : a.engine->snapshot().members)
            QVERIFY(m.deviceId != c.id());
    }
};

QTEST_GUILESS_MAIN(TestShunSync)
#include "tst_shunsync.moc"
