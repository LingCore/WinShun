// The MCP server (src/mcp): JSON-RPC framing, the tools on a hand-made index
// over a temporary folder, the lifecycle through an in-process session,
// the named pipe, and the relay WinShunMcp.exe driven over stdio, with
// Win顺 there and away; and the plugins' commands as tools (plugin::AiBridge).

#include "AiBridge.h"
#include "ContentScanner.h"
#include "FileIndex.h"
#include "Host.h"
#include "IndexService.h"
#include "JsonRpc.h"
#include "McpServer.h"
#include "McpTools.h"
#include "Pipe.h"
#include "Sandbox.h"
#include "SearchEngine.h"
#include "Visibility.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ws;
using namespace ws::mcp;
using namespace Qt::StringLiterals;

namespace {

QJsonObject parseObject(const QByteArray& line)
{
    return QJsonDocument::fromJson(line).object();
}

QByteArray requestLine(int id, const QString& method, const QJsonObject& params = {})
{
    return jsonrpc::request(id, method, params);
}

QByteArray callLine(int id, const QString& tool, const QJsonObject& arguments = {})
{
    return jsonrpc::request(id, u"tools/call"_s, {{u"name"_s, tool}, {u"arguments"_s, arguments}});
}

// The structured result of a tools/call response, or the error text.
struct Outcome {
    bool isError = false;
    QString text;
    QJsonObject structured;
    QJsonObject error; // a protocol error
};

Outcome outcomeOf(const QByteArray& line)
{
    Outcome out;
    const QJsonObject message = parseObject(line);
    if (message.contains(u"error"_s)) {
        out.isError = true;
        out.error = message.value(u"error"_s).toObject();
        out.text = out.error.value(u"message"_s).toString();
        return out;
    }
    const QJsonObject result = message.value(u"result"_s).toObject();
    out.isError = result.value(u"isError"_s).toBool();
    const QJsonArray content = result.value(u"content"_s).toArray();
    if (!content.isEmpty())
        out.text = content.first().toObject().value(u"text"_s).toString();
    out.structured = result.value(u"structuredContent"_s).toObject();
    return out;
}

QStringList names(const QJsonObject& structured)
{
    QStringList out;
    for (const QJsonValue& item : structured.value(u"items"_s).toArray())
        out.push_back(item.toObject().value(u"name"_s).toString());
    return out;
}

// A folder on disk and the same in a hand-made index: the service is not
// started (it reads no volume), its index is filled by hand to mirror the
// folder, and the engine, host and server run on it as in the app.
struct Fixture {
    QTemporaryDir dir;
    QString root; // the folder, native
    std::unique_ptr<IndexService> service;
    std::unique_ptr<SearchEngine> engine;
    std::unique_ptr<ToolHost> host;
    std::unique_ptr<McpServer> server;
    EntryId folder = kNoEntry;
    QStringList opened; // what the fake shell opened
    QStringList log;
    std::vector<ClipSummary> clips;
    std::vector<ScreenshotSummary> shots; // the screenshot history, as the app's would list it
    // take_screenshot: as the user would answer (App::confirmScreenshot); unset: never answers.
    std::function<void(const ScreenshotRequest&, std::function<void(ScreenshotTaken)>)> asker;
    QString tempFolder; // where screenshot_history copies the pictures

    // The settings tools, with a handful of settings of their own (the app's
    // catalog is tst_aisettings'): one that asks always, one a plain switch.
    QMap<QString, QJsonValue> settingValues {{u"Launcher/DoubleCtrl"_s, true}, {u"Results/Sort"_s, u"match"_s},
        {u"Launcher/Autostart"_s, false}};
    // As the user would answer the card (App::confirmSettingsChange); unset: never answers.
    std::function<void(const SettingsAsk&, std::function<void(SettingsAnswer, bool)>)> settingsAsker;
    int settingsAsks = 0;
    int settingsCancels = 0;
    SettingsAsk lastAsk;
    QList<SettingsApply> settingsApplied;
    SettingsQuery lastSettingsQuery;
    QString openedAt; // "key|section"

    // `shares`: network locations, as configured (the service is not started:
    // none is listed or asked anything).
    explicit Fixture(bool withShell = true, bool withClipboard = true, std::vector<ShareConfig> shares = {})
    {
        root = QDir::toNativeSeparators(dir.path());
        IndexService::Options options;
        options.content.enabled = false;
        options.shares.shares = std::move(shares);
        service = std::make_unique<IndexService>(dir.path() + u"/index"_s, options);
        folder = addFolder(root);
        engine = std::make_unique<SearchEngine>(service.get());
        Services services;
        services.index = service.get();
        services.engine = engine.get();
        services.version = u"0.0-test"_s;
        services.log = [this](const QString& line) { log.push_back(line); };
        if (withShell) {
            services.open = [this](const QString& path, std::function<void(bool)> done) {
                opened.push_back(u"open "_s + path);
                done(true);
            };
            services.reveal = [this](const QString& path, std::function<void(bool)> done) {
                opened.push_back(u"reveal "_s + path);
                done(true);
            };
        }
        if (withClipboard) {
            services.clipboard = [this](const ClipQuery& query) {
                std::vector<ClipSummary> out;
                for (const ClipSummary& c : clips) {
                    if ((query.kind == u"all" || query.kind == c.kind) && (query.query.isEmpty() || c.text.contains(query.query))
                        && std::ssize(out) < query.limit) {
                        ClipSummary cut = c;
                        cut.text = c.text.left(query.maxChars);
                        out.push_back(cut);
                    }
                }
                return out;
            };
            services.clipboardText = [this](qint64 id) -> std::optional<QString> {
                for (const ClipSummary& c : clips) {
                    if (c.id == id)
                        return c.text;
                }
                return std::nullopt;
            };
            services.screenshots = [this](const QString& query, int limit) {
                std::vector<ScreenshotSummary> out;
                for (const ScreenshotSummary& shot : shots) {
                    if ((query.isEmpty() || shot.ocr.contains(query)) && std::ssize(out) < limit)
                        out.push_back(shot);
                }
                return out;
            };
            services.takeScreenshot = [this](const ScreenshotRequest& request, std::function<void(ScreenshotTaken)> done) {
                if (asker)
                    asker(request, std::move(done));
            };
        }
        services.getSettings = [this](const SettingsQuery& query) {
            lastSettingsQuery = query;
            QJsonArray list;
            for (auto it = settingValues.cbegin(); it != settingValues.cend(); ++it)
                list.push_back(QJsonObject {{u"key"_s, it.key()}, {u"value"_s, it.value()}});
            return ToolResult {{{u"settings"_s, list}, {u"sections"_s, QJsonArray()}}};
        };
        services.planSettings = [this](const QJsonArray& changes) {
            SettingsPlan plan;
            for (const QJsonValue& v : changes) {
                const QJsonObject change = v.toObject();
                const QString key = change.value(u"key"_s).toString();
                if (!settingValues.contains(key)) {
                    plan.problems.push_back(u"unknown setting '%1'"_s.arg(key));
                    continue;
                }
                const QJsonValue value = change.value(u"value"_s);
                if (value.type() != settingValues.value(key).type()) {
                    plan.problems.push_back(u"%1: wrong type"_s.arg(key));
                    continue;
                }
                plan.items.push_back({key, settingValues.value(key) != value, key == u"Launcher/Autostart", value,
                    QJsonObject {{u"key"_s, key}, {u"title"_s, key}, {u"old"_s, settingValues.value(key)}, {u"new"_s, value}}});
            }
            return plan;
        };
        services.confirmSettings = [this](const SettingsAsk& ask, std::function<void(SettingsAnswer, bool)> done) {
            ++settingsAsks;
            lastAsk = ask;
            if (settingsAsker)
                settingsAsker(ask, std::move(done));
        };
        services.cancelSettings = [this] { ++settingsCancels; };
        services.applySettings = [this](const SettingsApply& apply, std::function<void(ToolResult)> done) {
            settingsApplied.push_back(apply);
            for (const QJsonValue& v : apply.changes)
                settingValues[v.toObject().value(u"key"_s).toString()] = v.toObject().value(u"value"_s);
            done({{{u"changed"_s, static_cast<int>(apply.changes.size())}}});
        };
        services.openSettings = [this](const QString& key, const QString& section) {
            openedAt = key + u'|' + section;
            return ToolResult {{{u"opened"_s, true}, {u"where"_s, openedAt}}};
        };
        tempFolder = dir.path() + u"/mcp-temp"_s;
        services.tempFolder = tempFolder;
        host = std::make_unique<ToolHost>(std::move(services));
        Policy policy;
        policy.contentExtensions = {u"txt"_s, u"md"_s};
        policy.contentDocuments = false;
        host->setPolicy(policy);
        server = std::make_unique<McpServer>(host.get(), u"0.0-test"_s);
    }

    // The entry of a folder on disk, every folder on the way added.
    EntryId addFolder(const QString& path)
    {
        const auto index = service->index();
        const auto lock = index->writeLock();
        const QStringList parts = path.split(u'\\', Qt::SkipEmptyParts);
        EntryId at = kNoEntry;
        for (const QString& part : parts) {
            const std::string name = part.toStdString();
            if (at == kNoEntry) {
                bool found = false;
                for (const EntryId r : index->roots()) {
                    if (QString::fromStdString(std::string(index->name(r))).compare(part, Qt::CaseInsensitive) == 0) {
                        at = r;
                        found = true;
                    }
                }
                if (!found)
                    at = index->addRoot(name);
                continue;
            }
            EntryId child = index->findChild(at, name);
            if (child == kNoEntry)
                child = index->add(at, name, EntryFlag::Directory);
            at = child;
        }
        return at;
    }

    // A file written to disk and put in the index, `modified` seconds since
    // 1970 (the index's; the disk keeps now).
    QString write(const QString& relative, const QByteArray& content, FileTime modified, std::uint8_t flags = 0)
    {
        const QString path = root + u"\\"_s + QDir::toNativeSeparators(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return {};
        file.write(content);
        file.close();
        const EntryId parent = addFolder(QFileInfo(path).absolutePath().replace(u'/', u'\\'));
        const auto index = service->index();
        const auto lock = index->writeLock();
        const EntryId id = index->add(parent, QFileInfo(path).fileName().toStdString(), flags);
        index->setModified(id, modified);
        return path;
    }

    EntryId subfolder(const QString& relative, FileTime modified)
    {
        const QString path = root + u"\\"_s + QDir::toNativeSeparators(relative);
        QDir().mkpath(path);
        const EntryId id = addFolder(path);
        const auto index = service->index();
        const auto lock = index->writeLock();
        index->setModified(id, modified);
        return id;
    }

    // Gives a folder (and what is in it) a DACL for administrators and the
    // system alone: this process, the owner, may still read and change its
    // security, but not list or read it (an elevated test process checks
    // with its linked token, which cannot either). Undone by the destructor.
    QString restricted;
    bool restrict(const QString& relative)
    {
        const QString path = root + u"\\"_s + QDir::toNativeSeparators(relative);
        if (!setDacl(path, L"D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", PROTECTED_DACL_SECURITY_INFORMATION))
            return false;
        restricted = path;
        return true;
    }
    void unrestrict()
    {
        if (!restricted.isEmpty())
            setDacl(restricted, L"D:(A;OICI;FA;;;WD)", UNPROTECTED_DACL_SECURITY_INFORMATION);
        restricted.clear();
    }
    ~Fixture() { unrestrict(); }

    static bool setDacl(const QString& path, const wchar_t* sddl, SECURITY_INFORMATION protection)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr))
            return false;
        PACL dacl = nullptr;
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        ::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
        const DWORD error = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(path.toStdWString().c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | protection, nullptr, nullptr, dacl, nullptr);
        ::LocalFree(descriptor);
        return error == ERROR_SUCCESS;
    }
};

// An in-process session: what the server sends is collected.
struct Session {
    McpServer* server;
    McpServer::SessionId id;
    QList<QByteArray> received;

    explicit Session(McpServer* s)
        : server(s)
    {
        id = server->openSession([this](const QByteArray& line) { received.push_back(line); });
    }
    ~Session() { server->closeSession(id); }

