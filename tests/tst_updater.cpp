// The in-app update (Updater): what the update service's answer gives,
// where packages are downloaded from and how they are checked, unpacking a
// portable zip safely, and swapping a portable copy's files with "data"
// kept and a failure rolled back.

#include "Release.h"
#include "UpdatePackage.h"
#include "platform/Http.h"
#include "Swap.h"
#include "UsageReport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUuid>
#include <QtEndian>

#include <miniz.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "TestJunction.h" // after winsock2.h: it brings windows.h

#include <atomic>
#include <map>
#include <mutex>
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
    unsigned requests() const { return m_requests.load(); }
    // The last request as it came: request line, headers and body.
    QByteArray lastRequest() const
    {
        const std::lock_guard lock(m_lastLock);
        return m_last;
    }

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
            // A POST's body, as long as its Content-Length says.
            const qsizetype headEnd = request.indexOf("\r\n\r\n") + 4;
            const qsizetype lengthAt = request.toLower().indexOf("\r\ncontent-length:");
            if (headEnd >= 4 && lengthAt >= 0 && lengthAt < headEnd) {
                const qsizetype lineEnd = request.indexOf("\r\n", lengthAt + 2);
                const qsizetype length = request.mid(lengthAt + 17, lineEnd - lengthAt - 17).trimmed().toLongLong();
                while (request.size() < headEnd + length) {
                    const int got = ::recv(client, buffer, sizeof(buffer), 0);
                    if (got <= 0)
                        break;
                    request.append(buffer, got);
                }
            }
            {
                const std::lock_guard lock(m_lastLock);
                m_last = request;
            }
            const QByteArray path = request.split(' ').value(1);
            ++m_requests;
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
    std::atomic_uint m_requests {0};
    mutable std::mutex m_lastLock;
    QByteArray m_last;
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

