// The in-app update (Updater): what the update service's answer gives,
// where packages are downloaded from and how they are checked, unpacking a
// portable zip safely, and swapping a portable copy's files with "data"
// kept and a failure rolled back.

#include "Release.h"
#include "UpdatePackage.h"
#include "platform/Http.h"
#include "Swap.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <miniz.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <map>
#include <thread>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

QByteArray sha256(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// A blocking HTTP/1.0 server on 127.0.0.1 for the downloads: each path its
// status and body, one request per connection.
class FakeServer {
public:
    struct Route {
        int status = 200;
        QByteArray body;
    };

    explicit FakeServer(std::map<QByteArray, Route> routes)
        : m_routes(std::move(routes))
    {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
        m_listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        int length = sizeof(address);
        ::getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &length);
        m_port = ntohs(address.sin_port);
        ::listen(m_listener, 8);
        m_thread = std::thread([this] { serve(); });
    }
    ~FakeServer()
    {
        m_stop = true;
        ::closesocket(m_listener);
        m_thread.join();
        ::WSACleanup();
    }

    QString url(const char* path) const { return u"http://127.0.0.1:%1%2"_s.arg(m_port).arg(QLatin1StringView(path)); }

private:
    void serve()
    {
        while (!m_stop) {
            const SOCKET client = ::accept(m_listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                return;
            QByteArray request;
            char buffer[4096];
            while (!request.contains("\r\n\r\n")) {
                const int got = ::recv(client, buffer, sizeof(buffer), 0);
                if (got <= 0)
                    break;
                request.append(buffer, got);
            }
            const QByteArray path = request.split(' ').value(1);
            const auto it = m_routes.find(path);
            const Route route = it != m_routes.end() ? it->second : Route {404, "not found"};
            QByteArray answer = "HTTP/1.0 " + QByteArray::number(route.status) + " X\r\nContent-Length: "
                + QByteArray::number(route.body.size()) + "\r\nConnection: close\r\n\r\n" + route.body;
            ::send(client, answer.constData(), static_cast<int>(answer.size()), 0);
            ::shutdown(client, SD_SEND);
            ::closesocket(client);
        }
    }

    std::map<QByteArray, Route> m_routes;
    SOCKET m_listener = INVALID_SOCKET;
    int m_port = 0;
    std::atomic<bool> m_stop {false};
    std::thread m_thread;
};

// A zip with these entries (name -> content; a name ending in '/' is a folder).
QByteArray makeZip(const QList<std::pair<QByteArray, QByteArray>>& entries)
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};
    // miniz refuses to write the names a hostile zip would have ('\', a
    // leading '/', a drive): written under a stand-in of the same length,
    // which is then replaced in the headers.
    QList<std::pair<QByteArray, QByteArray>> renames;
    for (const auto& [name, content] : entries) {
        QByteArray written = name;
        if (name.contains('\\') || name.startsWith('/') || name.contains(':')) {
            written = QByteArray(name.size(), static_cast<char>('j' + renames.size()));
            renames.append({written, name});
        }
        if (!mz_zip_writer_add_mem(&zip, written.constData(), content.constData(), static_cast<size_t>(content.size()),
                MZ_DEFAULT_LEVEL))
            return {};
    }
    void* data = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &data, &size);
    QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(size));
    mz_zip_writer_end(&zip);
    for (const auto& [from, to] : renames)
        bytes.replace(from, to);
    return bytes;
}

// Every central directory entry's external attributes set to `attributes`.
void setExternalAttributes(QByteArray& zip, quint32 attributes)
{
    for (qsizetype at = zip.indexOf("PK\x01\x02"); at >= 0; at = zip.indexOf("PK\x01\x02", at + 4))
        qToLittleEndian(attributes, zip.data() + at + 38);
}

void writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