    // Sends a line and waits for the next line back.
    std::optional<QByteArray> exchange(const QByteArray& line, int timeoutMs = 5000)
    {
        const qsizetype before = received.size();
        server->receive(id, line);
        QElapsedTimer timer;
        timer.start();
        while (received.size() <= before && timer.elapsed() < timeoutMs)
            QTest::qWait(5);
        if (received.size() <= before)
            return std::nullopt;
        return received.at(before);
    }

    bool initialize(const QString& version = u"2025-11-25"_s, const QString& client = u"tst_mcp"_s)
    {
        const auto reply = exchange(requestLine(1, u"initialize"_s,
            {{u"protocolVersion"_s, version}, {u"capabilities"_s, QJsonObject()},
                {u"clientInfo"_s, QJsonObject {{u"name"_s, client}, {u"version"_s, u"1.0"_s}}}}));
        if (!reply)
            return false;
        server->receive(id, jsonrpc::notification(u"notifications/initialized"_s));
        return parseObject(*reply).value(u"result"_s).toObject().value(u"protocolVersion"_s).toString() == version;
    }
};

// The relay, driven over its stdio as an AI client drives it: started with
// pipes for its stdin and stdout (Win32, as the clients do; its stderr is
// the test's), lines written, lines read on a thread of their own.
struct Relay {
    HANDLE process = nullptr;
    HANDLE toRelay = nullptr; // its stdin
    HANDLE fromRelay = nullptr; // its stdout
    std::thread reader;
    std::mutex mutex;
    QByteArray pending;
    bool eof = false;
    DWORD exitCode = 0xFFFF'FFFF;

    ~Relay()
    {
        if (toRelay)
            ::CloseHandle(toRelay);
        if (process) {
            if (::WaitForSingleObject(process, 2000) != WAIT_OBJECT_0)
                ::TerminateProcess(process, 9);
            ::CloseHandle(process);
        }
        if (reader.joinable())
            reader.join();
        if (fromRelay)
            ::CloseHandle(fromRelay);
    }

    bool start(const std::wstring& pipeName)
    {
        SECURITY_ATTRIBUTES inheritable {sizeof inheritable, nullptr, TRUE};
        HANDLE inRead = nullptr;
        HANDLE outWrite = nullptr;
        if (!::CreatePipe(&inRead, &toRelay, &inheritable, 0) || !::CreatePipe(&fromRelay, &outWrite, &inheritable, 0))
            return false;
        ::SetHandleInformation(toRelay, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(fromRelay, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inRead;
        startup.hStdOutput = outWrite;
        startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
        std::wstring command = L"\"" + QString::fromUtf8(WINSHUN_MCP_RELAY).toStdWString() + L"\" --pipe " + pipeName;
        PROCESS_INFORMATION info {};
        const bool started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &info);
        ::CloseHandle(inRead);
        ::CloseHandle(outWrite);
        if (!started)
            return false;
        ::CloseHandle(info.hThread);
        process = info.hProcess;
        reader = std::thread([this] {
            char chunk[64 * 1024];
            DWORD got = 0;
            while (::ReadFile(fromRelay, chunk, sizeof chunk, &got, nullptr) && got > 0) {
                const std::lock_guard lock(mutex);
                pending.append(chunk, static_cast<qsizetype>(got));
            }
            const std::lock_guard lock(mutex);
            eof = true;
        });
        return true;
    }

    void send(const QByteArray& line)
    {
        DWORD put = 0;
        ::WriteFile(toRelay, line.constData(), static_cast<DWORD>(line.size()), &put, nullptr);
    }

    // The event loop runs meanwhile: the server (on this thread) must get
    // to answer the relay.
    std::optional<QByteArray> readLine(int timeoutMs = 10000)
    {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            {
                const std::lock_guard lock(mutex);
                if (const qsizetype at = pending.indexOf('\n'); at >= 0) {
                    QByteArray line = pending.left(at);
                    pending.remove(0, at + 1);
                    return line;
                }
                if (eof)
                    return std::nullopt;
            }
            if (timer.elapsed() >= timeoutMs)
                return std::nullopt;
            QTest::qWait(5);
        }
    }

    std::optional<QByteArray> exchange(const QByteArray& line, int timeoutMs = 10000)
    {
        send(line);
        return readLine(timeoutMs);
    }

    // Closes its stdin, as a client shutting down does, and waits for it to exit.
    void finish()
    {
        ::CloseHandle(toRelay);
        toRelay = nullptr;
        if (::WaitForSingleObject(process, 5000) == WAIT_OBJECT_0)
            ::GetExitCodeProcess(process, &exitCode);
    }
};

std::wstring testPipeName(const char* tag)
{
    return L"WinShun.Mcp.test-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::wstring(tag, tag + std::strlen(tag));
}

// This test runs as administrator (ctest started from an elevated shell).
bool elevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const bool result = plugin::sandbox::tokenElevated(token);
    ::CloseHandle(token);
    return result;
}

} // namespace

