// The cloud backup plugin (src/cloudbackup-plugin, GitHub #276): the names of
// the backups, what a WebDAV server and GitHub answer, both places against a
// local server that keeps the files in memory, and WinShunCloudBackup.exe
// itself over its protocol, the test playing Win顺 (backup_export,
// backup_restore) and the cloud.

#include "Cloud.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <deque>
#include <future>
#include <map>

using winshun::Json;
using namespace Qt::StringLiterals;

namespace {

// A WebDAV folder at /dav/WinShun/ (user "user", password "secret") and
// GitHub's REST API for the repository me/backups (token "token"), in memory.
class FakeCloud : public QObject {
public:
    FakeCloud()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input = QByteArray()]() mutable {
                    input += socket->readAll();
                    for (;;) {
                        const qsizetype head = input.indexOf("\r\n\r\n");
                        if (head < 0)
                            return;
                        const QList<QByteArray> lines = input.first(head).split('\n');
                        QMap<QByteArray, QByteArray> headers;
                        for (qsizetype i = 1; i < lines.size(); ++i) {
                            const qsizetype colon = lines[i].indexOf(':');
                            if (colon > 0)
                                headers[lines[i].first(colon).trimmed().toLower()] = lines[i].mid(colon + 1).trimmed();
                        }
                        const qsizetype length = headers.value("content-length").toLongLong();
                        if (input.size() < head + 4 + length)
                            return;
                        const QList<QByteArray> start = lines[0].trimmed().split(' ');
                        const QByteArray body = input.mid(head + 4, length);
                        input.remove(0, head + 4 + length);
                        handle(socket, start.value(0), start.value(1), headers, body);
                    }
                });
            }
        });
        m_ok = m_server.listen(QHostAddress::LocalHost, 0);
    }

    bool ok() const { return m_ok; }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(m_server.serverPort()); }

    bool davFolder = false;
    QMap<QByteArray, QByteArray> dav; // name -> bytes
    bool hubPrivate = true;
    QMap<QByteArray, QByteArray> hub;
    QList<QByteArray> requests; // "PUT /dav/WinShun/…"