// An administrator working with normal rights (the split token UAC gives),
// whom UAC raises without a prompt: a test may then start a program as
// administrator without anyone having to answer.
bool elevatesWithoutPrompt()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION_TYPE type {};
    DWORD size = 0;
    const bool limited = ::GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size)
        && type == TokenElevationTypeLimited;
    ::CloseHandle(token);
    DWORD consent = 1;
    DWORD bytes = sizeof(consent);
    const bool read = ::RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                          L"ConsentPromptBehaviorAdmin", RRF_RT_REG_DWORD, nullptr, &consent, &bytes)
        == ERROR_SUCCESS;
    return limited && read && consent == 0;
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
    // GitHub #300: what a silent Setup waited for, from its log.
    void setupLogNamesWhatHeldTheFiles()
    {
        QCOMPARE(update::setupBlockedBy("2026-11-12 12:11:30.100   Starting the installation process.\r\n"), QString());
        const QByteArray log = QByteArrayLiteral("\xEF\xBB\xBF"
                                                 "2026-11-12 12:11:31.000   Update blocked by: old one\r\n"
                                                 "2026-11-12 12:11:41.000   Update blocked by: tio \xE4\xB8\xB2\xE5\x8F\xA3\xE7\xBB\x88\xE7\xAB\xAF\xE3\x80\x81Notepad\r\n"
                                                 "2026-11-12 12:11:41.100   Deinitializing Setup.\r\n");
        QCOMPARE(update::setupBlockedBy(log), u"tio 串口终端、Notepad"_s);
        QCOMPARE(update::setupBlockedBy("x Update blocked by: last line"), u"last line"_s);
        QCOMPARE(update::setupBlockedBy("Update blocked by: " + QByteArray(200, 'a')).size(), 80);
    }

    // GitHub #363: a copy run as administrator when it starts an update is
    // raised again after it; the mark is kept beside the other update state.
    void elevationBeforeUpdateIsRemembered()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString data = QDir::toNativeSeparators(dir.path());
        QVERIFY(!update::elevatedBeforeUpdate(data));
        update::rememberElevation(data, true);
        QVERIFY(update::elevatedBeforeUpdate(data));
        QVERIFY(QFile::exists(data + u"\\update.ini"_s));
        update::rememberElevation(data, false);
        QVERIFY(!update::elevatedBeforeUpdate(data));
    }

    // GitHub #400: restarted as administrator once the helper is done and
    // nothing is open; a window still in use a minute later gets a question,
    // not an endless wait.
    void elevationAfterUpdateWaitsThenAsks()
    {
        using update::ElevationStep;
        using update::ElevationWait;
        using namespace std::chrono_literals;
        const ElevationWait::Clock::time_point t0 {};

        { // nothing open: as soon as the helper is gone
            ElevationWait wait {t0};
            QCOMPARE(wait.next(t0 + 1s, true, true, false), ElevationStep::Wait);
            QCOMPARE(wait.next(t0 + 9s, true, true, true), ElevationStep::Wait);
            QCOMPARE(wait.next(t0 + 10s, false, true, false), ElevationStep::Restart);
        }
        { // not wanted (the mode changed meanwhile, raised by hand already): done, nothing asked
            ElevationWait wait {t0};
            QCOMPARE(wait.next(t0 + 10s, false, false, true), ElevationStep::Done);
        }
        { // a helper that never ends is left alone
            ElevationWait wait {t0};
            QCOMPARE(wait.next(t0 + ElevationWait::kHelperGiven - 1s, true, true, false), ElevationStep::Wait);
            QCOMPARE(wait.next(t0 + ElevationWait::kHelperGiven, true, true, false), ElevationStep::Done);
        }
        { // the launcher put away: restarted once it has been away a moment
            ElevationWait wait {t0};
            QCOMPARE(wait.next(t0 + 10s, false, true, true), ElevationStep::Wait);
            QCOMPARE(wait.next(t0 + 20s, false, true, true), ElevationStep::Wait);
            QCOMPARE(wait.next(t0 + 21s, false, true, false), ElevationStep::Wait); // an Enter's paste may still be going on
            QCOMPARE(wait.next(t0 + 20s + ElevationWait::kSettled, false, true, false), ElevationStep::Restart);
        }
        { // the reporter's case: the settings open on and on: asked a minute after the helper ended
            ElevationWait wait {t0};
            QCOMPARE(wait.next(t0 + 10s, false, true, true), ElevationStep::Wait);
            QVERIFY(wait.windowSeen);
            QCOMPARE(wait.next(t0 + 10s + ElevationWait::kWindowsGiven - 1s, false, true, true), ElevationStep::Wait);
            QCOMPARE(wait.next(t0 + 10s + ElevationWait::kWindowsGiven, false, true, true), ElevationStep::Ask);
        }
    }

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
        // A 304 (the ETag of an answer already had) is nothing understood by
        // itself; the Updater keeps what it has.
        QCOMPARE(release::judge(304, {}, u"0.14.0"_s, &newer), release::Answer::Invalid);
    }

    // docs/releasing.md "推送方式": the hidden line, and each copy's slot.
    void pushNowOrInTurn()
    {
        QCOMPARE(release::push(u"notes"_s), release::Push::Normal);
        QCOMPARE(release::push(u"notes\n\n<!-- winshun-push now -->"_s), release::Push::Now);
        QCOMPARE(release::push(u"notes\r\n\r\n<!-- winshun-push now -->\r\n\r\n<!-- winshun-delta 1 x -->"_s),
            release::Push::Now);
        QCOMPARE(release::push(u"say `<!-- winshun-push now -->` in a sentence"_s), release::Push::Normal);
        QCOMPARE(release::push(u"<!-- winshun-push later -->"_s), release::Push::Normal);

        QJsonObject feed = QJsonDocument::fromJson(kFeed).object();
        QVERIFY(!release::parse(kFeed)->published.isValid());
        feed.insert(u"published_at"_s, u"2026-11-17T08:45:01.000Z"_s);
        release::Info info = *release::parse(QJsonDocument(feed).toJson());
        const QDateTime published(QDate(2026, 11, 17), QTime(8, 45, 1), QTimeZone::UTC);
        QCOMPARE(info.published, published);

        // Normal: at the slot after publishing, not before.
        QVERIFY(!release::announceDue(info, 3600, published.addSecs(3599)));
        QVERIFY(release::announceDue(info, 3600, published.addSecs(3600)));
        QVERIFY(release::announceDue(info, 0, published));
        QVERIFY(!release::announceDue(info, release::kRolloutSeconds * 5, published.addSecs(release::kRolloutSeconds - 1)));
        QVERIFY(release::announceDue(info, release::kRolloutSeconds * 5, published.addSecs(release::kRolloutSeconds)));
        // Now: at once.
        info.notes += u"\n\n<!-- winshun-push now -->"_s;
        QVERIFY(release::announceDue(info, 3600, published));
        // No time of publishing: at once too.
        info.notes = u"notes"_s;
        info.published = {};
        QVERIFY(release::announceDue(info, 3600, published));
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

    // The daily usage report and the users count, in the 拾穗 service's
    // protocol (gleaning-web "installs", as MacShun sends it).
    void usageInstallIdIsTheServicesHash()
    {
        // SHA-256("gleaning.install.v1:winshun:<MachineGuid>"), as gleaning-web's
        // Send-GleaningInstallReport computes it.
        QCOMPARE(usage::installId(u"winshun", u"8f1c2a3b-0d4e-4f5a-9b6c-7d8e9f0a1b2c"),
            u"70350a6c0c47966078d9ea6530f3f9d535b8c940b7c1a1531504420dfdbdac48"_s);
        QVERIFY(usage::installId(u"winshun", u"x") != usage::installId(u"macshun", u"x"));
        QVERIFY(usage::installId(u"winshun", u"  ").isEmpty());
        QVERIFY(!usage::machineGuid().isEmpty()); // every Windows has one
    }

    void usageBodyFitsTheService()
    {
        const QString id = usage::installId(u"winshun", u"m");
        const QByteArray body = usage::reportBody(id, u"0.34.0"_s, u"10.0.26200"_s);
        QVERIFY(body.size() < 1024);
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        QCOMPARE(o.keys(), (QStringList {u"arch"_s, u"installId"_s, u"osVersion"_s, u"platform"_s, u"version"_s}));
        QCOMPARE(o.value(u"installId"_s).toString(), id);
        QCOMPARE(o.value(u"platform"_s).toString(), u"windows"_s);
        QCOMPARE(o.value(u"arch"_s).toString(), u"x64"_s);
        QVERIFY(!QJsonDocument::fromJson(usage::reportBody(id, u"0.34.0"_s, {})).object().contains(u"osVersion"_s));
    }

    void usageUrlsAtTheFeedsOrigin()
    {
        const QString feed = u"https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64"_s;
        QCOMPARE(usage::reportUrl(feed), u"https://lcstp.com/api/v1/projects/winshun/installs"_s);
        QCOMPARE(usage::projectsUrl(feed), u"https://lcstp.com/api/v1/projects"_s);
        QCOMPARE(usage::reportUrl(u"http://127.0.0.1:8123/feed.json"_s), u"http://127.0.0.1:8123/api/v1/projects/winshun/installs"_s);
        for (const QString& bad : {u""_s, u"http://lcstp.com/feed"_s, u"https://user@lcstp.com/feed"_s, u"ftp://lcstp.com/"_s})
            QVERIFY2(usage::reportUrl(bad).isEmpty() && usage::projectsUrl(bad).isEmpty(), qPrintable(bad));
    }

    void usageOncePerDay()
    {
        const QDate today(2026, 11, 17);
        QVERIFY(usage::due({}, today));
        QVERIFY(!usage::due(today, today));
        QVERIFY(usage::due(today.addDays(-1), today));
        QVERIFY(usage::due(today.addDays(1), today)); // the clock was set back
    }

    void usageCountsFromTheProjectsList()
    {
        const QByteArray json = R"({"projects":[
            {"slug":"macshun","stats":{"downloads":79,"users":30}},
            {"slug":"winshun","stats":{"downloads":869,"users":0}},
            {"slug":"moye","stats":{"users":2.5}},
            {"slug":"odd","stats":{"users":"12"}},
            {"slug":"","stats":{"users":4}},
            {"slug":"mojian3d"}]})";
        const QHash<QString, int> counts = usage::userCounts(json);
        QCOMPARE(counts.size(), 1);
        QCOMPARE(counts.value(u"macshun"_s), 30);
        QVERIFY(usage::userCounts("not json").isEmpty());
    }

    // The report goes out as a POST of the body, follows no redirect, and the
    // service's 204 comes back.
    void usageReportIsPosted()
    {
        FakeServer server({{"/api/v1/projects/winshun/installs", {204, {}}}, {"/moved", {302, {}}}});
        const QString url = usage::reportUrl(server.url("/updates/winshun/stable.json"));
        const QByteArray body = usage::reportBody(usage::installId(u"winshun", u"m"), u"0.34.0"_s, u"10.0.26200"_s);
        const http::Response answer = http::post(url, {{u"Content-Type"_s, u"application/json"_s}}, body, 5000);
        QCOMPARE(answer.status, 204);
        const QByteArray request = server.lastRequest();
        QVERIFY2(request.startsWith("POST /api/v1/projects/winshun/installs HTTP/1.1\r\n"), request.constData());
        QVERIFY(request.contains("\r\nContent-Type: application/json\r\n"));
        QVERIFY(request.endsWith("\r\n\r\n" + body));
        QCOMPARE(http::post(server.url("/moved"), {}, "{}", 5000).status, 302);
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

    void downloadCancelsBeforeTryingAnotherMirror()
    {
        const QByteArray good("valid package");
        FakeServer server({{"/unavailable", {503, {}}}, {"/good", {200, good}}});
        release::Asset asset;
        asset.size = good.size();
        asset.sha256 = QString::fromLatin1(sha256(good));
        QTemporaryDir dir;
        const auto result = update::download(asset, {server.url("/unavailable"), server.url("/good")}, dir.filePath(u"pkg.zip"_s),
            [&](qint64, qint64) { return server.requests() == 0; });
        QVERIFY(result.cancelled);
        QVERIFY(!result.ok);
        QCOMPARE(server.requests(), 1u);
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

    // Security software reads a new program file for a moment, holding it
    // with writing allowed: the hold is waited out, not taken for a bad
    // package (what turned a good 0.48.0 download into "没有通过检查").
    void heldFileWaitsOutAScan()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"setup.exe"_s);
        writeFile(path, "package bytes");
        const HANDLE scanner = ::CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(scanner != INVALID_HANDLE_VALUE);
        std::thread done([scanner] {
            ::Sleep(700);
            ::CloseHandle(scanner);
        });
        QElapsedTimer clock;
        clock.start();
        update::HeldFile held;
        const bool opened = held.open(path, std::chrono::seconds(10));
        const qint64 waited = clock.elapsed();
        done.join();
        QVERIFY(opened);
        QVERIFY2(waited >= 500, qPrintable(QString::number(waited)));
        QCOMPARE(held.error(), 0ul);
        QCOMPARE(held.sha256(), sha256("package bytes"));
    }

    void heldFileSaysWhyItCouldNotHold()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"setup.exe"_s);
        writeFile(path, "package bytes");
        const HANDLE scanner = ::CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(scanner != INVALID_HANDLE_VALUE);
        const auto release = qScopeGuard([scanner] { ::CloseHandle(scanner); });
        update::HeldFile held;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(!held.open(path, std::chrono::milliseconds(600)));
        QVERIFY(clock.elapsed() >= 500); // tried for the time given
        QCOMPARE(held.error(), static_cast<unsigned long>(ERROR_SHARING_VIOLATION));
        QVERIFY(update::blockedOnThisComputer(held.error()));
        // Cancelled (Win顺 quitting): no more waiting.
        std::atomic<bool> cancel {true};
        clock.restart();
        QVERIFY(!held.open(path, std::chrono::seconds(10), &cancel));
        QVERIFY(clock.elapsed() < 2000);
        // Taken away (deleted or quarantined): said at once.
        clock.restart();
        QVERIFY(!held.open(dir.filePath(u"gone.exe"_s), std::chrono::seconds(10)));
        QVERIFY(clock.elapsed() < 2000);
        QCOMPARE(held.error(), static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
        QVERIFY(update::blockedOnThisComputer(held.error()));
        QVERIFY(update::blockedOnThisComputer(ERROR_VIRUS_INFECTED));
        QVERIFY(update::blockedOnThisComputer(ERROR_VIRUS_DELETED));
        QVERIFY(!update::blockedOnThisComputer(ERROR_RESOURCE_TYPE_NOT_FOUND)); // no version resource: the package
        // A file without a version: no Windows error to blame, the package's own fault.
        const QString plain = dir.filePath(u"plain.exe"_s);
        writeFile(plain, "not a program");
        unsigned long error = 0;
        QVERIFY(update::fileVersion(plain, &error).isEmpty());
        QVERIFY(error != 0);
        QVERIFY2(!update::blockedOnThisComputer(error), qPrintable(QString::number(error)));
        QVERIFY(update::fileVersion(dir.filePath(u"gone.exe"_s), &error).isEmpty());
        QCOMPARE(error, static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
    }

    // The download's rename waits out a scan of the file it replaces.
    void downloadWaitsOutAHeldTarget()
    {
        const QByteArray good("the new package");
        FakeServer server({{"/good", {200, good}}});
        release::Asset asset;
        asset.size = good.size();
        asset.sha256 = QString::fromLatin1(sha256(good));
        QTemporaryDir dir;
        const QString target = dir.filePath(u"setup.exe"_s);
        writeFile(target, "an old download");
        const HANDLE scanner = ::CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(target).utf16()),
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); // no FILE_SHARE_DELETE
        QVERIFY(scanner != INVALID_HANDLE_VALUE);
        std::thread done([scanner] {
            ::Sleep(700);
            ::CloseHandle(scanner);
        });
        const update::DownloadResult result = update::download(asset, {server.url("/good")}, target, {});
        done.join();
        QVERIFY2(result.ok, qPrintable(result.failures.join(u'\n')));
        QVERIFY(!result.blocked);
        QCOMPARE(readFile(target), good);
        QVERIFY(!QFile::exists(target + u".partial"_s));
    }

    // A Scoop copy has a data folder too (persist), but is updated by Scoop, not by a swap.
    void scoopCopyIsNotPortable()
    {
        QTemporaryDir root;
        const QString version = root.path() + u"/scoop/apps/winshun/0.18.0"_s;
        QDir().mkpath(version + u"/data"_s);
        for (const QString& name : {u"scoop-manifest.json"_s, u"scoop-install.json"_s}) {
            QFile file(version + u'/' + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{}");
        }
        QCOMPARE(update::installKind(version, true), update::Kind::Scoop);
        const QString plain = root.path() + u"/portable"_s;
        QDir().mkpath(plain + u"/data"_s);
        QCOMPARE(update::installKind(plain, true), update::Kind::Portable);
    }

    // A portable copy keeps its downloads, the helper's copy and the logs
    // with itself (GitHub #484); the others in the user's own folder.
    void updatesFolderByKind()
    {
        using update::Kind;
        const QString local = u"C:/Users/me/AppData/Local"_s;
        const QString data = uR"(D:\Tools\WinShun\data)"_s;
        QCOMPARE(update::updatesFolder(Kind::Portable, data, data, local), uR"(D:\Tools\WinShun\data\Updates)"_s);
        QCOMPARE(update::updatesFolder(Kind::Scoop, data, data, local), uR"(C:\Users\me\AppData\Local\WinShun\Updates)"_s);
        QCOMPARE(update::updatesFolder(Kind::AllUsers, {}, {}, local), uR"(C:\Users\me\AppData\Local\WinShun\Updates)"_s);
        QCOMPARE(update::updatesFolder(Kind::CurrentUser, {}, {}, local), uR"(C:\Users\me\AppData\Local\WinShun\Updates)"_s);
        // --profile, for tests: its own, whatever the copy is.
        QCOMPARE(update::updatesFolder(Kind::Portable, uR"(E:\probe)"_s, data, local), uR"(E:\probe\Updates)"_s);
        QCOMPARE(update::updatesFolder(Kind::None, uR"(E:\probe)"_s, {}, local), uR"(E:\probe\Updates)"_s);
    }

    // What an earlier version's portable copy left in %LOCALAPPDATA%\WinShun
    // goes, with the folder, only when that is all there is.
    void strayUpdatesOfAPortableCopy()
    {
        QTemporaryDir dir;
        MadeLinks made; // after the folder: the link goes first, as a link
        const auto leave = [&](const QString& name) {
            const QString folder = dir.filePath(name);
            writeFile(folder + u"/Updates/update.log"_s, "log");
            writeFile(folder + u"/Updates/WinShunUpdate.exe"_s, "helper");
            return folder;
        };
        const QString alone = leave(u"alone"_s);
        QVERIFY(update::removeStrayUpdates(alone));
        QVERIFY(!QFileInfo::exists(alone));

        // An installed copy's files beside it: its folder, left as it is.
        const QString shared = leave(u"shared"_s);
        writeFile(shared + u"/state.ini"_s, "installed");
        QVERIFY(!update::removeStrayUpdates(shared));
        QCOMPARE(readFile(shared + u"/Updates/update.log"_s), QByteArray("log"));

        // A folder in it (an installed copy's delta): not a portable copy's leavings.
        const QString delta = leave(u"delta"_s);
        writeFile(delta + u"/Updates/WinShun-0.50.0-x64-delta/payload"_s, "x");
        QVERIFY(!update::removeStrayUpdates(delta));
        QCOMPARE(readFile(delta + u"/Updates/WinShunUpdate.exe"_s), QByteArray("helper"));

        // "Updates" a link to somewhere else: nothing behind it is touched.
        writeFile(dir.filePath(u"elsewhere/kept.txt"_s), "kept");
        const QString linked = dir.filePath(u"linked"_s);
        QVERIFY(QDir().mkpath(linked));
        QVERIFY(makeJunction(made << linked + u"/Updates"_s, dir.filePath(u"elsewhere"_s)));
        QVERIFY(!update::removeStrayUpdates(linked));
        QCOMPARE(readFile(dir.filePath(u"elsewhere/kept.txt"_s)), QByteArray("kept"));
        QVERIFY(QFileInfo::exists(linked + u"/Updates"_s));

        QVERIFY(!update::removeStrayUpdates(dir.filePath(u"none"_s)));
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
        // No program in it: refused, the package's fault.
        bool writeFailed = true;
        QVERIFY(!update::unpackPortable(makeZip({{"WinShun/other.exe", "x"}}), dir.filePath(u"none"_s), &writeFailed).isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath(u"none"_s)));
        QVERIFY(!writeFailed);
        // A good package that cannot be written here (a file where a folder goes;
        // security software refusing a new program file is alike): this computer's.
        writeFile(dir.filePath(u"blocker"_s), "a file");
        QVERIFY(!update::unpackPortable(makeZip({{"WinShun.exe", "x"}}), dir.filePath(u"blocker/stage"_s), &writeFailed).isEmpty());
        QVERIFY(writeFailed);
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

    // A whole run of WinShunUpdate.exe --portable, as Updater starts it once
    // the old copy has ended (none to wait for here, and a mutex of the
    // test's own in place of Win顺's): the new files go in, the new version
    // comes up, nothing of the old is left. With "以管理员身份运行此程序"
    // ticked in WinShun.exe's properties (RUNASADMIN, GitHub #274) a plain
    // start of it fails (740): the new version must still come up, then as
    // administrator, and not be swapped back for the old.
    void helperStartsNewVersion_data()
    {
        QTest::addColumn<bool>("runAsAdministrator");
        QTest::addColumn<bool>("fromData");
        QTest::newRow("plain") << false << false;
        QTest::newRow("run-as-administrator") << true << false;
        // As Updater runs it for a portable copy (GitHub #484): the helper's
        // copy and its log in the copy's own data\Updates, inside the folder
        // whose other entries it replaces.
        QTest::newRow("from-the-data-folder") << false << true;
    }
    void helperStartsNewVersion()
    {
        QFETCH(bool, runAsAdministrator);
        QFETCH(bool, fromData);
        if (runAsAdministrator && !elevatesWithoutPrompt())
            QSKIP("needs an administrator account with normal rights here and UAC that elevates without asking "
                  "(ConsentPromptBehaviorAdmin=0); elsewhere the prompt would wait for an answer");
        QTemporaryDir dir;
        const QString app = dir.filePath(u"app"_s);
        QVERIFY(QDir().mkpath(app + u"/.stage"_s));
        QVERIFY(QFile::copy(QStringLiteral(WINSHUN_FAKE_APP_EXE), app + u"/WinShun.exe"_s));
        QVERIFY(QFile::copy(QStringLiteral(WINSHUN_FAKE_APP_EXE), app + u"/.stage/WinShun.exe"_s));
        writeFile(app + u"/version.txt"_s, "old");
        writeFile(app + u"/data/keep.txt"_s, "the user's");
        writeFile(app + u"/.stage/version.txt"_s, "new");

        const std::wstring exe = QDir::toNativeSeparators(app + u"/WinShun.exe"_s).toStdWString();
        constexpr auto kLayers = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
        const auto unflag = qScopeGuard([&] { ::RegDeleteKeyValueW(HKEY_CURRENT_USER, kLayers, exe.c_str()); });
        if (runAsAdministrator) {
            const std::wstring flag = L"~ RUNASADMIN";
            QCOMPARE(::RegSetKeyValueW(HKEY_CURRENT_USER, kLayers, exe.c_str(), REG_SZ, flag.c_str(),
                         static_cast<DWORD>((flag.size() + 1) * sizeof(wchar_t))),
                LSTATUS(ERROR_SUCCESS));
        }

        const QString updates = fromData ? app + u"/data/Updates"_s : dir.path();
        const QString log = updates + u"/update.log"_s;
        QString helperExe = QStringLiteral(WINSHUN_UPDATE_EXE);
        if (fromData) {
            QVERIFY(QDir().mkpath(updates));
            helperExe = updates + u"/WinShunUpdate.exe"_s;
            QVERIFY(QFile::copy(QStringLiteral(WINSHUN_UPDATE_EXE), helperExe));
        }
        QProcess helper;
        helper.start(helperExe,
            {u"--portable"_s, u"--pid"_s, u"0"_s, u"--ready"_s, u"Local\\WinShun.Test.Ready.%1"_s.arg(QUuid::createUuid().toString(QUuid::Id128)),
                u"--instance"_s, u"Local\\WinShun.Test.Instance.%1"_s.arg(QUuid::createUuid().toString(QUuid::Id128)),
                u"--app"_s, QDir::toNativeSeparators(app), u"--stage"_s, QDir::toNativeSeparators(app + u"/.stage"_s),
                u"--backup"_s, QDir::toNativeSeparators(app + u"/.backup"_s), u"--log"_s, QDir::toNativeSeparators(log)});
        QVERIFY(helper.waitForFinished(90000));
        qInfo().noquote() << u"helper log:\n"_s + QString::fromUtf8(readFile(log)).trimmed();
        QCOMPARE(helper.exitCode(), 0);
        QCOMPARE(readFile(app + u"/version.txt"_s), QByteArray("new"));
        QCOMPARE(readFile(app + u"/data/ran.txt"_s), runAsAdministrator ? QByteArray("new elevated") : QByteArray("new normal"));
        QCOMPARE(readFile(app + u"/data/keep.txt"_s), QByteArray("the user's"));
        QVERIFY(!QFileInfo::exists(app + u"/.backup"_s));
        QVERIFY(!QFileInfo::exists(app + u"/.stage"_s));
        if (fromData) // its own copy and its log stayed where they were, with the user's files
            QVERIFY(QFileInfo::exists(helperExe) && !readFile(log).isEmpty());
        // The stand-in ends by itself (one running as administrator is not
        // ours to end): the folder goes once it has. QTRY_ evaluates its
        // expression again after the wait, so it must hold once the file is gone.
        const QString exePath = QString::fromStdWString(exe);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::remove(exePath) || !QFileInfo::exists(exePath), 15000);
    }

    // The real service and packages (WINSHUN_UPDATE_LIVE=1; ~150 MB): its
    // answer for both packages, the Setup straight from GitHub (whose
    // redirect goes to another host), the zip after a source that is not
    // there, then each checked as Updater checks it.
    // The checks every 10 minutes (Updater::check): lcstp.com's answer has an
    // ETag, a time of publishing, and an unchanged answer is a 304 without a body.
    void liveServiceUnchanged()
    {
        if (qEnvironmentVariableIsEmpty("WINSHUN_UPDATE_LIVE"))
            QSKIP("set WINSHUN_UPDATE_LIVE=1 to ask lcstp.com");
        const QString feed = u"https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=10.0.22631"_s;
        const http::Response first = http::get(feed, {}, 20000, 1 << 20);
        QCOMPARE(first.status, 200);
        QVERIFY(first.etag.startsWith(u'"') && first.etag.endsWith(u'"'));
        QVERIFY(release::parse(first.body)->published.isValid());
        const http::Response again = http::get(feed, {{u"If-None-Match"_s, first.etag}}, 20000, 1 << 20);
        QCOMPARE(again.status, 304);
        QVERIFY(again.body.isEmpty());
    }

    void liveService_data()
    {
        QTest::addColumn<bool>("portable");
        QTest::addColumn<int>("sourceIndex");
        QTest::newRow("setup") << false << -1;
        QTest::newRow("portable") << true << -1;
        QTest::newRow("setup-first-mirror") << false << 0;
        QTest::newRow("setup-second-mirror") << false << 1;
    }
    void liveService()
    {
        if (qEnvironmentVariableIsEmpty("WINSHUN_UPDATE_LIVE"))
            QSKIP("set WINSHUN_UPDATE_LIVE=1 to download from lcstp.com");
        QFETCH(bool, portable);
        QFETCH(int, sourceIndex);
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
        QVERIFY(all.size() >= 3);
        const QStringList urls = sourceIndex >= 0 ? QStringList {all[sourceIndex]}
            : portable ? QStringList {u"https://github.com/LingCore/WinShun/releases/download/v0.0.0/missing.zip"_s} + all
                       : QStringList {asset->url};
        QTemporaryDir dir;
        const QString target = dir.filePath(portable ? u"p.zip"_s : u"s.exe"_s);
        const update::DownloadResult result = update::download(*asset, urls, target, {});
        qInfo().noquote() << "requested sources:" << urls;
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