// The service's answer for the Setup, as lcstp.com sends it (a mirror of
// plain http and one of ftp added, which are never used).
const QByteArray kFeed = QJsonDocument(QJsonObject {
    {u"tag_name"_s, u"v0.15.0"_s},
    {u"html_url"_s, u"https://lcstp.com/projects/winshun"_s},
    {u"body"_s, u"notes"_s},
    {u"assets"_s, QJsonArray {QJsonObject {
        {u"name"_s, u"WinShun-0.15.0-x64-setup.exe"_s},
        {u"browser_download_url"_s, u"https://github.com/LingCore/WinShun/releases/download/v0.15.0/WinShun-0.15.0-x64-setup.exe"_s},
        {u"mirrors"_s, QJsonArray {u"https://gh-proxy.com/https://github.com/x/WinShun-0.15.0-x64-setup.exe"_s,
            u"http://plain.example/WinShun-0.15.0-x64-setup.exe"_s, u"ftp://old.example/WinShun-0.15.0-x64-setup.exe"_s,
            u"https://ghfast.top/https://github.com/x/WinShun-0.15.0-x64-setup.exe"_s}},
        {u"size"_s, 42989976},
        {u"digest"_s, u"sha256:44F752C0ED60CB820626705842B6D347F844DAB2DB64AEC246357B8414A78915"_s},
    }}},
}).toJson();

} // namespace

class TestUpdater : public QObject {
    Q_OBJECT

private slots:
    void parsesMirrorsAndDigest()
    {
        const auto info = release::parse(kFeed);
        QVERIFY(info);
        QCOMPARE(info->version, u"0.15.0"_s);
        QCOMPARE(info->assets.size(), 1);
        const release::Asset& a = info->assets.first();
        QCOMPARE(a.size, 42989976);
        QCOMPARE(a.sha256, u"44f752c0ed60cb820626705842b6d347f844dab2db64aec246357b8414a78915"_s); // lowercased
        QCOMPARE(a.mirrors.size(), 4);
        QVERIFY(release::packageAsset(*info, release::Package::Setup));
        QVERIFY(!release::packageAsset(*info, release::Package::Portable)); // not the package asked for
    }

    void rejectsAssetsWithoutDigest()
    {
        for (const char* digest : {"", "md5:0123", "sha256:abc", "sha256:44f752c0ed60cb820626705842b6d347f844dab2db64aec246357b8414a7891Z"}) {
            QByteArray json = kFeed;
            json.replace("sha256:44F752C0ED60CB820626705842B6D347F844DAB2DB64AEC246357B8414A78915", digest);
            const auto info = release::parse(json);
            QVERIFY(info);
            QVERIFY2(info->assets.first().sha256.isEmpty(), digest);
            QVERIFY2(!release::packageAsset(*info, release::Package::Setup), digest);
        }
        QByteArray noDigest = kFeed;
        noDigest.replace("\"digest\"", "\"other\"");
        QVERIFY(!release::packageAsset(*release::parse(noDigest), release::Package::Setup));
        // Two assets, or none: not the one package the service sends.
        auto info = *release::parse(kFeed);
        info.assets.append(info.assets.first());
        QVERIFY(!release::packageAsset(info, release::Package::Setup));
        info.assets.clear();
        QVERIFY(!release::packageAsset(info, release::Package::Setup));
    }

    void portablePackageName()
    {
        release::Info info;
        info.version = u"0.15.0"_s;
        info.assets.append({u"WinShun-0.15.0-x64-portable.zip"_s, u"https://github.com/a.zip"_s, {}, 57752971,
            QString(64, u'a')});
        QVERIFY(release::packageAsset(info, release::Package::Portable));
        QVERIFY(!release::packageAsset(info, release::Package::Setup));
        info.assets.first().name = u"WinShun-0.15.0-x64-portable.zip.exe"_s;
        QVERIFY(!release::packageAsset(info, release::Package::Portable));
    }