private:
    static void reply(QTcpSocket* socket, int status, const QByteArray& body = {}, const QByteArray& type = "application/json")
    {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: " + type + "\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    }

    static QByteArray sha(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex(); }

    void handle(QTcpSocket* socket, const QByteArray& method, const QByteArray& path, const QMap<QByteArray, QByteArray>& headers,
        const QByteArray& body)
    {
        requests.append(method + ' ' + path);
        if (path.startsWith("/dav/WinShun/")) {
            if (headers.value("authorization") != "Basic " + QByteArray("user:secret").toBase64())
                return reply(socket, 401);
            const QByteArray name = QByteArray::fromPercentEncoding(path.mid(13));
            if (method == "MKCOL") {
                const bool was = davFolder;
                davFolder = true;
                return reply(socket, was ? 405 : 201);
            }
            if (!davFolder)
                return reply(socket, 404);
            if (method == "PROPFIND") {
                QByteArray xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:multistatus xmlns:d=\"DAV:\">"
                                 "<d:response><d:href>/dav/WinShun/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/>"
                                 "</d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
                for (auto it = dav.cbegin(); it != dav.cend(); ++it)
                    xml += "<d:response><d:href>/dav/WinShun/" + it.key().toPercentEncoding() + "</d:href><d:propstat><d:prop>"
                        "<d:getcontentlength>" + QByteArray::number(it.value().size()) + "</d:getcontentlength></d:prop>"
                        "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
                return reply(socket, 207, xml + "</d:multistatus>", "application/xml");
            }
            if (method == "PUT") {
                dav[name] = body;
                return reply(socket, 201);
            }
            if (method == "GET")
                return dav.contains(name) ? reply(socket, 200, dav.value(name), "application/zip") : reply(socket, 404);
            if (method == "DELETE")
                return reply(socket, dav.remove(name) ? 204 : 404);
            return reply(socket, 405);
        }
        if (path.startsWith("/repos/me/backups")) {
            if (headers.value("authorization") != "Bearer token")
                return reply(socket, 401, R"({"message":"Bad credentials"})");
            const QByteArray rest = path.mid(17);
            if (rest.isEmpty())
                return reply(socket, 200, hubPrivate ? R"({"private":true})" : R"({"private":false})");
            if (rest == "/contents/WinShun" && method == "GET") {
                if (hub.isEmpty())
                    return reply(socket, 404, R"({"message":"Not Found"})");
                QJsonArray listing;
                for (auto it = hub.cbegin(); it != hub.cend(); ++it)
                    listing.append(QJsonObject {{u"type"_s, u"file"_s}, {u"name"_s, QString::fromUtf8(it.key())},
                        {u"size"_s, it.value().size()}, {u"sha"_s, QString::fromLatin1(sha(it.value()))}});
                listing.append(QJsonObject {{u"type"_s, u"file"_s}, {u"name"_s, u"README.md"_s}, {u"size"_s, 3}, {u"sha"_s, u"x"_s}});
                return reply(socket, 200, QJsonDocument(listing).toJson(QJsonDocument::Compact));
            }
            if (!rest.startsWith("/contents/WinShun/"))
                return reply(socket, 404, R"({"message":"Not Found"})");
            const QByteArray name = QByteArray::fromPercentEncoding(rest.mid(18));
            const QJsonObject sent = QJsonDocument::fromJson(body).object();
            if (method == "PUT") {
                if (hub.contains(name))
                    return reply(socket, 422, R"({"message":"sha was not supplied"})");
                hub[name] = QByteArray::fromBase64(sent.value(u"content"_s).toString().toLatin1());
                return reply(socket, 201, "{}");
            }
            if (method == "GET") {
                if (!hub.contains(name) || headers.value("accept") != "application/vnd.github.raw+json")
                    return reply(socket, 404, R"({"message":"Not Found"})");
                return reply(socket, 200, hub.value(name), "application/octet-stream");
            }
            if (method == "DELETE") {
                if (!hub.contains(name))
                    return reply(socket, 404, R"({"message":"Not Found"})");
                if (sent.value(u"sha"_s).toString().toLatin1() != sha(hub.value(name)))
                    return reply(socket, 409, R"({"message":"does not match"})");
                hub.remove(name);
                return reply(socket, 200, "{}");
            }
        }
        reply(socket, 404, R"({"message":"Not Found"})");
    }

    QTcpServer m_server;
    bool m_ok = false;
};

// A blocking call of the plugin's while this thread serves the fake cloud.
template <typename F>
auto away(F call)
{
    auto future = std::async(std::launch::async, std::move(call));
    while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        QTest::qWait(2);
    return future.get();
}

cb::Settings settingsFor(const FakeCloud& cloud)
{
    cb::Settings s;
    s.webdavUrl = cloud.base() + "/dav/WinShun";
    s.webdavUser = "user";
    s.webdavPassword = "secret";
    s.githubRepo = "me/backups";
    s.githubToken = "token";
    s.githubApi = cloud.base();
    return s;
}

// WinShunCloudBackup.exe with the test at the other end of its protocol.
struct Process {
    QProcess process;
    QByteArray output;
    std::map<long long, Json> replies;
    std::deque<Json> tools; // winshun/tool requests not answered yet
    std::vector<std::string> notices; // winshun/notify texts
    long long nextId = 1;