class TestMcp : public QObject {
    Q_OBJECT

private slots:
    void jsonRpcParse()
    {
        QString problem;
        auto m = jsonrpc::parse(R"({"jsonrpc":"2.0","id":1,"method":"ping"})", &problem);
        QVERIFY(m);
        QVERIFY(m->isRequest());
        QCOMPARE(m->method, u"ping"_s);
        QCOMPARE(m->id.toInt(), 1);

        m = jsonrpc::parse(R"({"jsonrpc":"2.0","id":"abc","method":"tools/call","params":{"name":"x"}})");
        QVERIFY(m && m->isRequest());
        QCOMPARE(m->params.value(u"name"_s).toString(), u"x"_s);
        QCOMPARE(jsonrpc::idKey(m->id), u"\"abc\""_s);

        m = jsonrpc::parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        QVERIFY(m && m->isNotification());

        m = jsonrpc::parse(R"({"jsonrpc":"2.0","id":3,"result":{"ok":true}})");
        QVERIFY(m && m->isResponse() && !m->failed);
        m = jsonrpc::parse(R"({"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"no"}})");
        QVERIFY(m && m->isResponse() && m->failed);
        QCOMPARE(m->error.value(u"code"_s).toInt(), -32601);

        QVERIFY(!jsonrpc::parse("not json", &problem));
        QVERIFY(problem.contains(u"JSON"));
        QVERIFY(!jsonrpc::parse(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])", &problem)); // no batches
        QVERIFY(!jsonrpc::parse(R"({"id":1,"method":"ping"})")); // no version
        QVERIFY(!jsonrpc::parse(R"({"jsonrpc":"2.0","id":{"a":1},"method":"ping"})")); // id must be a string or a number
        QVERIFY(!jsonrpc::parse(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":[1]})")); // params an object

        const QByteArray line = jsonrpc::errorResponse(7, jsonrpc::kMethodNotFound, u"Method not found: x"_s);
        QVERIFY(line.endsWith('\n'));
        QVERIFY(!line.chopped(1).contains('\n'));
        const QJsonObject error = parseObject(line).value(u"error"_s).toObject();
        QCOMPARE(error.value(u"code"_s).toInt(), -32601);
    }

    void toolsOnOffer()
    {
        Fixture f;
        QStringList expected {u"search_files"_s, u"search_content"_s, u"recent_files"_s, u"list_folder"_s,
            u"get_file_info"_s, u"get_settings"_s, u"open_settings"_s, u"index_status"_s, u"read_file_text"_s};
        QCOMPARE(f.host->toolNames(), expected);
        QCOMPARE(f.host->known(u"change_settings"_s), ToolHost::Known::Off); // until the user allows it
        QCOMPARE(f.host->known(u"open_item"_s), ToolHost::Known::Off);
        QCOMPARE(f.host->known(u"nothing"_s), ToolHost::Known::No);
        QCOMPARE(f.host->known(u"search_files"_s), ToolHost::Known::Yes);

        QSignalSpy changed(f.host.get(), &ToolHost::toolsChanged);
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        policy.allowClipboard = true;
        f.host->setPolicy(policy);
        QCOMPARE(changed.count(), 1);
        QVERIFY(f.host->toolNames().contains(u"open_item"_s));
        QVERIFY(f.host->toolNames().contains(u"show_in_folder"_s));
        QVERIFY(f.host->toolNames().contains(u"clipboard_history"_s));
        QVERIFY(f.host->toolNames().contains(u"clipboard_get"_s));
        f.host->setPolicy(policy); // the same again: no change
        QCOMPARE(changed.count(), 1);
        QVERIFY(!f.host->toolNames().contains(u"change_settings"_s));
        policy.settingsMode = SettingsMode::Ask;
        f.host->setPolicy(policy);
        QVERIFY(f.host->toolNames().contains(u"change_settings"_s));
        policy.settingsMode = SettingsMode::Off;
        f.host->setPolicy(policy);
        QVERIFY(!f.host->toolNames().contains(u"change_settings"_s));

        // Every tool has what MCP wants of it.
        for (const QJsonValue& v : f.host->tools()) {
            const QJsonObject tool = v.toObject();
            QVERIFY2(!tool.value(u"name"_s).toString().isEmpty(), "name");
            QVERIFY2(!tool.value(u"description"_s).toString().isEmpty(), "description");
            QCOMPARE(tool.value(u"inputSchema"_s).toObject().value(u"type"_s).toString(), u"object"_s);
            QVERIFY(tool.value(u"annotations"_s).toObject().contains(u"readOnlyHint"_s));
        }

        // Without a shell or a clipboard (wsbench), their tools are not offered even when allowed.
        Fixture bare(false, false);
        policy.settingsMode = SettingsMode::Auto;
        bare.host->setPolicy(policy);
        QVERIFY(!bare.host->toolNames().contains(u"open_item"_s));
        QVERIFY(!bare.host->toolNames().contains(u"clipboard_history"_s));
    }

    void lifecycle()
    {
        Fixture f;
        Session s(f.server.get());
        // Before initialize: ping is fine, nothing else.
        auto reply = s.exchange(requestLine(10, u"ping"_s));
        QVERIFY(reply);
        QVERIFY(parseObject(*reply).contains(u"result"_s));
        reply = s.exchange(requestLine(11, u"tools/list"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kUnavailable);

        // An older revision is echoed; an unknown one gets the newest.
        QVERIFY(s.initialize(u"2025-06-18"_s));
        reply = s.exchange(requestLine(1, u"initialize"_s, {{u"protocolVersion"_s, u"1999-01-01"_s}}));
        QVERIFY(reply);
        const QJsonObject result = parseObject(*reply).value(u"result"_s).toObject();
        QCOMPARE(result.value(u"protocolVersion"_s).toString(), u"2025-11-25"_s);
        QCOMPARE(result.value(u"serverInfo"_s).toObject().value(u"name"_s).toString(), u"winshun"_s);
        QVERIFY(result.value(u"capabilities"_s).toObject().value(u"tools"_s).toObject().value(u"listChanged"_s).toBool());
        QVERIFY(result.value(u"capabilities"_s).toObject().contains(u"prompts"_s));
        QVERIFY(result.value(u"instructions"_s).toString().contains(u"pinyin"));

        reply = s.exchange(requestLine(12, u"tools/list"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray().size(), 9);
        reply = s.exchange(requestLine(13, u"prompts/list"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"result"_s).toObject().value(u"prompts"_s).toArray().size(), 1);
        reply = s.exchange(requestLine(14, u"nothing/here"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kMethodNotFound);
        reply = s.exchange("{not json");
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kInvalidRequest);
        QVERIFY(parseObject(*reply).value(u"id"_s).isNull());

        // Unknown tool: a protocol error; wrong arguments: a tool error.
        reply = s.exchange(callLine(15, u"no_such_tool"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kInvalidParams);
        reply = s.exchange(callLine(16, u"search_files"_s, {{u"limit"_s, 5}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"query"), qPrintable(out.text));
        reply = s.exchange(callLine(17, u"search_files"_s, {{u"query"_s, u"x"_s}, {u"kind"_s, u"sideways"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        // A tool its switch keeps off: a tool error saying so.
        reply = s.exchange(callLine(18, u"open_item"_s, {{u"path"_s, u"C:\\"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"turned off"), qPrintable(out.text));

        // The session is counted, with the client's name.
        const auto sessions = f.server->sessions();
        QCOMPARE(sessions.size(), std::size_t {1});
        QCOMPARE(sessions.front().client, u"tst_mcp"_s);
        QCOMPARE(sessions.front().calls, 2); // the two that reached a tool
    }

    void searchFiles()
    {
        Fixture f;
        f.write(u"年度报告.docx"_s, "x", 1'700'000'000);
        f.write(u"报告草稿.txt"_s, "draft", 1'700'000'100);
        f.write(u"notes.md"_s, "notes", 1'700'000'200);
        f.write(u"项目\\readme.md"_s, "readme", 1'700'000'300);
        f.subfolder(u"报告"_s, 1'700'000'400);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        auto reply = s.exchange(callLine(2, u"search_files"_s, {{u"query"_s, u"报告"_s}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QStringList found = names(out.structured);
        QVERIFY2(found.contains(u"年度报告.docx"_s) && found.contains(u"报告草稿.txt"_s) && found.contains(u"报告"_s), qPrintable(found.join(u", "_s)));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 3);
        QVERIFY(!out.structured.value(u"truncated"_s).toBool());
        // The text block is the same JSON.
        QCOMPARE(parseObject(out.text.toUtf8()).value(u"returned"_s).toInt(), 3);
        // Items carry a path, a type, the time from the index and the size from the disk.
        const QJsonObject first = out.structured.value(u"items"_s).toArray().first().toObject();
        QVERIFY(first.value(u"path"_s).toString().startsWith(f.root));
        QVERIFY2(first.value(u"modified"_s).toString().startsWith(u"2023-11-1"), qPrintable(first.value(u"modified"_s).toString()));
        for (const QJsonValue& v : out.structured.value(u"items"_s).toArray()) {
            const QJsonObject item = v.toObject();
            if (item.value(u"type"_s).toString() == u"file")
                QVERIFY(item.value(u"size"_s).toInt() > 0);
        }

        // Pinyin, folders only, files only, extensions, a limit.
        reply = s.exchange(callLine(3, u"search_files"_s, {{u"query"_s, u"ndbg"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"年度报告.docx"_s});
        reply = s.exchange(callLine(4, u"search_files"_s, {{u"query"_s, u"报告"_s}, {u"kind"_s, u"folders"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"报告"_s});
        reply = s.exchange(callLine(5, u"search_files"_s, {{u"query"_s, u"报告"_s}, {u"kind"_s, u"files"_s}}));
        QVERIFY(reply);
        found = names(outcomeOf(*reply).structured);
        QCOMPARE(found.size(), 2);
        QVERIFY(!found.contains(u"报告"_s));
        reply = s.exchange(callLine(6, u"search_files"_s, {{u"query"_s, u"ext:md"_s}}));
        QVERIFY(reply);
        found = names(outcomeOf(*reply).structured);
        QVERIFY(found.contains(u"notes.md"_s) && found.contains(u"readme.md"_s));
        reply = s.exchange(callLine(7, u"search_files"_s, {{u"query"_s, u"报告"_s}, {u"limit"_s, 1}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 1);
        QVERIFY(out.structured.value(u"truncated"_s).toBool());
        // Newest first.
        reply = s.exchange(callLine(8, u"search_files"_s, {{u"query"_s, u"报告"_s}, {u"sort"_s, u"modified"_s}, {u"kind"_s, u"files"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), (QStringList {u"报告草稿.txt"_s, u"年度报告.docx"_s}));
        // A folder AI must not see.
        Policy policy = f.host->policy();
        policy.excludedPaths = {f.root + u"\\项目"_s};
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(9, u"search_files"_s, {{u"query"_s, u"ext:md"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"notes.md"_s});
        reply = s.exchange(callLine(10, u"list_folder"_s, {{u"path"_s, f.root + u"\\项目"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        QVERIFY(f.log.size() >= 9);
        QVERIFY2(f.log.last().contains(u"tst_mcp list_folder"), qPrintable(f.log.last()));
    }

    void searchContent()
    {
        Fixture f;
        f.write(u"销售.txt"_s, QByteArrayLiteral("第一行\n第三季度销售额 123 万\n第三行"), 1'700'000'000);
        f.write(u"other.txt"_s, "nothing here", 1'700'000'000);
        f.write(u"code.md"_s, "x\nconst auto lock = index->readLock();\n", 1'700'000'000);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        auto reply = s.exchange(callLine(2, u"search_content"_s, {{u"query"_s, u"第三季度"_s}}), 20000);
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"销售.txt"_s});
        const QJsonObject item = out.structured.value(u"items"_s).toArray().first().toObject();
        QCOMPARE(item.value(u"location"_s).toObject().value(u"line"_s).toInt(), 2);
        QVERIFY(item.value(u"snippet"_s).toString().contains(u"第三季度销售额"));
        QVERIFY(out.structured.value(u"complete"_s).toBool());

        reply = s.exchange(callLine(3, u"search_content"_s, {{u"query"_s, u"const auto lock = index->readLock();"_s}}), 20000);
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"code.md"_s});
        reply = s.exchange(callLine(4, u"search_content"_s, {{u"query"_s, u"zzqqxx"_s}}), 20000);
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 0);

        // Content search turned off in the settings: no file is read for AI either.
        Policy policy = f.host->policy();
        policy.content = false;
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(5, u"search_content"_s, {{u"query"_s, u"第三季度"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"turned off"), qPrintable(out.text));
    }

    // Content search in some folders: named among the words ("第三季度 abc/")
    // or given (folders), every folder so named, system folders only when
    // nothing else is so named, and nowhere else.
    void searchContentInFolders()
    {
        Fixture f;
        const QByteArray text = QByteArrayLiteral("第三季度销售额 123 万");
        f.write(u"项目\\abc\\报告.txt"_s, text, 1'700'000'000);
        f.write(u"备份\\abc\\报告.txt"_s, text, 1'700'000'000);
        f.write(u"其他\\报告.txt"_s, text, 1'700'000'000);
        f.write(u"其他\\abcdef\\报告.txt"_s, text, 1'700'000'000);
        f.subfolder(u"tools\\abc"_s, 1'700'000'000);
        f.subfolder(u"tools\\only"_s, 1'700'000'000);
        {
            const auto index = f.service->index();
            const auto lock = index->writeLock();
            for (const QString& folder : {u"tools"_s, u"tools\\abc"_s, u"tools\\only"_s})
                index->setFlags(index->findPath((f.root + u'\\' + folder).toStdWString()),
                    static_cast<std::uint8_t>(EntryFlag::Directory | EntryFlag::LowPriority));
        }
        f.write(u"tools\\abc\\报告.txt"_s, text, 1'700'000'000, EntryFlag::LowPriority);
        f.write(u"tools\\only\\报告.txt"_s, text, 1'700'000'000, EntryFlag::LowPriority);
        const QString sep = u"\\"_s;
        const auto in = [&](const QString& relative) { return f.root + sep + relative + sep + u"报告.txt"_s; };

        // Straight to the engine: where it says it looks, and what it finds.
        const auto search = [&](SearchEngine::Request request) {
            request.scope = Scope::Content;
            request.contentExtensions = {u"txt"_s};
            request.contentDocuments = false;
            QSignalSpy scopes(f.engine.get(), &SearchEngine::contentScope);
            QSignalSpy results(f.engine.get(), &SearchEngine::contentResults);
            QSignalSpy progress(f.engine.get(), &SearchEngine::contentProgress);
            const quint64 id = f.engine->submit(std::move(request));
            const auto finished = [&] {
                return std::ranges::any_of(progress, [&](const QList<QVariant>& p) { return p[0].toULongLong() == id && p[3].toBool(); });
            };
            if (!QTest::qWaitFor(finished, 20000))
                return std::pair {ContentScope {}, QStringList {u"(timed out)"_s}};
            QStringList paths;
            for (const QList<QVariant>& r : results) {
                for (const SearchResult& hit : r[1].value<SearchResults>())
                    paths.append(hit.path);
            }
            paths.sort();
            return std::pair {scopes.isEmpty() ? ContentScope {} : scopes.last()[1].value<ContentScope>(), paths};
        };
        const auto sorted = [](QStringList list) {
            list.sort();
            return list;
        };

        SearchEngine::Request request;
        request.text = u"第三季度 abc/"_s;
        auto [scope, found] = search(request);
        QCOMPARE(found, sorted({in(u"项目\\abc"_s), in(u"备份\\abc"_s)})); // not tools\abc: a program folder
        QCOMPARE(scope.words, u"第三季度"_s);
        QCOMPARE(scope.places.size(), std::size_t {1});
        QCOMPARE(scope.places[0].typed, u"abc/"_s);
        QCOMPARE(scope.places[0].status, ContentScope::Status::Searched);
        QCOMPARE(scope.places[0].count, 2);
        QVERIFY(!scope.places[0].approximate);
        // One of them picked.
        request.contentPick = {u"abc/"_s, scope.places[0].folders[1]};
        std::tie(scope, found) = search(request);
        QCOMPARE(found, QStringList {scope.places[0].folders[1] + u"\\报告.txt"_s});
        QCOMPARE(scope.places[0].chosen, request.contentPick.folder);
        QCOMPARE(scope.places[0].count, 2); // still told of both, for the chips

        // A folder in a folder; "/name" last; only program folders so named: those.
        request = {};
        request.text = u"第三季度 项目/abc/"_s;
        QCOMPARE(search(request).second, QStringList {in(u"项目\\abc"_s)});
        request.text = u"第三季度 /only"_s;
        QCOMPARE(search(request).second, QStringList {in(u"tools\\only"_s)});
        // None named just so: those whose names hold it.
        request.text = u"第三季度 abcd/"_s;
        std::tie(scope, found) = search(request);
        QCOMPARE(found, QStringList {in(u"其他\\abcdef"_s)});
        QVERIFY(scope.places[0].approximate);
        // None at all: it is something to find ("/nosuch" is not in the file).
        request.text = u"第三季度 /nosuch"_s;
        std::tie(scope, found) = search(request);
        QVERIFY(found.isEmpty());
        QCOMPARE(scope.places[0].status, ContentScope::Status::Words);
        QCOMPARE(scope.words, u"第三季度 /nosuch"_s);
        // ... and searched for as such, everywhere: a file that has it is found.
        f.write(u"路由\\接口.txt"_s, QByteArrayLiteral("第三季度 GET nosuchfolder/ 200"), 1'700'000'000);
        request.text = u"第三季度 nosuchfolder/"_s;
        std::tie(scope, found) = search(request);
        QCOMPARE(found, QStringList {f.root + u"\\路由\\接口.txt"_s});
        QCOMPARE(scope.places[0].status, ContentScope::Status::Words);
        // A path: that folder alone; one not in the index: nowhere.
        request.text = f.root + u"\\其他\\ 第三季度"_s;
        QCOMPARE(search(request).second, sorted({in(u"其他"_s), in(u"其他\\abcdef"_s)}));
        request.text = f.root + u"\\不在\\ 第三季度"_s;
        std::tie(scope, found) = search(request);
        QVERIFY(found.isEmpty());
        QCOMPARE(scope.places[0].status, ContentScope::Status::NotFound);
        // Given (a folder chosen in the launcher, or by AI); typed ones go first.
        request.text = u"第三季度"_s;
        request.contentFolders = {f.root + u"\\备份"_s};
        QCOMPARE(search(request).second, QStringList {in(u"备份\\abc"_s)});
        request.text = u"第三季度 项目/"_s;
        QCOMPARE(search(request).second, QStringList {in(u"项目\\abc"_s)});
        // A program folder given: read all the same.
        request.text = u"第三季度"_s;
        request.contentFolders = {f.root + u"\\tools"_s};
        QCOMPARE(search(request).second, sorted({in(u"tools\\abc"_s), in(u"tools\\only"_s)}));

        // Through MCP: folders.
        Session s(f.server.get());
        QVERIFY(s.initialize());
        auto reply = s.exchange(callLine(2, u"search_content"_s,
                                    {{u"query"_s, u"第三季度"_s}, {u"folders"_s, QJsonArray {f.root + u"\\项目"_s, f.root + u"\\备份\\"_s}}}),
            20000);
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 2);
        reply = s.exchange(callLine(3, u"search_content"_s, {{u"query"_s, u"第三季度"_s}, {u"folders"_s, QJsonArray {f.root + u"\\不在"_s}}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not in the index"), qPrintable(out.text));
        reply = s.exchange(callLine(4, u"search_content"_s, {{u"query"_s, u"第三季度"_s}, {u"folders"_s, QJsonArray {in(u"其他"_s)}}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"is a file"), qPrintable(out.text));
    }

    void readFileText()
    {
        Fixture f;
        f.write(u"utf8.txt"_s, QByteArray("\xEF\xBB\xBF") + u"第一行\r\n报告 report\r\n"_s.toUtf8(), 1'700'000'000);
        f.write(u"gbk.txt"_s, QByteArray("\xB1\xA8\xB8\xE6\n"), 1'700'000'000); // 报告 in GBK
        f.write(u"utf16.txt"_s, QByteArray("\xFF\xFE", 2) + QByteArray(reinterpret_cast<const char*>(u"报告"), 4), 1'700'000'000);
        f.write(u"tool.exe"_s, QByteArray("MZ\0\0\x90\0", 6), 1'700'000'000);
        f.write(u"long.txt"_s, QByteArray(2500, 'a'), 1'700'000'000);
        f.write(u"empty.txt"_s, QByteArray(), 1'700'000'000);
        f.write(u"private\\p.txt"_s, "secret", 1'700'000'000);
        Policy policy = f.host->policy();
        policy.excludedPaths = {f.root + u"\\private"_s};
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        const auto read = [&](int id, const QString& name, QJsonObject more = {}) {
            more.insert(u"path"_s, f.root + u"\\"_s + name);
            const auto reply = s.exchange(callLine(id, u"read_file_text"_s, more), 20000);
            return reply ? outcomeOf(*reply) : Outcome {};
        };
        Outcome out = read(2, u"utf8.txt"_s);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"text"_s).toString(), u"第一行\n报告 report\n"_s); // no BOM, no CR
        QCOMPARE(out.structured.value(u"kind"_s).toString(), u"text"_s);
        QVERIFY(!out.structured.contains(u"next_offset"_s));
        if (ContentScanner::legacyCodePage() == 936) // as the content search reads it: in the system's code page
            QCOMPARE(read(3, u"gbk.txt"_s).structured.value(u"text"_s).toString(), u"报告\n"_s);
        QCOMPARE(read(4, u"utf16.txt"_s).structured.value(u"text"_s).toString(), u"报告"_s);
        out = read(5, u"tool.exe"_s);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not a text file"), qPrintable(out.text));

        // A long text in parts.
        out = read(6, u"long.txt"_s, {{u"max_chars"_s, 1000}});
        QCOMPARE(out.structured.value(u"text"_s).toString().size(), 1000);
        QCOMPARE(out.structured.value(u"chars"_s).toInt(), 2500);
        QCOMPARE(out.structured.value(u"next_offset"_s).toInt(), 1000);
        out = read(7, u"long.txt"_s, {{u"max_chars"_s, 1000}, {u"offset"_s, 2000}});
        QCOMPARE(out.structured.value(u"text"_s).toString().size(), 500);
        QVERIFY(!out.structured.contains(u"next_offset"_s));
        out = read(8, u"empty.txt"_s);
        QVERIFY(!out.isError);
        QVERIFY(out.structured.contains(u"note"_s));

        out = read(9, u"missing.txt"_s);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"no such file"), qPrintable(out.text));
        QVERIFY(read(10, u""_s).isError); // the folder itself
        out = read(11, u"private\\p.txt"_s);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not visible to AI"), qPrintable(out.text));
    }

    void recentAndFolder()
    {
        Fixture f;
        f.write(u"a.txt"_s, "a", 1'700'000'000);
        f.write(u"b.docx"_s, "b", 1'700'000'300);
        f.write(u"sub\\c.txt"_s, "c", 1'700'000'200);
        f.write(u"hidden.txt"_s, "h", 1'700'000'500, EntryFlag::Hidden);
        f.write(u"sys.txt"_s, "s", 1'700'000'600, EntryFlag::LowPriority);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        auto reply = s.exchange(callLine(2, u"recent_files"_s, {{u"folder"_s, f.root}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), (QStringList {u"b.docx"_s, u"c.txt"_s, u"a.txt"_s}));
        reply = s.exchange(callLine(3, u"recent_files"_s, {{u"folder"_s, f.root}, {u"include_hidden"_s, true}, {u"include_system"_s, true}, {u"limit"_s, 2}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), (QStringList {u"sys.txt"_s, u"hidden.txt"_s}));
        reply = s.exchange(callLine(4, u"recent_files"_s, {{u"folder"_s, f.root}, {u"extensions"_s, QJsonArray {u"docx"_s}}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"b.docx"_s});
        reply = s.exchange(callLine(5, u"recent_files"_s, {{u"folder"_s, f.root}, {u"extensions"_s, u".TXT"_s}, {u"days"_s, 1}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 0); // 2023 is older than a day
        reply = s.exchange(callLine(6, u"recent_files"_s, {{u"folder"_s, f.root + u"\\nowhere"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);

        reply = s.exchange(callLine(7, u"list_folder"_s, {{u"path"_s, f.root + u"\\"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), (QStringList {u"sub"_s, u"a.txt"_s, u"b.docx"_s, u"hidden.txt"_s, u"sys.txt"_s}));
        QCOMPARE(out.structured.value(u"total"_s).toInt(), 5);
        reply = s.exchange(callLine(8, u"list_folder"_s, {{u"path"_s, f.root}, {u"kind"_s, u"folders"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"sub"_s});
        reply = s.exchange(callLine(9, u"list_folder"_s, {{u"path"_s, f.root + u"\\a.txt"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);

        reply = s.exchange(callLine(10, u"get_file_info"_s, {{u"path"_s, f.root + u"/sub/c.txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(out.structured.value(u"exists"_s).toBool());
        QCOMPARE(out.structured.value(u"size"_s).toInt(), 1);
        QCOMPARE(out.structured.value(u"extension"_s).toString(), u"txt"_s);
        QVERIFY(out.structured.value(u"indexed"_s).toBool());
        QCOMPARE(out.structured.value(u"path"_s).toString(), f.root + u"\\sub\\c.txt"_s);
        reply = s.exchange(callLine(11, u"get_file_info"_s, {{u"path"_s, f.root + u"\\missing.txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(!out.isError);
        QVERIFY(!out.structured.value(u"exists"_s).toBool());

        reply = s.exchange(callLine(12, u"index_status"_s));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.structured.value(u"running"_s).toBool() && out.structured.value(u"connected"_s).toBool());
        QCOMPARE(out.structured.value(u"version"_s).toString(), u"0.0-test"_s);
        QVERIFY(out.structured.value(u"index"_s).toObject().value(u"items"_s).toInt() >= 5);
    }

    void openAndClipboard()
    {
        Fixture f;
        const QString file = f.write(u"open me.txt"_s, "x", 1'700'000'000);
        f.clips.push_back({1, u"text"_s, u"hello world"_s, 11, 1'700'000'000'000, 1'700'000'000'000, u"notepad"_s, {}, false, 0, 0});
        f.clips.push_back({2, u"link"_s, u"https://example.com"_s, 19, 1'700'000'001'000, 1'700'000'001'000, u"msedge"_s, u"固定"_s, true, 0, 0});
        f.clips.push_back({3, u"image"_s, {}, 0, 1'700'000'002'000, 1'700'000'002'000, u"mspaint"_s, {}, false, 640, 480});
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        policy.allowClipboard = true;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        auto reply = s.exchange(callLine(2, u"open_item"_s, {{u"path"_s, file}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(out.structured.value(u"opened"_s).toBool());
        QCOMPARE(f.opened, QStringList {u"open "_s + file});
        reply = s.exchange(callLine(3, u"show_in_folder"_s, {{u"path"_s, file}}));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.opened.last(), u"reveal "_s + file);
        reply = s.exchange(callLine(4, u"open_item"_s, {{u"path"_s, f.root + u"\\gone.txt"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        QCOMPARE(f.opened.size(), 2);
        policy.excludedPaths = {f.root};
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(5, u"open_item"_s, {{u"path"_s, file}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        QCOMPARE(f.opened.size(), 2);

        reply = s.exchange(callLine(6, u"clipboard_history"_s));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 3);
        const QJsonArray items = out.structured.value(u"items"_s).toArray();
        QCOMPARE(items.at(0).toObject().value(u"text"_s).toString(), u"hello world"_s);
        QCOMPARE(items.at(1).toObject().value(u"group"_s).toString(), u"固定"_s);
        QVERIFY(items.at(1).toObject().value(u"pinned"_s).toBool());
        QVERIFY(!items.at(2).toObject().contains(u"text"_s));
        QCOMPARE(items.at(2).toObject().value(u"width"_s).toInt(), 640);
        reply = s.exchange(callLine(7, u"clipboard_history"_s, {{u"kind"_s, u"link"_s}, {u"max_chars"_s, 50}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 1);
        reply = s.exchange(callLine(8, u"clipboard_get"_s, {{u"id"_s, 1}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"text"_s).toString(), u"hello world"_s);
        reply = s.exchange(callLine(9, u"clipboard_get"_s, {{u"id"_s, 99}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        // The log never has the texts.
        for (const QString& line : f.log)
            QVERIFY2(!line.contains(u"hello world"), qPrintable(line));
    }

    // screenshot_history: under the clipboard's switch, empty with a reason
    // while the capture is off; each picture copied out to the temporary
    // folder for the client to look at.
    void screenshotHistory()
    {
        Fixture f;
        const QString picture = f.root + u"\\history-5.png"_s;
        {
            QFile file(picture); // what is in it does not matter here: it is copied as it is
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not really a PNG");
        }
        f.shots.push_back({5, 1'700'000'002'000, 4, 3, u"季度报表 Q3"_s, picture});
        f.shots.push_back({4, 1'700'000'001'000, 1920, 1080, QString(), QString()}); // its file still being written
        QVERIFY(!f.host->toolNames().contains(u"screenshot_history"_s)); // the clipboard's switch is off
        Policy policy = f.host->policy();
        policy.allowClipboard = true;
        f.host->setPolicy(policy);
        QVERIFY(f.host->toolNames().contains(u"screenshot_history"_s));
        Session s(f.server.get());
        QVERIFY(s.initialize());

        // The capture off: nothing, and why.
        auto reply = s.exchange(callLine(2, u"screenshot_history"_s));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 0);
        QVERIFY(out.structured.value(u"reason"_s).toString().contains(u"turned off"));

        policy.captureEnabled = true;
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(3, u"screenshot_history"_s));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 2);
        QVERIFY(!out.structured.contains(u"reason"_s));
        const QJsonArray items = out.structured.value(u"items"_s).toArray();
        const QJsonObject first = items.at(0).toObject();
        QCOMPARE(first.value(u"id"_s).toInt(), 5);
        QCOMPARE(first.value(u"text"_s).toString(), u"季度报表 Q3"_s);
        QCOMPARE(first.value(u"width"_s).toInt(), 4);
        QVERIFY(first.value(u"taken"_s).toString().startsWith(u"2023-11-1"));
        const QString copy = first.value(u"image_path"_s).toString();
        QCOMPARE(QFileInfo(copy).absoluteFilePath(), QFileInfo(f.tempFolder + u"/5.png"_s).absoluteFilePath());
        QFile copied(copy);
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), QByteArray("not really a PNG"));
        QVERIFY(!items.at(1).toObject().contains(u"image_path"_s));
        // Words and a limit.
        reply = s.exchange(callLine(4, u"screenshot_history"_s, {{u"query"_s, u"Q3"_s}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 1);
        reply = s.exchange(callLine(5, u"screenshot_history"_s, {{u"limit"_s, 1}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 1);
        // The log never has the words read in the pictures.
        for (const QString& line : f.log)
            QVERIFY2(!line.contains(u"季度报表"), qPrintable(line));

        // A screenshot may deduplicate with an earlier JPEG containing the
        // same pixels. Its exported bytes and suffix still agree.
        const QString jpeg = f.root + u"/history-6.jpg"_s;
        {
            QFile file(jpeg);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("original JPEG bytes");
        }
        f.shots = {{6, 1'700'000'003'000, 4, 3, QString(), jpeg}};
        reply = s.exchange(callLine(6, u"screenshot_history"_s));
        QVERIFY(reply);
        const QString jpegCopy = outcomeOf(*reply).structured.value(u"items"_s).toArray().at(0).toObject()
                                     .value(u"image_path"_s).toString();
        QVERIFY(jpegCopy.endsWith(u"/6.jpg") || jpegCopy.endsWith(u"\\6.jpg"));
        QFile original(jpegCopy);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("original JPEG bytes"));
    }

    // take_screenshot: off unless switched on; then only what the user
    // answers (here a stand-in for App::confirmScreenshot) decides.
    void takeScreenshot()
    {
        Fixture f;
        Policy policy = f.host->policy();
        policy.allowClipboard = true;
        policy.captureEnabled = true;
        f.host->setPolicy(policy);
        QVERIFY(!f.host->toolNames().contains(u"take_screenshot"_s));
        QCOMPARE(f.host->known(u"take_screenshot"_s), ToolHost::Known::Off);
        int asked = 0;
        ScreenshotRequest last;
        f.asker = [&](const ScreenshotRequest& request, std::function<void(ScreenshotTaken)> done) {
            ++asked;
            last = request;
            done({u"The user refused the screenshot."_s});
        };
        Session s(f.server.get());
        QVERIFY(s.initialize());
        // Off by default: refused without asking anyone.
        auto reply = s.exchange(callLine(2, u"take_screenshot"_s));
        QVERIFY(reply);
        QVERIFY(outcomeOf(*reply).isError);
        QCOMPARE(asked, 0);

        policy.allowScreenshot = true;
        f.host->setPolicy(policy);
        QVERIFY(f.host->toolNames().contains(u"take_screenshot"_s));
        reply = s.exchange(callLine(3, u"take_screenshot"_s, {{u"monitor"_s, 2}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"refused"), qPrintable(out.text));
        QCOMPARE(asked, 1);
        QCOMPARE(last.client, u"tst_mcp"_s);
        QCOMPARE(last.monitor, 2);
        reply = s.exchange(callLine(4, u"take_screenshot"_s, {{u"monitor"_s, u"two"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError); // not a number: nobody is asked
        QCOMPARE(asked, 1);

        // Allowed a moment later, from another thread (the grab's).
        f.asker = [&](const ScreenshotRequest&, std::function<void(ScreenshotTaken)> done) {
            ++asked;
            QTimer::singleShot(50, [done] {
                std::thread([done] { done({{}, u"C:/Temp/WinShun/mcp/screenshot-1.png"_s, 2560, 1600, u"monitor 1 (2560x1600, primary)"_s}); }).detach();
            });
        };
        reply = s.exchange(callLine(5, u"take_screenshot"_s));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(asked, 2);
        QCOMPARE(out.structured.value(u"path"_s).toString(), u"C:\\Temp\\WinShun\\mcp\\screenshot-1.png"_s);
        QCOMPARE(out.structured.value(u"width"_s).toInt(), 2560);
        QVERIFY(out.structured.value(u"monitor"_s).toString().contains(u"primary"));

        // Never answered: the call waits (the app's own timeout answers it), and is cancelled cleanly.
        f.asker = {};
        QVERIFY(!s.exchange(callLine(6, u"take_screenshot"_s), 300));
        QCOMPARE(f.host->pendingCalls(), 1);
        f.server->receive(s.id, jsonrpc::notification(u"notifications/cancelled"_s, {{u"requestId"_s, 6}}));
        QCOMPARE(f.host->pendingCalls(), 0);
    }

    static QJsonObject changeArgs(const QJsonArray& changes, const QString& reason = u"Because the user asked"_s)
    {
        return {{u"changes"_s, changes}, {u"reason"_s, reason}};
    }
    static QJsonArray setting(const QString& key, const QJsonValue& value)
    {
        return {QJsonObject {{u"key"_s, key}, {u"value"_s, value}}};
    }

    // get_settings: the filters reach the app, and the answer says how AI may change settings.
    void getSettings()
    {
        Fixture f;
        Session s(f.server.get());
        QVERIFY(s.initialize());
        auto reply = s.exchange(callLine(2, u"get_settings"_s,
            {{u"sections"_s, QJsonArray {u"clipboard"_s}}, {u"query"_s, u"剪贴板 history"_s}, {u"keys"_s, QJsonArray {u"Results/Sort"_s}},
                {u"changed_only"_s, true}, {u"facts"_s, true}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(f.lastSettingsQuery.sections, QStringList {u"clipboard"_s});
        QCOMPARE(f.lastSettingsQuery.words, u"剪贴板 history"_s);
        QCOMPARE(f.lastSettingsQuery.keys, QStringList {u"Results/Sort"_s});
        QVERIFY(f.lastSettingsQuery.changedOnly && f.lastSettingsQuery.facts);
        QCOMPARE(out.structured.value(u"ai"_s).toObject().value(u"settings_mode"_s).toString(), u"off"_s);
        QVERIFY(!out.structured.value(u"ai"_s).toObject().value(u"change_settings_offered"_s).toBool());
        QCOMPARE(out.structured.value(u"settings"_s).toArray().size(), 3);
        reply = s.exchange(callLine(3, u"get_settings"_s, {{u"changed_only"_s, u"yes"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);

        Policy policy = f.host->policy();
        policy.settingsMode = SettingsMode::Auto;
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(4, u"get_settings"_s));
        out = outcomeOf(*reply);
        QCOMPARE(out.structured.value(u"ai"_s).toObject().value(u"settings_mode"_s).toString(), u"auto"_s);
        QVERIFY(out.structured.value(u"ai"_s).toObject().value(u"change_settings_offered"_s).toBool());
        reply = s.exchange(callLine(5, u"index_status"_s));
        QCOMPARE(outcomeOf(*reply).structured.value(u"ai"_s).toObject().value(u"settings_mode"_s).toString(), u"auto"_s);

        // The tools say what they take.
        for (const QJsonValue& v : f.host->tools()) {
            const QJsonObject tool = v.toObject();
            if (tool.value(u"name"_s).toString() != u"change_settings")
                continue;
            const QJsonObject annotations = tool.value(u"annotations"_s).toObject();
            QVERIFY(!annotations.value(u"readOnlyHint"_s).toBool());
            QVERIFY(!annotations.value(u"destructiveHint"_s).toBool());
            QVERIFY(annotations.value(u"idempotentHint"_s).toBool());
            QVERIFY(!annotations.value(u"openWorldHint"_s).toBool());
            const QJsonObject schema = tool.value(u"inputSchema"_s).toObject();
            QCOMPARE(schema.value(u"required"_s).toArray().size(), 2);
            QCOMPARE(schema.value(u"properties"_s).toObject().value(u"changes"_s).toObject().value(u"maxItems"_s).toInt(), 20);
        }
    }

    void openSettings()
    {
        Fixture f;
        Session s(f.server.get());
        QVERIFY(s.initialize());
        auto reply = s.exchange(callLine(2, u"open_settings"_s, {{u"key"_s, u"Results/Sort"_s}}));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.openedAt, u"Results/Sort|"_s);
        reply = s.exchange(callLine(3, u"open_settings"_s, {{u"section"_s, u"clipboard"_s}}));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.openedAt, u"|clipboard"_s);
        reply = s.exchange(callLine(4, u"open_settings"_s));
        QVERIFY(reply && outcomeOf(*reply).isError);
    }

    // Off: not offered, and the refusal names the switch. Ask: the card decides.
    void changeSettingsAsk()
    {
        Fixture f;
        Session s(f.server.get());
        QVERIFY(s.initialize());
        auto reply = s.exchange(callLine(2, u"change_settings"_s, changeArgs(setting(u"Results/Sort"_s, u"newest"_s))));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"允许 AI 修改设置"), qPrintable(out.text));
        QVERIFY(out.text.contains(u"AI 助手"));
        QCOMPARE(f.settingsAsks, 0);

        Policy policy = f.host->policy();
        policy.settingsMode = SettingsMode::Ask;
        f.host->setPolicy(policy);
        const QJsonArray sort = setting(u"Results/Sort"_s, u"newest"_s);

        // Declined, no answer, not shown: errors, nothing applied.
        const std::pair<SettingsAnswer, const char*> refusals[] {{SettingsAnswer::Refused, "declined"}, {SettingsAnswer::Timeout, "60 seconds"}, {SettingsAnswer::Unavailable, "could not ask"}, {SettingsAnswer::Busy, "settings request is waiting"}};
        int id = 3;
        for (const auto& [answer, words] : refusals) {
            f.settingsAsker = [answer = answer](const SettingsAsk&, std::function<void(SettingsAnswer, bool)> done) { done(answer, true); };
            reply = s.exchange(callLine(id++, u"change_settings"_s, changeArgs(sort)));
            QVERIFY(reply);
            out = outcomeOf(*reply);
            QVERIFY(out.isError);
            QVERIFY2(out.text.contains(QLatin1StringView(words)), qPrintable(out.text));
        }
        QVERIFY(f.settingsApplied.isEmpty());
        QCOMPARE(f.settingValues.value(u"Results/Sort"_s), QJsonValue(u"match"_s));
        QCOMPARE(f.settingsAsks, 4);

        // Allowed (a moment later, as a person would): what the card shows is what changes, with who asks and why.
        f.settingsAsker = [](const SettingsAsk&, std::function<void(SettingsAnswer, bool)> done) {
            QTimer::singleShot(30, [done] { done(SettingsAnswer::Allowed, false); });
        };
        reply = s.exchange(callLine(id++, u"change_settings"_s, changeArgs(sort, u"Newest first suits you"_s)));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"changed"_s).toInt(), 1);
        QCOMPARE(f.lastAsk.client, u"tst_mcp"_s);
        QCOMPARE(f.lastAsk.reason, u"Newest first suits you"_s);
        QCOMPARE(f.lastAsk.items.size(), 1);
        QVERIFY(!f.lastAsk.askAlways);
        QCOMPARE(f.settingsApplied.size(), 1);
        QVERIFY(!f.settingsApplied.first().alwaysAllow);
        QCOMPARE(f.settingsApplied.first().reason, u"Newest first suits you"_s);
        QCOMPARE(f.settingValues.value(u"Results/Sort"_s), QJsonValue(u"newest"_s));

        // "From now on" counts when allowed on a card with nothing that asks always.
        f.settingsAsker = [](const SettingsAsk&, std::function<void(SettingsAnswer, bool)> done) { done(SettingsAnswer::Allowed, true); };
        reply = s.exchange(callLine(id++, u"change_settings"_s, changeArgs(setting(u"Results/Sort"_s, u"oldest"_s))));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QVERIFY(f.settingsApplied.last().alwaysAllow);
    }

    // The reason is the model's own words, and must be there; changes come 1 to 20.
    void changeSettingsArguments()
    {
        Fixture f;
        Policy policy = f.host->policy();
        policy.settingsMode = SettingsMode::Auto;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        const QJsonArray sort = setting(u"Results/Sort"_s, u"newest"_s);
        for (const QJsonObject& args : {QJsonObject {{u"changes"_s, sort}}, QJsonObject {{u"changes"_s, sort}, {u"reason"_s, u"  "_s}},
                 QJsonObject {{u"changes"_s, QJsonArray()}, {u"reason"_s, u"x"_s}}, QJsonObject {{u"changes"_s, u"Sort"_s}, {u"reason"_s, u"x"_s}},
                 QJsonObject {{u"reason"_s, u"x"_s}}}) {
            const auto reply = s.exchange(callLine(2, u"change_settings"_s, args));
            QVERIFY(reply && outcomeOf(*reply).isError);
        }
        QJsonArray many;
        for (int i = 0; i < 21; ++i)
            many.push_back(sort.first());
        auto reply = s.exchange(callLine(3, u"change_settings"_s, changeArgs(many)));
        QVERIFY(reply && outcomeOf(*reply).isError);
        QVERIFY(f.settingsApplied.isEmpty());

        reply = s.exchange(callLine(4, u"change_settings"_s, changeArgs(sort, QString(300, u'x'))));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.settingsApplied.last().reason.size(), 200);
    }

    // AI's own words reach the card as one plain line (no line breaks, no direction
    // or invisible characters), cut; what the user was shown is what may be applied.
    void changeSettingsTextAndApproval()
    {
        Fixture f;
        Policy policy = f.host->policy();
        policy.settingsMode = SettingsMode::Ask;
        f.host->setPolicy(policy);
        f.settingsAsker = [](const SettingsAsk&, std::function<void(SettingsAnswer, bool)> done) { done(SettingsAnswer::Allowed, false); };
        Session s(f.server.get());
        QVERIFY(s.initialize());
        const QString messy = u"Tidy up\nthe   list\t\u202Eand\u2028more\u200B\u0007 now"_s;
        auto reply = s.exchange(callLine(2, u"change_settings"_s, changeArgs(setting(u"Results/Sort"_s, u"newest"_s), messy)));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.lastAsk.reason, u"Tidy up the list and more now"_s);
        QCOMPARE(f.settingsApplied.last().reason, f.lastAsk.reason);
        QVERIFY(f.settingsApplied.last().asked);
        QCOMPARE(f.settingsApplied.last().approved.size(), std::size_t {1});
        QCOMPARE(f.settingsApplied.last().approved.front().key, u"Results/Sort"_s);
        QCOMPARE(f.settingsApplied.last().approved.front().value, QJsonValue(u"newest"_s));

        // No card in auto mode: not asked.
        policy.settingsMode = SettingsMode::Auto;
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(3, u"change_settings"_s, changeArgs(setting(u"Results/Sort"_s, u"oldest"_s))));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QVERIFY(!f.settingsApplied.last().asked);
        QCOMPARE(f.settingsApplied.last().approved.size(), std::size_t {1});
    }

    void cleanShownTextRules()
    {
        QCOMPARE(cleanShownText(QString(300, u'x'), 200).size(), 200);
        QVERIFY(cleanShownText(QString(300, u'x'), 200).endsWith(QChar(0x2026)));
        QCOMPARE(cleanShownText(u"  a\r\n\u2029b  "_s, 50), u"a b"_s);
        QCOMPARE(cleanShownText(u"\u202Eevil\u202C\u200F"_s, 50), u"evil"_s);
        // Not cut between the halves of one character.
        const QString faces = QString(10, u'a') + u"\U0001F600\U0001F600\U0001F600"_s;
        const QString cut = cleanShownText(faces, 12);
        QVERIFY(cut.endsWith(QChar(0x2026)));
        for (qsizetype i = 0; i < cut.size(); ++i)
            QVERIFY(!cut[i].isHighSurrogate() || (i + 1 < cut.size() && cut[i + 1].isLowSurrogate()));
    }

    // Auto: no card, applied at once; what asks always still asks.
    void changeSettingsAuto()
    {
        Fixture f;
        Policy policy = f.host->policy();
        policy.settingsMode = SettingsMode::Auto;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        auto reply = s.exchange(callLine(2, u"change_settings"_s, changeArgs(setting(u"Results/Sort"_s, u"newest"_s))));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(f.settingsAsks, 0);
        QCOMPARE(f.settingsApplied.size(), 1);
        QCOMPARE(f.settingValues.value(u"Results/Sort"_s), QJsonValue(u"newest"_s));

        // Something that asks always: the card, even in auto, without the "from now on" box; refused, nothing changes
        // (the plain switch in the same call too).
        f.settingsAsker = [](const SettingsAsk&, std::function<void(SettingsAnswer, bool)> done) { done(SettingsAnswer::Refused, false); };
        QJsonArray both = setting(u"Launcher/DoubleCtrl"_s, false);
        both.push_back(QJsonObject {{u"key"_s, u"Launcher/Autostart"_s}, {u"value"_s, true}});
        reply = s.exchange(callLine(3, u"change_settings"_s, changeArgs(both)));
        QVERIFY(reply && outcomeOf(*reply).isError);
        QCOMPARE(f.settingsAsks, 1);
        QVERIFY(f.lastAsk.askAlways);
        QCOMPARE(f.lastAsk.items.size(), 2);
        QCOMPARE(f.settingsApplied.size(), 1);
        QCOMPARE(f.settingValues.value(u"Launcher/DoubleCtrl"_s), QJsonValue(true));

        // Allowed with the box ticked: it does not count, the batch has one that asks always.
        f.settingsAsker = [](const SettingsAsk&, std::function<void(SettingsAnswer, bool)> done) { done(SettingsAnswer::Allowed, true); };
        reply = s.exchange(callLine(4, u"change_settings"_s, changeArgs(both)));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.settingsApplied.size(), 2);
        QVERIFY(!f.settingsApplied.last().alwaysAllow);
        QCOMPARE(f.settingValues.value(u"Launcher/Autostart"_s), QJsonValue(true));
    }

    // All or nothing: one wrong change stops the call with every problem named, and nobody is asked.
    void changeSettingsIsAtomic()
    {
        Fixture f;
        Policy policy = f.host->policy();
        policy.settingsMode = SettingsMode::Auto;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        QJsonArray changes = setting(u"Results/Sort"_s, u"newest"_s);
        changes.push_back(QJsonObject {{u"key"_s, u"Nope/One"_s}, {u"value"_s, 1}});
        changes.push_back(QJsonObject {{u"key"_s, u"Launcher/DoubleCtrl"_s}, {u"value"_s, u"maybe"_s}});
        const auto reply = s.exchange(callLine(2, u"change_settings"_s, changeArgs(changes)));
        QVERIFY(reply);
        const Outcome out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"Nope/One") && out.text.contains(u"wrong type"), qPrintable(out.text));
        QVERIFY(out.text.contains(u"Nothing was changed"));
        QCOMPARE(f.settingsAsks, 0);
        QVERIFY(f.settingsApplied.isEmpty());
        QCOMPARE(f.settingValues.value(u"Results/Sort"_s), QJsonValue(u"match"_s));
    }

    // Everything set so already: done, without a card.
    void changeSettingsNoOp()
    {
        Fixture f;
        Policy policy = f.host->policy();
        policy.settingsMode = SettingsMode::Ask;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        const auto reply = s.exchange(callLine(2, u"change_settings"_s, changeArgs(setting(u"Results/Sort"_s, u"match"_s))));
        QVERIFY(reply);
        const Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"changed"_s).toInt(), 0);
        QCOMPARE(out.structured.value(u"unchanged"_s).toArray().size(), 1);
        QCOMPARE(f.settingsAsks, 0);
        QVERIFY(f.settingsApplied.isEmpty());
    }

    // A card waiting: a second request is refused; the client giving the call up takes the card away.
    void changeSettingsCancelAndBusy()
    {
        Fixture f;
        Policy policy = f.host->policy();
        policy.settingsMode = SettingsMode::Ask;
        f.host->setPolicy(policy);
        std::function<void(SettingsAnswer, bool)> answer;
        f.settingsAsker = [&](const SettingsAsk&, std::function<void(SettingsAnswer, bool)> done) { answer = std::move(done); };
        Session s(f.server.get());
        QVERIFY(s.initialize());
        QVERIFY(!s.exchange(callLine(2, u"change_settings"_s, changeArgs(setting(u"Results/Sort"_s, u"newest"_s))), 300));
        QCOMPARE(f.host->pendingCalls(), 1);
        QCOMPARE(f.settingsAsks, 1);

        const auto busy = s.exchange(callLine(3, u"change_settings"_s, changeArgs(setting(u"Results/Sort"_s, u"oldest"_s))));
        QVERIFY(busy);
        const Outcome refused = outcomeOf(*busy);
        QVERIFY(refused.isError);
        QVERIFY2(refused.text.contains(u"Another"), qPrintable(refused.text));
        QCOMPARE(f.settingsAsks, 1);

        f.server->receive(s.id, jsonrpc::notification(u"notifications/cancelled"_s, {{u"requestId"_s, 2}}));
        QCOMPARE(f.host->pendingCalls(), 0);
        QCOMPARE(f.settingsCancels, 1);
        // A late answer goes nowhere.
        answer(SettingsAnswer::Allowed, false);
        QTest::qWait(50);
        QVERIFY(f.settingsApplied.isEmpty());
        QCOMPARE(f.settingValues.value(u"Results/Sort"_s), QJsonValue(u"match"_s));
    }

    void prompts()
    {
        Fixture f;
        Session s(f.server.get());
        QVERIFY(s.initialize());
        auto reply = s.exchange(requestLine(2, u"prompts/list"_s));
        QVERIFY(reply);
        const QJsonArray list = parseObject(*reply).value(u"result"_s).toObject().value(u"prompts"_s).toArray();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().toObject().value(u"name"_s).toString(), u"setup"_s);
        QCOMPARE(list.first().toObject().value(u"arguments"_s).toArray().first().toObject().value(u"name"_s).toString(), u"focus"_s);

        reply = s.exchange(requestLine(3, u"prompts/get"_s, {{u"name"_s, u"setup"_s}, {u"arguments"_s, QJsonObject {{u"focus"_s, u"the clipboard"_s}}}}));
        QVERIFY(reply);
        const QJsonObject result = parseObject(*reply).value(u"result"_s).toObject();
        const QJsonObject message = result.value(u"messages"_s).toArray().first().toObject();
        QCOMPARE(message.value(u"role"_s).toString(), u"user"_s);
        const QString text = message.value(u"content"_s).toObject().value(u"text"_s).toString();
        QVERIFY(text.contains(u"get_settings"));
        QVERIFY(text.contains(u"change_settings"));
        QVERIFY(text.contains(u"Answer in my language"));
        QVERIFY(text.contains(u"the clipboard"));
        reply = s.exchange(requestLine(4, u"prompts/get"_s, {{u"name"_s, u"nothing"_s}}));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kInvalidParams);
    }

    // Only what the caller could list or read itself: a folder for
    // administrators alone is, to the viewer, not there, nor is anything in it.
    void visibility()
    {
        Fixture f;
        f.write(u"open.txt"_s, "visible SECRETWORD here", 1'700'000'000);
        f.write(u"secret\\hidden.txt"_s, "SECRETWORD inside", 1'700'000'500);
        f.write(u"secret\\deeper\\more.txt"_s, "SECRETWORD too", 1'700'000'600);
        QVERIFY(f.restrict(u"secret"_s));
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        // The folder's own name is in a folder the viewer can list; its contents are not.
        auto reply = s.exchange(callLine(2, u"list_folder"_s, {{u"path"_s, f.root}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), (QStringList {u"secret"_s, u"open.txt"_s}));
        reply = s.exchange(callLine(3, u"list_folder"_s, {{u"path"_s, f.root + u"\\secret"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not in the index"), qPrintable(out.text)); // the same words as for a folder that is not there
        reply = s.exchange(callLine(4, u"search_files"_s, {{u"query"_s, u"ext:txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(names(out.structured), QStringList {u"open.txt"_s});
        QVERIFY(!out.structured.contains(u"total_matches"_s)); // no count of what is hidden either
        reply = s.exchange(callLine(5, u"recent_files"_s, {{u"folder"_s, f.root}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"open.txt"_s});
        reply = s.exchange(callLine(6, u"get_file_info"_s, {{u"path"_s, f.root + u"\\secret\\hidden.txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(!out.isError);
        QVERIFY(!out.structured.value(u"exists"_s).toBool());
        reply = s.exchange(callLine(7, u"get_file_info"_s, {{u"path"_s, f.root + u"\\secret"_s}}));
        QVERIFY(reply);
        QVERIFY(!outcomeOf(*reply).structured.value(u"exists"_s).toBool());
        reply = s.exchange(callLine(8, u"get_file_info"_s, {{u"path"_s, f.root + u"\\open.txt"_s}}));
        QVERIFY(reply);
        QVERIFY(outcomeOf(*reply).structured.value(u"exists"_s).toBool());
        reply = s.exchange(callLine(9, u"search_content"_s, {{u"query"_s, u"SECRETWORD"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"open.txt"_s});
        reply = s.exchange(callLine(10, u"open_item"_s, {{u"path"_s, f.root + u"\\secret\\hidden.txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"no such file"), qPrintable(out.text));
        QVERIFY(f.opened.isEmpty());
        reply = s.exchange(callLine(11, u"open_item"_s, {{u"path"_s, f.root + u"\\open.txt"_s}}));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.opened.size(), 1);
        // The checks are counted in the log.
        QVERIFY2(f.log.last().contains(u"access checks"), qPrintable(f.log.last()));

        // Let go of the restriction: everything is back.
        f.unrestrict();
        reply = s.exchange(callLine(12, u"search_files"_s, {{u"query"_s, u"ext:txt"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured).size(), 3);
    }

    // GitHub #114: paths on servers and in WSL.
    void remotePaths()
    {
        // Where a path is, from its text alone.
        using Place = Visibility::Place;
        const std::vector<std::wstring> roots {L"\\\\nas\\docs", L"\\\\127.0.0.1@8714\\DavWWWRoot", L"\\\\wsl.localhost\\Ubuntu"};
        QCOMPARE(Visibility::placeOf(u"C:\\Users\\me"_s, roots), Place::Local);
        QCOMPARE(Visibility::placeOf(u"d:"_s, roots), Place::Local);
        QCOMPARE(Visibility::placeOf(u"D:/data/a.txt"_s, roots), Place::Local);
        QCOMPARE(Visibility::placeOf(u"C:relative"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"relative\\a.txt"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"\\\\nas\\docs"_s, roots), Place::Remote);
        QCOMPARE(Visibility::placeOf(u"\\\\NAS\\Docs\\a.txt"_s, roots), Place::Remote);
        QCOMPARE(Visibility::placeOf(u"//nas/docs/a.txt"_s, roots), Place::Remote);
        QCOMPARE(Visibility::placeOf(u"\\\\nas\\docs2\\a.txt"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"\\\\nas\\other\\a.txt"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"\\\\nas"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"\\\\attacker.example\\share\\a.txt"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"\\\\?\\UNC\\nas\\docs\\a.txt"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"\\\\?\\C:\\a.txt"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"\\\\.\\pipe\\x"_s, roots), Place::Elsewhere);
        QCOMPARE(Visibility::placeOf(u"\\\\nas\\docs\\a.txt"_s, {}), Place::Elsewhere);
        // WebDAV: the server's root holds every folder on it, written either way.
        QCOMPARE(Visibility::placeOf(u"\\\\127.0.0.1@8714\\DavWWWRoot\\dav\\a.txt"_s, roots), Place::Remote);
        QCOMPARE(Visibility::placeOf(u"\\\\127.0.0.1@8714\\dav\\a.txt"_s, roots), Place::Remote);
        QCOMPARE(Visibility::placeOf(u"\\\\127.0.0.1@8715\\dav\\a.txt"_s, roots), Place::Elsewhere);
        // WSL by either of its names; another distribution is not under the root.
        QCOMPARE(Visibility::placeOf(u"\\\\wsl$\\Ubuntu\\home\\me"_s, roots), Place::Remote);
        QCOMPARE(Visibility::placeOf(u"\\\\wsl.localhost\\Debian\\home"_s, roots), Place::Elsewhere);

        // Under a root on a server, what the index has is this user's to see
        // and read, and no server is asked (nothing answers at 192.0.2.1: one
        // asked would hold every check up for seconds). Anyone else: nothing.
        const std::vector<std::wstring> away {L"\\\\192.0.2.1\\share"};
        QElapsedTimer timer;
        timer.start();
        {
            Visibility mine(nullptr, away);
            QVERIFY(mine.sees(u"\\\\192.0.2.1\\share\\a.txt"_s));
            QVERIFY(mine.canRead(u"\\\\192.0.2.1\\share\\a.txt"_s));
            QVERIFY(mine.canList(u"\\\\192.0.2.1\\share"_s));
            QVERIFY(!mine.canRead(u"\\\\192.0.2.1\\other\\a.txt"_s)); // not under the root
            QCOMPARE(mine.checks(), 0);
        }
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
        HANDLE anonymous = nullptr;
        QVERIFY(::ImpersonateAnonymousToken(::GetCurrentThread()));
        const BOOL opened = ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &anonymous);
        ::RevertToSelf();
        QVERIFY(opened);
        {
            Visibility other(anonymous, away);
            QVERIFY(!other.sees(u"\\\\192.0.2.1\\share\\a.txt"_s));
            QVERIFY(!other.canRead(u"\\\\192.0.2.1\\share\\a.txt"_s));
        }
        ::CloseHandle(anonymous);

        // The tools, with that share configured and a file of it in the index.
        auto f = std::make_unique<Fixture>(true, true, std::vector<ShareConfig> {{L"\\\\192.0.2.1\\share"}});
        {
            const auto index = f->service->index();
            const auto lock = index->writeLock();
            const EntryId shareRoot = index->addRoot("\\\\192.0.2.1\\share");
            const EntryId file = index->add(shareRoot, "remote-report.txt", 0);
            index->setModified(file, 1'700'000'000);
            index->setSize(file, packSize(1234));
        }
        Policy policy = f->host->policy();
        policy.allowOpen = true;
        f->host->setPolicy(policy);
        auto s = std::make_unique<Session>(f->server.get());
        QVERIFY(s->initialize());
        timer.restart();
        auto reply = s->exchange(callLine(2, u"search_files"_s, {{u"query"_s, u"remote-report"_s}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"remote-report.txt"_s});
        QCOMPARE(out.structured.value(u"items"_s).toArray().first().toObject().value(u"size"_s).toInt(), 1234); // the index's
        reply = s->exchange(callLine(3, u"list_folder"_s, {{u"path"_s, u"\\\\192.0.2.1\\share"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"remote-report.txt"_s});
        reply = s->exchange(callLine(4, u"recent_files"_s, {{u"folder"_s, u"\\\\192.0.2.1\\share"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"remote-report.txt"_s});
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed()))); // no server was asked

        // A path on a server the index has no root on is refused before
        // anything touches it (that would sign in to it).
        timer.restart();
        const QString elsewhere = u"\\\\192.0.2.2\\share\\a.txt"_s;
        const auto refused = [&](int id, const QString& tool, const QJsonObject& arguments) {
            const auto r = s->exchange(callLine(id, tool, arguments));
            const Outcome o = r ? outcomeOf(*r) : Outcome {};
            return r && o.isError && o.text.contains(u"network locations Win顺 indexes");
        };
        QVERIFY(refused(5, u"get_file_info"_s, {{u"path"_s, elsewhere}}));
        QVERIFY(refused(6, u"read_file_text"_s, {{u"path"_s, elsewhere}}));
        QVERIFY(refused(7, u"open_item"_s, {{u"path"_s, elsewhere}}));
        QVERIFY(refused(8, u"search_files"_s, {{u"query"_s, u"\\\\192.0.2.2\\share\\report"_s}}));
        QVERIFY(refused(9, u"get_file_info"_s, {{u"path"_s, u"\\\\?\\UNC\\192.0.2.2\\share\\a.txt"_s}}));
        QVERIFY(refused(10, u"get_file_info"_s, {{u"path"_s, u"\\\\.\\pipe\\x"_s}}));
        QVERIFY(f->opened.isEmpty());
        QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));

        // A file under the root is asked of its server off the GUI thread: one
        // that does not answer holds up that call alone, and the host goes
        // without waiting for it.
        const qsizetype before = s->received.size();
        timer.restart();
        f->server->receive(s->id, callLine(11, u"get_file_info"_s, {{u"path"_s, u"\\\\192.0.2.1\\share\\remote-report.txt"_s}}));
        f->server->receive(s->id, callLine(12, u"read_file_text"_s, {{u"path"_s, u"\\\\192.0.2.1\\share\\remote-report.txt"_s}}));
        QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
        f->server->receive(s->id, requestLine(13, u"ping"_s));
        QTRY_VERIFY_WITH_TIMEOUT(std::ranges::any_of(s->received.mid(before),
                                     [](const QByteArray& line) { return parseObject(line).value(u"id"_s).toInt() == 13; }),
            1000);
        timer.restart();
        s.reset();
        f.reset();
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    }

    void cancelAndQueue()
    {
        Fixture f;
        f.write(u"q.txt"_s, "q", 1'700'000'000);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        // Two searches at once: the second waits; cancelled, it never answers.
        const qsizetype before = s.received.size();
        f.server->receive(s.id, callLine(2, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        f.server->receive(s.id, callLine(3, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        f.server->receive(s.id, jsonrpc::notification(u"notifications/cancelled"_s, {{u"requestId"_s, 3}}));
        QTRY_VERIFY(s.received.size() > before);
        QTest::qWait(300);
        QCOMPARE(s.received.size(), before + 1);
        QCOMPARE(parseObject(s.received.last()).value(u"id"_s).toInt(), 2);
        QCOMPARE(f.host->pendingCalls(), 0);

        // Three in a row all answer, in order.
        const qsizetype start = s.received.size();
        for (int i = 4; i <= 6; ++i)
            f.server->receive(s.id, callLine(i, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        QTRY_COMPARE(s.received.size(), start + 3);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(parseObject(s.received.at(start + i)).value(u"id"_s).toInt(), 4 + i);

        // A session that ends takes its calls with it.
        {
            Session other(f.server.get());
            QVERIFY(other.initialize());
            f.server->receive(other.id, callLine(7, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        }
        QTest::qWait(200);
        QCOMPARE(f.host->pendingCalls(), 0);
    }

    void pipeEndToEnd()
    {
        Fixture f;
        f.write(u"管道.txt"_s, "p", 1'700'000'000);
        const std::wstring name = testPipeName("pipe");
        DWORD error = 0;
        QVERIFY2(f.server->listen(name, &error), qPrintable(QString::number(error)));
        // The same name again is refused: it is taken.
        {
            McpServer other(f.host.get(), u"x"_s);
            QVERIFY(!other.listen(name, &error));
            QCOMPARE(error, DWORD {ERROR_ACCESS_DENIED});
        }

        pipe::Client client;
        QVERIFY(client.connect(name, 2000, &error));
        QTRY_COMPARE(f.server->sessionCount(), 1);
        QVERIFY(client.write(requestLine(1, u"initialize"_s, {{u"protocolVersion"_s, u"2025-11-25"_s}, {u"clientInfo"_s, QJsonObject {{u"name"_s, u"pipe-test"_s}}}})));
        QByteArray line;
        // Reading blocks this thread; the server answers on it. So read on another.
        const auto readOne = [&](QByteArray& out) {
            std::optional<QByteArray> got;
            std::thread reader([&] {
                QByteArray l;
                if (client.readLine(l))
                    got = l;
            });
            QElapsedTimer timer;
            timer.start();
            while (!got && timer.elapsed() < 5000)
                QTest::qWait(5);
            reader.join();
            if (!got)
                return false;
            out = *got;
            return true;
        };
        QVERIFY(readOne(line));
        QCOMPARE(parseObject(line).value(u"result"_s).toObject().value(u"protocolVersion"_s).toString(), u"2025-11-25"_s);
        QVERIFY(client.write(jsonrpc::notification(u"notifications/initialized"_s)));
        QVERIFY(client.write(callLine(2, u"search_files"_s, {{u"query"_s, u"管道"_s}})));
        QVERIFY(readOne(line));
        QCOMPARE(names(outcomeOf(line).structured), QStringList {u"管道.txt"_s});
        QCOMPARE(f.server->sessions().front().client, u"pipe-test"_s);
        // A changed policy is announced.
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        f.host->setPolicy(policy);
        QVERIFY(readOne(line));
        QCOMPARE(parseObject(line).value(u"method"_s).toString(), u"notifications/tools/list_changed"_s);
        client.close();
        QTRY_COMPARE(f.server->sessionCount(), 0);
        f.server->stop();
    }

    void relayOnline()
    {
        Fixture f;
        f.write(u"中文 路径.txt"_s, "r", 1'700'000'000);
        const std::wstring name = testPipeName("relay");
        QVERIFY(f.server->listen(name));
        Relay relay;
        QVERIFY2(relay.start(name), WINSHUN_MCP_RELAY);

        auto reply = relay.exchange(requestLine(1, u"initialize"_s,
            {{u"protocolVersion"_s, u"2025-06-18"_s}, {u"clientInfo"_s, QJsonObject {{u"name"_s, u"relay-test"_s}, {u"version"_s, u"1"_s}}}}));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"result"_s).toObject().value(u"protocolVersion"_s).toString(), u"2025-06-18"_s);
        relay.send(jsonrpc::notification(u"notifications/initialized"_s));
        QTRY_COMPARE(f.server->sessionCount(), 1);
        QTRY_COMPARE(f.server->sessions().front().client, u"relay-test"_s);

        reply = relay.exchange(requestLine(2, u"tools/list"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray().size(), 9);
        // The session has the relay's own token (ImpersonateNamedPipeClient);
        // that what its user cannot list stays out of the results is
        // relayRestricted()'s check.
        QVERIFY(f.server->sessions().front().identified);
        reply = relay.exchange(callLine(3, u"search_files"_s, {{u"query"_s, u"中文"_s}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"中文 路径.txt"_s});
        reply = relay.exchange(requestLine(4, u"ping"_s));
        QVERIFY(reply && parseObject(*reply).contains(u"result"_s));

        // Win顺 goes away: the relay answers for it, then finds it again.
        f.server->stop();
        QTRY_COMPARE(f.server->sessionCount(), 0);
        reply = relay.exchange(callLine(5, u"search_files"_s, {{u"query"_s, u"中文"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        // "not running", or, with a Win顺 running on this PC that does not
        // serve the pipe (an older one, or its AI connection off), that.
        QVERIFY2(out.text.contains(u"not running") || out.text.contains(u"AI connection is off"), qPrintable(out.text));
        reply = relay.exchange(callLine(6, u"index_status"_s));
        QVERIFY(reply);
        QVERIFY(!outcomeOf(*reply).structured.value(u"connected"_s).toBool()); // running: whatever this PC has
        reply = relay.exchange(requestLine(7, u"tools/list"_s));
        QVERIFY(reply);
        QVERIFY(parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray().size() >= 1); // cached, or index_status alone

        QVERIFY(f.server->listen(name));
        reply = relay.exchange(callLine(8, u"search_files"_s, {{u"query"_s, u"中文"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"中文 路径.txt"_s});
        QTRY_COMPARE(f.server->sessions().front().client, u"relay-test"_s); // initialized again by the relay

        relay.finish();
        QCOMPARE(relay.exitCode, DWORD {0});
        QTRY_COMPARE(f.server->sessionCount(), 0);
        f.server->stop();
    }

    // The session has the relay's token (ImpersonateNamedPipeClient): what
    // the relay's user cannot list is not among the results. The relay is
    // started here with this process's token, as an AI client would start
    // it with the user's; in an elevated test run it inherits the
    // administrator's, which sees the restricted folder, so the check
    // means nothing there and is skipped (the real relay is never elevated:
    // AI clients run as the user).
    void relayRestricted()
    {
        if (elevated())
            QSKIP("elevated: the relay inherits the administrator token and sees the restricted folder too");
        Fixture f;
        f.write(u"中文 路径.txt"_s, "r", 1'700'000'000);
        f.write(u"secret\\中文 hidden.txt"_s, "s", 1'700'000'100);
        QVERIFY(f.restrict(u"secret"_s)); // administrators alone: not for the relay's user
        const std::wstring name = testPipeName("restricted");
        QVERIFY(f.server->listen(name));
        Relay relay;
        QVERIFY2(relay.start(name), WINSHUN_MCP_RELAY);
        auto reply = relay.exchange(requestLine(1, u"initialize"_s,
            {{u"protocolVersion"_s, u"2025-06-18"_s}, {u"clientInfo"_s, QJsonObject {{u"name"_s, u"relay-test"_s}, {u"version"_s, u"1"_s}}}}));
        QVERIFY(reply);
        relay.send(jsonrpc::notification(u"notifications/initialized"_s));
        QTRY_COMPARE(f.server->sessionCount(), 1);
        QVERIFY(f.server->sessions().front().identified);
        reply = relay.exchange(callLine(2, u"search_files"_s, {{u"query"_s, u"中文"_s}}));
        QVERIFY(reply);
        const Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"中文 路径.txt"_s});
        relay.finish();
        QCOMPARE(relay.exitCode, DWORD {0});
        QTRY_COMPARE(f.server->sessionCount(), 0);
        f.server->stop();
    }

    void relayOffline()
    {
        Relay relay;
        QVERIFY(relay.start(testPipeName("nobody")));
        auto reply = relay.exchange(requestLine(1, u"initialize"_s, {{u"protocolVersion"_s, u"2025-11-25"_s}}));
        QVERIFY(reply);
        const QJsonObject result = parseObject(*reply).value(u"result"_s).toObject();
        QCOMPARE(result.value(u"protocolVersion"_s).toString(), u"2025-11-25"_s);
        QVERIFY(result.value(u"instructions"_s).toString().contains(u"not running"));
        relay.send(jsonrpc::notification(u"notifications/initialized"_s));
        reply = relay.exchange(requestLine(2, u"tools/list"_s));
        QVERIFY(reply);
        bool hasStatus = false;
        for (const QJsonValue& tool : parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray())
            hasStatus = hasStatus || tool.toObject().value(u"name"_s).toString() == u"index_status";
        QVERIFY(hasStatus);
        reply = relay.exchange(callLine(3, u"search_files"_s, {{u"query"_s, u"x"_s}}));
        QVERIFY(reply);
        const Outcome out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"Win顺") && (out.text.contains(u"not running") || out.text.contains(u"AI connection is off")),
            qPrintable(out.text));
        reply = relay.exchange("garbage\n");
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kParseError);
        reply = relay.exchange(requestLine(4, u"something/else"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kMethodNotFound);
        relay.finish();
        QCOMPARE(relay.exitCode, DWORD {0});
    }

    // The plugins' commands as tools (plugin::AiBridge): none while the
    // switch is off; queries once it is on, from a zero-code plugin and from
    // a resident process through the runner; actions only with their own
    // switch; a plugin turned off is known but off, with words that name it.
    void pluginTools()
    {
        Fixture f;
        QTemporaryDir dir;
        plugin::Host::Options options;
        options.userPluginDir = dir.path() + u"/plugins"_s;
        options.dataRoot = dir.path() + u"/data"_s;
        options.runnerExe = QString::fromUtf8(WINSHUN_PLUGIN_HOST);
        options.version = u"0.0-test"_s;
        plugin::Host host(options);
        const auto install = [&](const QString& id, const QByteArray& manifest, bool fake) {
            const QString folder = options.userPluginDir + u'/' + id;
            QDir().mkpath(folder);
            if (fake)
                QVERIFY(QFile::copy(QString::fromUtf8(WINSHUN_PLUGIN_FAKE), folder + u"/fake.exe"_s));
            QFile file(folder + u"/plugin.json"_s);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(manifest);
        };
        // (Addresses spelt with /: a raw string with // in it is a comment to moc.)
        install(u"zero.items"_s,
            R"({"manifest":1,"id":"zero.items","name":"Zero","version":"1.0.0","ai":{"expose":true},
                "commands":[{"id":"list","keyword":"zi","title":"List","run":{"items":[
                  {"id":"a","title":"Alpha","subtitle":"first"},
                  {"id":"b","title":"Beta","subtitle":"second","open":{"url":"https://example.com/b"}}]}}]})",
            false);
        install(u"fake.serve"_s,
            R"({"manifest":1,"id":"fake.serve","name":"Fake","version":"1.0.0","ai":{"expose":true},
                "runtime":{"mode":"resident","program":"fake.exe","args":["--serve"]},
                "commands":[{"id":"main","keyword":"fk","title":"Main","description":"Echoes the words",
                  "actions":[{"id":"open","title":"Open","key":"Enter"},{"id":"do","title":"Do","key":"Ctrl+Enter"}]}]})",
            true);
        install(u"no.ai"_s,
            R"({"manifest":1,"id":"no.ai","name":"Private","version":"1.0.0","commands":[{"id":"main","keyword":"na","run":{"url":"https://example.com/?q=%s"}}]})",
            false);
        host.catalog().scan();
        for (const QString& id : {u"zero.items"_s, u"fake.serve"_s, u"no.ai"_s})
            host.setEnabled(id, true);
        plugin::AiBridge bridge(&host, f.host.get());

        Session s(f.server.get());
        QVERIFY(s.initialize());
        const auto toolNames = [&] {
            QStringList names;
            const auto reply = s.exchange(requestLine(2, u"tools/list"_s));
            if (!reply)
                return names;
            for (const QJsonValue& tool : parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray())
                names.push_back(tool.toObject().value(u"name"_s).toString());
            return names;
        };
        const auto pluginToolsOf = [](const QStringList& names) { return names.filter(u"plugin_"_s); };

        // Off: nothing of the plugins'.
        QVERIFY(pluginToolsOf(toolNames()).isEmpty());
        QCOMPARE(f.host->known(u"plugin_zero_items_list"_s), ToolHost::Known::No);

        // On: a tool per command of the plugins that expose themselves, queries only.
        QSignalSpy changed(f.host.get(), &ToolHost::toolsChanged);
        plugin::AiBridge::Options opts;
        opts.enabled = true;
        bridge.setOptions(opts);
        QCOMPARE(changed.count(), 1);
        QVERIFY(std::any_of(s.received.cbegin(), s.received.cend(), [](const QByteArray& line) { return line.contains("notifications/tools/list_changed"); }));
        QStringList names = pluginToolsOf(toolNames());
        names.sort();
        QCOMPARE(names, (QStringList {u"plugin_fake_serve_main"_s, u"plugin_zero_items_list"_s}));
        {
            const auto reply = s.exchange(requestLine(3, u"tools/list"_s));
            QVERIFY(reply);
            for (const QJsonValue& tool : parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray()) {
                const QJsonObject t = tool.toObject();
                if (!t.value(u"name"_s).toString().startsWith(u"plugin_"))
                    continue;
                QVERIFY(t.value(u"annotations"_s).toObject().value(u"readOnlyHint"_s).toBool());
                QVERIFY(t.value(u"inputSchema"_s).toObject().value(u"properties"_s).toObject().contains(u"query"_s));
                QVERIFY(t.value(u"description"_s).toString().contains(u"Win顺"));
            }
        }

        // A zero-code command answers in place; a resident one through its process.
        auto reply = s.exchange(callLine(10, u"plugin_zero_items_list"_s, {{u"query"_s, u"beta"_s}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 1);
        QCOMPARE(out.structured.value(u"items"_s).toArray().first().toObject().value(u"title"_s).toString(), u"Beta"_s);
        QCOMPARE(out.structured.value(u"items"_s).toArray().first().toObject().value(u"url"_s).toString(), u"https://example.com/b"_s);
        reply = s.exchange(callLine(11, u"plugin_fake_serve_main"_s, {{u"query"_s, u"hi 你好"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        const QJsonObject served = out.structured.value(u"items"_s).toArray().first().toObject();
        QCOMPARE(served.value(u"title"_s).toString(), u"serve: hi 你好"_s);
        QVERIFY(!served.contains(u"data"_s)); // the plugin's own, never the model's
        QVERIFY2(f.log.last().contains(u"plugin_fake_serve_main") && f.log.last().contains(u"1 items"), qPrintable(f.log.last()));

        // Actions: not until their switch; then a _run tool for commands with a process (never zero-code).
        QCOMPARE(f.host->known(u"plugin_fake_serve_main_run"_s), ToolHost::Known::No);
        opts.actions = true;
        bridge.setOptions(opts);
        names = pluginToolsOf(toolNames());
        QVERIFY(names.contains(u"plugin_fake_serve_main_run"_s));
        QVERIFY(!names.contains(u"plugin_zero_items_list_run"_s));
        reply = s.exchange(callLine(12, u"plugin_fake_serve_main_run"_s, {{u"query"_s, u"hi"_s}, {u"item_id"_s, u"nothing"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(out.isError && out.text.contains(u"No row with id"), qPrintable(out.text));
        reply = s.exchange(callLine(13, u"plugin_fake_serve_main_run"_s, {{u"query"_s, u"hi"_s}, {u"item_id"_s, u"q-hi"_s}, {u"action"_s, u"do"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(out.structured.value(u"done"_s).toBool());
        QVERIFY2(out.structured.value(u"message"_s).toString() == u"did do on serve: hi"_s, qPrintable(out.structured.value(u"message"_s).toString()));

        // A plugin turned off: its tools known but off, the words naming it.
        host.setEnabled(u"fake.serve"_s, false);
        names = pluginToolsOf(toolNames());
        QVERIFY(!names.contains(u"plugin_fake_serve_main"_s));
        QCOMPARE(f.host->known(u"plugin_fake_serve_main"_s), ToolHost::Known::Off);
        reply = s.exchange(callLine(14, u"plugin_fake_serve_main"_s, {{u"query"_s, u"hi"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"'Fake'") && out.text.contains(u"turned off"), qPrintable(out.text));

        // The card's switch: a plugin kept from the AI is not even known.
        opts.excluded.insert(u"zero.items"_s);
        bridge.setOptions(opts);
        QVERIFY(pluginToolsOf(toolNames()).isEmpty());
        QCOMPARE(f.host->known(u"plugin_zero_items_list"_s), ToolHost::Known::No);

        // The switch off again: nothing was on offer any more, so no news.
        const qsizetype before = changed.count();
        bridge.setOptions({});
        QCOMPARE(changed.count(), before);
        host.shutdown();
    }
};

QTEST_GUILESS_MAIN(TestMcp)
#include "tst_mcp.moc"