    void urlOrderAndHttpsOnly()
    {
        const release::Asset a = release::parse(kFeed)->assets.first();
        const QStringList urls = release::downloadUrls(a);
        QCOMPARE(urls, (QStringList {a.mirrors[0], a.mirrors[3], a.url})); // mirrors in order, GitHub last
        // Plain http only for a fake service on this computer, and only when allowed.
        release::Asset local = a;
        local.mirrors = {u"http://127.0.0.1:8000/a"_s, u"http://localhost/b"_s, u"http://10.0.0.1/c"_s};
        QCOMPARE(release::downloadUrls(local), QStringList {a.url});
        QCOMPARE(release::downloadUrls(local, true),
            (QStringList {u"http://127.0.0.1:8000/a"_s, u"http://localhost/b"_s, a.url}));
        local.mirrors = {a.url};
        QCOMPARE(release::downloadUrls(local), QStringList {a.url}); // no source twice
    }

    void noContentIsUpToDate()
    {
        std::optional<release::Info> newer;
        QCOMPARE(release::judge(204, {}, u"0.15.0"_s, &newer), release::Answer::UpToDate);
        QVERIFY(!newer);
        QCOMPARE(release::judge(200, kFeed, u"0.14.2"_s, &newer), release::Answer::Newer);
        QVERIFY(newer);
        QCOMPARE(release::judge(200, kFeed, u"0.15.0"_s, &newer), release::Answer::UpToDate);
        QVERIFY(!newer);
        QCOMPARE(release::judge(200, "not json", u"0.14.0"_s, &newer), release::Answer::Invalid);
        QCOMPARE(release::judge(400, "{}", u"0.14.0"_s, &newer), release::Answer::Invalid);
        QCOMPARE(release::judge(0, {}, u"0.14.0"_s, &newer), release::Answer::Invalid);
    }

    void releasePageOnlyOnLcstp()
    {
        const QString fixed = QString::fromLatin1(release::kProjectPage);
        QCOMPARE(release::releasePage(u"https://lcstp.com/projects/winshun/v0.15.0"_s), u"https://lcstp.com/projects/winshun/v0.15.0"_s);
        for (const QString& bad : {u""_s, u"http://lcstp.com/projects/winshun"_s, u"https://github.com/LingCore/WinShun/releases"_s,
                 u"https://lcstp.com.evil.example/"_s, u"https://lcstp.com@evil.example/"_s, u"javascript:alert(1)"_s,
                 u"https://evil.example/https://lcstp.com/"_s})
            QCOMPARE(release::releasePage(bad), fixed);
    }

    void versionResource()
    {
        QVERIFY(release::sameVersion(u"0.15.0.0"_s, u"v0.15.0"_s));
        QVERIFY(!release::sameVersion(u"0.15.1.0"_s, u"0.15.0"_s));
        QVERIFY(!release::sameVersion(QString(), u"0.15.0"_s)); // no version resource: refused
        // The helper is built with this version's resource.
        const QString version = update::fileVersion(QStringLiteral(WINSHUN_UPDATE_EXE));
        QVERIFY(release::sameVersion(version, QStringLiteral(WINSHUN_VERSION)));
        QVERIFY(!release::sameVersion(version, u"0.0.1"_s));
        QVERIFY(update::fileVersion(QCoreApplication::applicationFilePath()).isEmpty());
    }