    bool start(const QString& dataDir, Json settings)
    {
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [this] {
            output += process.readAllStandardOutput();
            while (output.contains('\n')) {
                const auto end = output.indexOf('\n');
                const auto message = Json::parse(output.first(end).toStdString());
                output.remove(0, end + 1);
                if (!message)
                    continue;
                const std::string method = (*message)["method"].toString();
                if (method == "winshun/tool")
                    tools.push_back(*message);
                else if (method == "winshun/notify")
                    notices.push_back((*message)["params"]["text"].toString());
                else if (method.empty() && (*message)["id"].isNumber())
                    replies[(*message)["id"].toInt()] = *message;
            }
        });
        process.start(QCoreApplication::applicationDirPath() + u"/WinShunCloudBackup.exe"_s);
        if (!process.waitForStarted())
            return false;
        const long long id = request("initialize", Json::Object {{"locale", "zh"}, {"dataDir", QDir::toNativeSeparators(dataDir).toStdString()},
            {"settings", std::move(settings)}});
        return QTest::qWaitFor([&] { return replies.contains(id); });
    }

    void write(const Json& message) { process.write(QByteArray::fromStdString(message.dump()) + '\n'); }
    long long request(const char* method, Json params)
    {
        const long long id = nextId++;
        write(Json::Object {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
        return id;
    }
    // The answer's result, null when none came.
    Json call(const char* method, Json params)
    {
        const long long id = request(method, std::move(params));
        if (!QTest::qWaitFor([&] { return replies.contains(id); }))
            return {};
        return replies[id]["result"];
    }
    Json rows() { return call("query", Json::Object {{"queryId", nextId}, {"command", "cloud"}, {"text", ""}})["items"]; }
    Json act(const Json& item) { return call("action", Json::Object {{"command", "cloud"}, {"action", "default"}, {"item", item}}); }
    // The next tool call of the plugin's, answered with `result` by `answer`.
    bool tool(const std::string& name, const std::function<Json(const Json& arguments)>& answer)
    {
        if (!QTest::qWaitFor([&] { return !tools.empty(); }, 15000))
            return false;
        const Json asked = tools.front();
        tools.pop_front();
        if (asked["params"]["name"].toString() != name)
            return false;
        Json reply = answer(asked["params"]["arguments"]);
        Json message(Json::Object {{"jsonrpc", "2.0"}, {"id", asked["id"]}});
        if (reply.has("error"))
            message.set("error", reply["error"]);
        else
            message.set("result", std::move(reply));
        write(message);
        return true;
    }
};

Json rowOf(const Json& rows, const std::string& idStart)
{
    for (const Json& row : rows.toArray()) {
        if (row["id"].toString().rfind(idStart, 0) == 0)
            return row;
    }
    return {};
}

// A place's row once the worker is done with it (while it is at it, the row
// says what it does and Enter does nothing).
Json idleRow(Process& p, const std::string& id)
{
    Json row;
    const bool idle = QTest::qWaitFor([&] {
        row = rowOf(p.rows(), id);
        return row["data"].isObject();
    }, 15000);
    return idle ? row : Json();
}

} // namespace