    void downloadFallsThroughToGoodSource()
    {
        const QByteArray good = QByteArray(300 * 1024, 'w') + "end";
        QByteArray tampered = good;
        tampered[1000] = 'x';
        FakeServer server({{"/tampered", {200, tampered}}, {"/short", {200, good.left(100)}}, {"/good", {200, good}}});
        release::Asset asset;
        asset.size = good.size();
        asset.sha256 = QString::fromLatin1(sha256(good));
        QTemporaryDir dir;
        const QString target = dir.filePath(u"WinShun-0.15.0-x64-setup.exe"_s);
        const QStringList urls {server.url("/missing"), server.url("/tampered"), server.url("/short"), server.url("/good")};
        qint64 heard = 0;
        const update::DownloadResult result = update::download(asset, urls, target, [&](qint64 received, qint64) {
            heard = received;
            return true;
        });
        QVERIFY2(result.ok, qPrintable(result.failures.join(u'\n')));
        QCOMPARE(result.failures.size(), 3);
        QVERIFY(result.failures[0].contains(u"HTTP 404"_s));
        QVERIFY(result.failures[1].contains(u"SHA-256"_s));
        QVERIFY(result.failures[2].contains(u"bytes instead of"_s));
        QCOMPARE(readFile(target), good);
        QCOMPARE(heard, good.size());
        QVERIFY(!QFile::exists(target + u".partial"_s));
    }

    void downloadFailsWhenNoSourceIsRight()
    {
        const QByteArray good("the right bytes");
        FakeServer server({{"/tampered", {200, "the wrong bytes"}}, {"/error", {500, "oops"}}});
        release::Asset asset;
        asset.size = good.size();
        asset.sha256 = QString::fromLatin1(sha256(good));
        QTemporaryDir dir;
        const QString target = dir.filePath(u"pkg.zip"_s);
        const update::DownloadResult result = update::download(asset,
            {server.url("/tampered"), server.url("/error"), server.url("/missing")}, target, {});
        QVERIFY(!result.ok);
        QVERIFY(!result.cancelled);
        QCOMPARE(result.failures.size(), 3);
        QVERIFY(!QFile::exists(target));
        QVERIFY(!QFile::exists(target + u".partial"_s));
        QVERIFY(QDir(dir.path()).isEmpty());
        // No digest: nothing is downloaded.
        asset.sha256.clear();
        QVERIFY(!update::download(asset, {server.url("/tampered")}, target, {}).ok);
        QVERIFY(QDir(dir.path()).isEmpty());
    }

    void downloadCancels()
    {
        const QByteArray good(2 * 1024 * 1024, 'c');
        FakeServer server({{"/good", {200, good}}});
        release::Asset asset;
        asset.size = good.size();
        asset.sha256 = QString::fromLatin1(sha256(good));
        QTemporaryDir dir;
        const QString target = dir.filePath(u"pkg.zip"_s);
        const update::DownloadResult result = update::download(asset, {server.url("/good")}, target,
            [](qint64 received, qint64) { return received < 300 * 1024; });
        QVERIFY(!result.ok);
        QVERIFY(result.cancelled);
        QVERIFY(QDir(dir.path()).isEmpty());
    }

    void heldFileRefusesChanges()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"setup.exe"_s);
        writeFile(path, "package bytes");
        update::HeldFile held;
        QVERIFY(held.open(path));
        QByteArray bytes;
        QCOMPARE(held.sha256(&bytes), sha256("package bytes"));
        QCOMPARE(bytes, QByteArray("package bytes"));
        QFile other(path);
        QVERIFY(!other.open(QIODevice::WriteOnly | QIODevice::Append)); // writing refused
        QVERIFY(!QFile::remove(path)); // and deleting
        QVERIFY(QFile(path).open(QIODevice::ReadOnly)); // reading not
        held.close();
        QVERIFY(QFile::remove(path));
    }

    void unpacksPortableZip()
    {
        QTemporaryDir dir;
        const QString stage = dir.filePath(u"stage"_s);
        const QByteArray zip = makeZip({{"WinShun/", {}}, {"WinShun/WinShun.exe", "new exe"},
            {"WinShun/qml/", {}}, {"WinShun/qml/Main.qml", "Item {}"}, {"WinShun/data/README.txt", "theirs"},
            {"WinShun/data/WinShun.ini", "should not come"}});
        QCOMPARE(update::unpackPortable(zip, stage), QString());
        QCOMPARE(readFile(stage + u"/WinShun.exe"_s), QByteArray("new exe"));
        QCOMPARE(readFile(stage + u"/qml/Main.qml"_s), QByteArray("Item {}"));
        QVERIFY(!QFileInfo::exists(stage + u"/data"_s)); // the zip's data is ignored
        // Files at the root unpack as they are.
        const QString flat = dir.filePath(u"flat"_s);
        QCOMPARE(update::unpackPortable(makeZip({{"WinShun.exe", "x"}, {"a.dll", "y"}}), flat), QString());
        QVERIFY(QFileInfo::exists(flat + u"/a.dll"_s));
        // No program in it: refused.
        QVERIFY(!update::unpackPortable(makeZip({{"WinShun/other.exe", "x"}}), dir.filePath(u"none"_s)).isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath(u"none"_s)));
    }

    void refusesPathTraversal_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::newRow("dotdot") << QByteArray("WinShun/../evil.txt");
        QTest::newRow("dotdot first") << QByteArray("../evil.txt");
        QTest::newRow("backslash dotdot") << QByteArray("WinShun\\..\\..\\evil.txt");
        QTest::newRow("absolute") << QByteArray("/Windows/evil.txt");
        QTest::newRow("absolute backslash") << QByteArray("\\evil.txt");
        QTest::newRow("drive") << QByteArray("C:/evil.txt");
        QTest::newRow("drive relative") << QByteArray("C:evil.txt");
        QTest::newRow("stream") << QByteArray("WinShun/WinShun.exe:evil");
        QTest::newRow("unc") << QByteArray("//server/share/evil.txt");
        QTest::newRow("device") << QByteArray("WinShun/CON.txt");
        QTest::newRow("trailing dot") << QByteArray("WinShun/qml./a.txt");
        QTest::newRow("trailing space") << QByteArray("WinShun/a.txt ");
    }
    void refusesPathTraversal()
    {
        QFETCH(QByteArray, name);
        QTemporaryDir dir;
        const QString stage = dir.filePath(u"stage"_s);
        const QByteArray zip = makeZip({{"WinShun/WinShun.exe", "x"}, {name, "evil"}});
        QVERIFY(!zip.isEmpty());
        QVERIFY(!update::unpackPortable(zip, stage).isEmpty());
        QVERIFY(!QFileInfo::exists(stage));
        QVERIFY(!QFileInfo::exists(dir.filePath(u"evil.txt"_s)));
    }

    void refusesLinksAndDuplicates()
    {
        QTemporaryDir dir;
        QByteArray link = makeZip({{"WinShun/WinShun.exe", "x"}});
        setExternalAttributes(link, 0xA1FFu << 16); // Unix symbolic link
        QVERIFY(!update::unpackPortable(link, dir.filePath(u"a"_s)).isEmpty());
        QByteArray reparse = makeZip({{"WinShun/WinShun.exe", "x"}});
        setExternalAttributes(reparse, FILE_ATTRIBUTE_REPARSE_POINT);
        QVERIFY(!update::unpackPortable(reparse, dir.filePath(u"b"_s)).isEmpty());
        QVERIFY(!update::unpackPortable(makeZip({{"WinShun/WinShun.exe", "x"}, {"WinShun/winshun.EXE", "y"}}),
            dir.filePath(u"c"_s)).isEmpty());
        QVERIFY(!update::unpackPortable("not a zip", dir.filePath(u"d"_s)).isEmpty());
        QVERIFY(QDir(dir.path()).isEmpty());
    }

    // A portable copy's folder: old files, the user's data, one file of
    // their own; the stage: the new version.
    void makeTree(const QString& root)
    {
        writeFile(root + u"/app/WinShun.exe"_s, "old exe");
        writeFile(root + u"/app/qml/Old.qml"_s, "old qml");
        writeFile(root + u"/app/Qt6Core.dll"_s, "old core");
        writeFile(root + u"/app/data/WinShun/WinShun.ini"_s, "settings");
        writeFile(root + u"/app/data/index.bin"_s, "index");
        writeFile(root + u"/app/mine.txt"_s, "the user's");
        writeFile(root + u"/app/.stage/WinShun.exe"_s, "new exe");
        writeFile(root + u"/app/.stage/qml/New.qml"_s, "new qml");
        writeFile(root + u"/app/.stage/Qt6Core.dll"_s, "new core");
        writeFile(root + u"/app/.stage/Added.dll"_s, "added");
    }
    update::SwapPlan plan(const QString& root)
    {
        return {QDir::toNativeSeparators(root + u"/app"_s).toStdWString(),
            QDir::toNativeSeparators(root + u"/app/.stage"_s).toStdWString(),
            QDir::toNativeSeparators(root + u"/app/.backup"_s).toStdWString()};
    }

    void portableSwapKeepsData()
    {
        QTemporaryDir dir;
        const QString root = dir.path();
        makeTree(root);
        update::SwapMoves moves;
        QVERIFY(update::swapIn(plan(root), moves, {}));
        QCOMPARE(readFile(root + u"/app/WinShun.exe"_s), QByteArray("new exe"));
        QCOMPARE(readFile(root + u"/app/Qt6Core.dll"_s), QByteArray("new core"));
        QCOMPARE(readFile(root + u"/app/Added.dll"_s), QByteArray("added"));
        QVERIFY(QFileInfo::exists(root + u"/app/qml/New.qml"_s));
        QVERIFY(!QFileInfo::exists(root + u"/app/qml/Old.qml"_s)); // the folder is replaced as a whole
        QCOMPARE(readFile(root + u"/app/data/WinShun/WinShun.ini"_s), QByteArray("settings"));
        QCOMPARE(readFile(root + u"/app/data/index.bin"_s), QByteArray("index"));
        QCOMPARE(readFile(root + u"/app/mine.txt"_s), QByteArray("the user's")); // not ours: not touched
        QCOMPARE(readFile(root + u"/app/.backup/WinShun.exe"_s), QByteArray("old exe"));
        QVERIFY(update::removeTree(plan(root).backup));
        QVERIFY(update::removeTree(plan(root).stage));
        QVERIFY(!QFileInfo::exists(root + u"/app/.backup"_s));
    }

    void portableSwapRollsBack_data()
    {
        QTest::addColumn<int>("failAfter");
        for (int i = 0; i < 7; ++i)
            QTest::newRow(QByteArray::number(i)) << i;
    }
    void portableSwapRollsBack()
    {
        QFETCH(int, failAfter);
        QTemporaryDir dir;
        const QString root = dir.path();
        makeTree(root);
        update::SwapMoves moves;
        QVERIFY(!update::swapIn(plan(root), moves, {}, failAfter));
        QCOMPARE(readFile(root + u"/app/WinShun.exe"_s), QByteArray("old exe"));
        QCOMPARE(readFile(root + u"/app/Qt6Core.dll"_s), QByteArray("old core"));
        QCOMPARE(readFile(root + u"/app/qml/Old.qml"_s), QByteArray("old qml"));
        QVERIFY(!QFileInfo::exists(root + u"/app/Added.dll"_s));
        QCOMPARE(readFile(root + u"/app/data/index.bin"_s), QByteArray("index"));
        QCOMPARE(readFile(root + u"/app/mine.txt"_s), QByteArray("the user's"));
        QCOMPARE(readFile(root + u"/app/.stage/WinShun.exe"_s), QByteArray("new exe")); // back where it was
        QCOMPARE(readFile(root + u"/app/.stage/qml/New.qml"_s), QByteArray("new qml"));
    }

    void rollBackAfterSwap()
    {
        // The new version did not come up: everything goes back.
        QTemporaryDir dir;
        const QString root = dir.path();
        makeTree(root);
        update::SwapMoves moves;
        QVERIFY(update::swapIn(plan(root), moves, {}));
        QVERIFY(update::rollBack(plan(root), moves, {}));
        QCOMPARE(readFile(root + u"/app/WinShun.exe"_s), QByteArray("old exe"));
        QCOMPARE(readFile(root + u"/app/qml/Old.qml"_s), QByteArray("old qml"));
        QVERIFY(!QFileInfo::exists(root + u"/app/Added.dll"_s));
        QCOMPARE(readFile(root + u"/app/data/WinShun/WinShun.ini"_s), QByteArray("settings"));
        QVERIFY(QDir(root + u"/app/.backup"_s).isEmpty());
    }

    void stagedNamesSkipData()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(u"s/WinShun.exe"_s), "x");
        writeFile(dir.filePath(u"s/DATA/x"_s), "x");
        writeFile(dir.filePath(u"s/qml/a"_s), "x");
        const auto names = update::stagedNames(QDir::toNativeSeparators(dir.filePath(u"s"_s)).toStdWString());
        QCOMPARE(names, (std::vector<std::wstring> {L"WinShun.exe", L"qml"}));
    }

    // The real service and packages (WINSHUN_UPDATE_LIVE=1; ~150 MB): its
    // answer for both packages, the Setup straight from GitHub (whose
    // redirect goes to another host), the zip after a source that is not
    // there, then each checked as Updater checks it.
    void liveService_data()
    {
        QTest::addColumn<bool>("portable");
        QTest::newRow("setup") << false;
        QTest::newRow("portable") << true;
    }
    void liveService()
    {
        if (qEnvironmentVariableIsEmpty("WINSHUN_UPDATE_LIVE"))
            QSKIP("set WINSHUN_UPDATE_LIVE=1 to download from lcstp.com");
        QFETCH(bool, portable);
        QString feed = u"https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=10.0.22631"_s;
        if (portable)
            feed += u"&package=portable"_s;
        const http::Response answer = http::get(feed, {}, 20000, 1 << 20);
        std::optional<release::Info> newer;
        QCOMPARE(release::judge(answer.status, answer.body, u"0.0.1"_s, &newer), release::Answer::Newer);
        QCOMPARE(release::releasePage(newer->pageUrl), newer->pageUrl);
        const auto asset = release::packageAsset(*newer, portable ? release::Package::Portable : release::Package::Setup);
        QVERIFY(asset);
        QVERIFY(!release::packageAsset(*newer, portable ? release::Package::Setup : release::Package::Portable));
        const QStringList all = release::downloadUrls(*asset);
        QCOMPARE(all.last(), asset->url);
        const QStringList urls = portable
            ? QStringList {u"https://github.com/LingCore/WinShun/releases/download/v0.0.0/missing.zip"_s} + all
            : QStringList {asset->url};
        QTemporaryDir dir;
        const QString target = dir.filePath(portable ? u"p.zip"_s : u"s.exe"_s);
        const update::DownloadResult result = update::download(*asset, urls, target, {});
        qInfo().noquote() << "sources that failed:" << result.failures;
        QVERIFY(result.ok);
        QCOMPARE(result.failures.size(), portable ? 1 : 0);
        update::HeldFile held;
        QVERIFY(held.open(target));
        QByteArray bytes;
        QCOMPARE(QString::fromLatin1(held.sha256(&bytes, 400LL << 20)), asset->sha256);
        if (!portable) {
            QVERIFY(release::sameVersion(update::fileVersion(target), newer->version));
            return;
        }
        const QString stage = dir.filePath(u"stage"_s);
        QCOMPARE(update::unpackPortable(bytes, stage), QString());
        QVERIFY(release::sameVersion(update::fileVersion(stage + u"/WinShun.exe"_s), newer->version));
        QVERIFY(!QFileInfo::exists(stage + u"/data"_s));
        QVERIFY(QFileInfo::exists(stage + u"/qml"_s));
    }
};

QTEST_GUILESS_MAIN(TestUpdater)
#include "tst_updater.moc"