class TestCloudBackup : public QObject {
    Q_OBJECT

private slots:
    void names()
    {
        const SYSTEMTIME when {2026, 11, 4, 12, 19, 47, 3, 0};
        QCOMPARE(cb::backupName(when, "DESKTOP-ABC"), std::string("WinShun-2026-11-12-194703-DESKTOP-ABC.zip"));
        QCOMPARE(cb::backupName(when, "办公室"), std::string("WinShun-2026-11-12-194703.zip")); // nothing that fits everywhere
        QCOMPARE(cb::backupName(when, "-my pc_"), std::string("WinShun-2026-11-12-194703-mypc.zip"));

        const auto parsed = cb::parseName("WinShun-2026-11-12-194703-DESKTOP-ABC.zip");
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->when, std::string("2026-11-12 19:47"));
        QCOMPARE(parsed->second, std::string("03"));
        QCOMPARE(parsed->computer, std::string("DESKTOP-ABC"));
        // What Win顺 itself names a backup (a folder synced by something else).
        QVERIFY(cb::parseName("WinShun-2026-10-13-194700.zip").has_value());
        QVERIFY(cb::parseName("WinShun-2026-10-13-194700.zip")->computer.empty());
        for (const char* other : {"README.md", "WinShun-2026-10-13.zip", "WinShun-2026-10-13-194700.zip.part",
                 "WinShun-2026-10-13-1947000.zip", "WinShun-2026-10-13-194700-.zip", "WinShun-2026-10-13-194700-a b.zip", "x.zip"})
            QVERIFY2(!cb::parseName(other).has_value(), other);
    }

    void encodings()
    {
        QCOMPARE(cb::base64(""), std::string(""));
        QCOMPARE(cb::base64("a"), std::string("YQ=="));
        QCOMPARE(cb::base64("ab"), std::string("YWI="));
        QCOMPARE(cb::base64("abc"), std::string("YWJj"));
        const QByteArray bytes = QByteArray::fromHex("00ff10807f").repeated(301);
        QCOMPARE(QByteArray::fromStdString(cb::base64(bytes.toStdString())), bytes.toBase64());
        QCOMPARE(cb::urlEncode("a b/c~.zip"), std::string("a%20b%2Fc~.zip"));
        QCOMPARE(cb::urlDecode("a%20b%2Fc%E5%A4%87%zz"), std::string("a b/c备%zz"));
        QCOMPARE(cb::encodeAddress("https://nas/dav/我的 备份/"), std::string("https://nas/dav/%E6%88%91%E7%9A%84%20%E5%A4%87%E4%BB%BD/"));
    }

    void repositories()
    {
        for (const char* text : {"me/backups", " me/backups ", "https://github.com/me/backups", "https://github.com/me/backups.git",
                 "github.com/me/backups/", "https://github.com/me/backups/tree/main"}) {
            const auto repo = cb::parseRepo(text);
            QVERIFY2(repo.has_value(), text);
            QCOMPARE(repo->owner, std::string("me"));
            QCOMPARE(repo->name, std::string("backups"));
        }
        for (const char* text : {"", "backups", "/backups", "me/", "me/back ups", "https://github.com/"})
            QVERIFY2(!cb::parseRepo(text).has_value(), text);
    }

    void listing()
    {
        // 坚果云 writes "d:", Nextcloud "d:" with more namespaces, IIS "D:" and
        // whole addresses, some none at all.
        std::string xml = R"(<?xml version="1.0"?><D:multistatus xmlns:D="DAV:">
<D:response><D:href>@NAS@/backup/WinShun/</D:href><D:propstat><D:prop><D:getcontentlength/></D:prop></D:propstat></D:response>
<D:response><D:href>@NAS@/backup/WinShun/WinShun-2026-11-10-080000-PC1.zip</D:href><D:propstat><D:prop><D:getcontentlength>2048</D:getcontentlength></D:prop></D:propstat></D:response>
<response xmlns="DAV:"><href>/backup/WinShun/WinShun-2026-11-12-090000.zip</href><propstat><prop><getcontentlength>10</getcontentlength></prop></propstat></response>
<d:response xmlns:d="DAV:"><d:href>/backup/WinShun/%E7%AC%94%E8%AE%B0.txt</d:href></d:response>
<d:response xmlns:d="DAV:"><d:href>/backup/WinShun/WinShun-2026-11-11-070000-A&amp;B.zip</d:href></d:response>
<d:response xmlns:d="DAV:"><d:href>/backup/WinShun/WinShun-2026-11-11-070000-PC2.zip/</d:href></d:response>
</D:multistatus>)";
        // (moc loses the class at two slashes inside a raw string.)
        for (std::size_t at = xml.find("@NAS@"); at != std::string::npos; at = xml.find("@NAS@"))
            xml.replace(at, 5, std::string("http:/") + "/nas:5005");
        const std::vector<cb::Remote> found = cb::webdavListing(xml);
        QCOMPARE(found.size(), std::size_t(3));
        QCOMPARE(found[0].name, std::string("WinShun-2026-11-12-090000.zip")); // newest first
        QCOMPARE(found[0].size, 10ll);
        QCOMPARE(found[1].name, std::string("WinShun-2026-11-11-070000-PC2.zip"));
        QCOMPARE(found[1].size, -1ll);
        QCOMPARE(found[2].name, std::string("WinShun-2026-11-10-080000-PC1.zip"));
        QCOMPARE(found[2].size, 2048ll);
        QVERIFY(cb::webdavListing("not xml <").empty());
    }

    void webdav()
    {
        FakeCloud cloud;
        QVERIFY(cloud.ok());
        const cb::Settings settings = settingsFor(cloud);
        const auto target = cb::webdavTarget(settings, true, nullptr);
        QVERIFY(target);
        std::vector<cb::Remote> found;
        // The folder is made at the first look.
        QVERIFY(!cloud.davFolder);
        QVERIFY(away([&] { return target->list(found); }).ok());
        QVERIFY(cloud.davFolder);
        QVERIFY(found.empty());

        const std::string bytes = QByteArray::fromHex("504b0304ff00").repeated(5000).toStdString();
        QVERIFY(away([&] { return target->upload("WinShun-2026-11-12-194703-PC.zip", bytes); }).ok());
        QVERIFY(away([&] { return target->upload("WinShun-2026-11-11-080000-PC.zip", "old"); }).ok());
        cloud.dav["notes.txt"] = "mine";
        QVERIFY(away([&] { return target->list(found); }).ok());
        QCOMPARE(found.size(), std::size_t(2));
        QCOMPARE(found[0].name, std::string("WinShun-2026-11-12-194703-PC.zip"));
        QCOMPARE(found[0].size, static_cast<long long>(bytes.size()));
        std::string back;
        QVERIFY(away([&] { return target->download(found[0], back); }).ok());
        QCOMPARE(back, bytes);
        QVERIFY(away([&] { return target->remove(found[1]); }).ok());
        QCOMPARE(cloud.dav.keys(), (QList<QByteArray> {"WinShun-2026-11-12-194703-PC.zip", "notes.txt"}));

        cb::Settings wrong = settings;
        wrong.webdavPassword = "guess";
        const cb::Outcome refused = away([&] { return cb::webdavTarget(wrong, true, nullptr)->list(found); });
        QVERIFY(QString::fromStdString(refused.error).contains(u"用户名或密码不对"_s));
        wrong = settings;
        wrong.webdavUrl = "dav.example.com/x";
        QVERIFY(QString::fromStdString(away([&] { return cb::webdavTarget(wrong, true, nullptr)->list(found); }).error).contains(u"https://"_s));
        wrong.webdavUrl.clear();
        QVERIFY(!cb::webdavTarget(wrong, true, nullptr));
    }

    void github()
    {
        FakeCloud cloud;
        QVERIFY(cloud.ok());
        const cb::Settings settings = settingsFor(cloud);
        const auto target = cb::githubTarget(settings, true, nullptr);
        QVERIFY(target);
        std::vector<cb::Remote> found;
        // A public repository gets nothing, whatever else is right.
        cloud.hubPrivate = false;
        const cb::Outcome open = away([&] { return target->list(found); });
        QVERIFY2(QString::fromStdString(open.error).contains(u"公开仓库"_s), open.error.c_str());
        cloud.hubPrivate = true;
        QVERIFY(away([&] { return target->list(found); }).ok()); // empty: the folder is not there yet
        QVERIFY(found.empty());

        const std::string bytes = QByteArray::fromHex("504b030400ff").repeated(5000).toStdString();
        QVERIFY(away([&] { return target->upload("WinShun-2026-11-12-194703-PC.zip", bytes); }).ok());
        QVERIFY(away([&] { return target->upload("WinShun-2026-11-11-080000.zip", "old"); }).ok());
        QCOMPARE(cloud.hub.value("WinShun-2026-11-12-194703-PC.zip"), QByteArray::fromStdString(bytes));
        QVERIFY(away([&] { return target->list(found); }).ok());
        QCOMPARE(found.size(), std::size_t(2)); // not the README beside them
        QCOMPARE(found[0].name, std::string("WinShun-2026-11-12-194703-PC.zip"));
        std::string back;
        QVERIFY(away([&] { return target->download(found[0], back); }).ok());
        QCOMPARE(back, bytes);
        QVERIFY(away([&] { return target->remove(found[1]); }).ok()); // with the sha the listing gave
        QCOMPARE(cloud.hub.size(), 1);

        cb::Settings wrong = settings;
        wrong.githubToken = "expired";
        QVERIFY(QString::fromStdString(away([&] { return cb::githubTarget(wrong, true, nullptr)->list(found); }).error).contains(u"令牌不对"_s));
        wrong = settings;
        wrong.githubRepo = "me/other";
        QVERIFY(QString::fromStdString(away([&] { return cb::githubTarget(wrong, true, nullptr)->list(found); }).error).contains(u"找不到仓库 me/other"_s));
        wrong.githubRepo = "backups";
        QVERIFY(QString::fromStdString(away([&] { return cb::githubTarget(wrong, true, nullptr)->list(found); }).error).contains(u"用户名/仓库名"_s));
        const std::string big(cb::kMaxGithub + 1, 'x');
        QVERIFY(QString::fromStdString(away([&] { return target->upload("WinShun-2026-11-12-200000.zip", big); }).error).contains(u"50 MB"_s));
        wrong.githubToken.clear();
        QVERIFY(!cb::githubTarget(wrong, true, nullptr));
    }

    // The plugin itself: a backup to both places, nothing again while nothing
    // changed, the oldest removed, and a backup taken back to Win顺.
    void plugin()
    {
        FakeCloud cloud;
        QVERIFY(cloud.ok());
        QTemporaryDir data;
        QVERIFY(data.isValid());
        Process p;
        QVERIFY(p.start(data.path(), Json::Object {{"webdavUrl", cloud.base() + "/dav/WinShun"}, {"webdavUser", "user"},
            {"webdavPassword", "secret"}, {"githubRepo", "me/backups"}, {"githubToken", "token"}, {"githubApi", cloud.base()},
            {"automatic", true}, {"keep", 2}}));

        // The rows: one for each place, then (once read) what is there.
        Json status = rowOf(p.rows(), "status:webdav");
        QVERIFY(status.isObject());
        QVERIFY(rowOf(p.rows(), "status:github").isObject());

        const auto backup = [&](const std::string& hash, const std::string& unless, bool written, const QByteArray& bytes) {
            if (!p.act(idleRow(p, "status:webdav")).has("effects"))
                return false;
            return p.tool("backup_export", [&](const Json& arguments) {
                if (arguments["unless"].toString() != unless)
                    return Json(Json::Object {{"error", Json::Object {{"code", -1}, {"message", "unless was " + arguments["unless"].toString()}}}});
                if (written) {
                    QFile file(QString::fromStdString(arguments["file"].toString()));
                    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
                        return Json(Json::Object {{"error", Json::Object {{"code", -1}, {"message", "not written"}}}});
                }
                return Json(Json::Object {{"hash", hash}, {"when", "2026-11-12T19:47:03"}, {"written", written}});
            });
        };
        const auto told = [&](const QString& text) {
            return QTest::qWaitFor([&] {
                return std::any_of(p.notices.begin(), p.notices.end(), [&](const std::string& n) { return QString::fromStdString(n).contains(text); });
            }, 15000);
        };

        QVERIFY(backup("h1", "", true, "first"));
        QVERIFY(told(u"已备份到云端"_s));
        QCOMPARE(cloud.dav.size(), 1);
        QCOMPARE(cloud.hub.size(), 1);
        QCOMPARE(cloud.dav.first(), QByteArray("first"));
        QCOMPARE(cloud.hub.first(), QByteArray("first"));
        QCOMPARE(cloud.dav.firstKey(), cloud.hub.firstKey()); // one name at both
        QVERIFY(cb::parseName(cloud.dav.firstKey().toStdString()).has_value());
        QVERIFY(!QFile::exists(data.filePath(u"upload.zip"_s)));

        // Nothing changed: Win顺 is told what both have, writes nothing, nothing is sent.
        p.notices.clear();
        QVERIFY(backup("h1", "h1", false, {}));
        QVERIFY(told(u"已经是最新"_s));
        QCOMPARE(cloud.dav.size(), 1);

        // Changed twice (in the same second, too: the names differ): two are kept.
        p.notices.clear();
        QVERIFY(backup("h2", "h1", true, "second"));
        QVERIFY(told(u"已备份到云端"_s));
        p.notices.clear();
        QVERIFY(backup("h3", "h2", true, "third"));
        QVERIFY(told(u"已备份到云端"_s));
        QCOMPARE(cloud.dav.size(), 2);
        QCOMPARE(cloud.hub.size(), 2);
        QCOMPARE(cloud.dav.values(), (QList<QByteArray> {"second", "third"}));
        QCOMPARE(cloud.hub.values(), (QList<QByteArray> {"second", "third"}));

        // Gone from one place (removed there by hand): that one gets it again.
        cloud.dav.clear();
        p.notices.clear();
        QVERIFY(backup("h3", "", true, "third"));
        QVERIFY(told(u"已备份到云端"_s));
        QCOMPARE(cloud.dav.values(), (QList<QByteArray> {"third"}));
        QCOMPARE(cloud.hub.size(), 2);

        // The rows of the backups; Enter asks first, and only then restores.
        QVERIFY(idleRow(p, "status:github").isObject());
        const Json rows = p.rows();
        const Json newest = rowOf(rows, "backup:github:");
        QVERIFY(newest.isObject());
        const Json asked = p.act(newest);
        const Json confirm = rowOf(asked["effects"].toArray().at(0)["items"], "confirm");
        QVERIFY(confirm.isObject());
        QVERIFY(p.tools.empty());
        QVERIFY(p.act(confirm).has("effects"));
        QByteArray handed;
        QVERIFY(p.tool("backup_restore", [&](const Json& arguments) {
            QFile file(QString::fromStdString(arguments["file"].toString()));
            if (file.open(QIODevice::ReadOnly))
                handed = file.readAll();
            return Json(Json::Object {{"error", Json::Object {{"code", -32001}, {"message", "这不是 Win顺 的备份"}}}});
        }));
        QCOMPARE(handed, cloud.hub.value(QByteArray::fromStdString(newest["data"]["name"].toString())));
        QVERIFY(told(u"这不是 Win顺 的备份"_s));
        QTRY_VERIFY(!QFile::exists(data.filePath(u"restore.zip"_s)));

        // What was uploaded is known after a restart: nothing is sent again.
        QVERIFY(p.call("shutdown", Json::Object {}).isObject());
        QTRY_COMPARE(p.process.state(), QProcess::NotRunning); // gone by itself
        Process again;
        QVERIFY(again.start(data.path(), Json::Object {{"githubRepo", "me/backups"}, {"githubToken", "token"}, {"githubApi", cloud.base()}}));
        QVERIFY(!rowOf(again.rows(), "status:webdav").isObject()); // not set: not shown
        QVERIFY(again.act(idleRow(again, "status:github")).has("effects"));
        QVERIFY(again.tool("backup_export", [&](const Json& arguments) {
            return Json(Json::Object {{"hash", arguments["unless"].toString()}, {"when", ""}, {"written", false}});
        }));
        QVERIFY(QTest::qWaitFor([&] { return !again.notices.empty(); }, 15000));
        QVERIFY2(QString::fromStdString(again.notices.back()).contains(u"已经是最新"_s), again.notices.back().c_str());
        QCOMPARE(cloud.hub.size(), 2);

        // A public repository: told, and nothing uploaded.
        cloud.hubPrivate = false;
        again.notices.clear();
        QVERIFY(again.act(idleRow(again, "status:github")).has("effects"));
        QVERIFY(QTest::qWaitFor([&] { return !again.notices.empty(); }, 15000));
        QVERIFY2(QString::fromStdString(again.notices.back()).contains(u"公开仓库"_s), again.notices.back().c_str());
        QVERIFY(again.tools.empty());

        // Told to stop while the worker waits for Win顺's answer: it ends in time.
        cloud.hubPrivate = true;
        QVERIFY(again.act(idleRow(again, "status:github")).has("effects"));
        QVERIFY(QTest::qWaitFor([&] { return !again.tools.empty(); }, 15000));
        QVERIFY(again.call("shutdown", Json::Object {}).isObject());
        QTRY_COMPARE(again.process.state(), QProcess::NotRunning);
    }

    void notSetUp()
    {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        Process p;
        QVERIFY(p.start(data.path(), Json::Object {}));
        const Json rows = p.rows();
        QCOMPARE(rows.toArray().size(), std::size_t(1));
        QCOMPARE(rows.toArray().at(0)["open"]["url"].toString(), std::string("winshun:settings"));
        QVERIFY(p.call("shutdown", Json::Object {}).isObject());
        QTRY_COMPARE(p.process.state(), QProcess::NotRunning); // gone by itself
    }
};

QTEST_GUILESS_MAIN(TestCloudBackup)
#include "tst_cloudbackup.moc"
