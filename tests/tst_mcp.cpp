// The MCP server (src/mcp): JSON-RPC framing, the tools on a hand-made index
// over a temporary folder, the lifecycle through an in-process session,
// the named pipe, and the relay WinShunMcp.exe driven over stdio, with
// Win顺 there and away; and the plugins' commands as tools (plugin::AiBridge).

#include "AiBridge.h"
#include "Aside.h"
#include "ContentScanner.h"
#include "FileIndex.h"
#include "Host.h"
#include "IndexService.h"
#include "IsoTime.h"
#include "JsonRpc.h"
#include "McpServer.h"
#include "McpTools.h"
#include "Pipe.h"
#include "Sandbox.h"
#include "SearchEngine.h"
#include "ShareIndexer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTimeZone>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
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

// Content searches' engine (Services::contentEngine): one of their own on
// the same index, as the app's host has; none, as the plugins' host (they
// queue on the one engine); or one on an index of its own, empty, that a
// test can hold locked to keep a content search where it starts.
enum class ContentEngine { Own, None, Locked };

// The work of calls aside, as it begins (Services::asideStarting): every
// job's tool and thread are noted, and the next `count` of one tool are held
// where they begin until released, or until told to stop (the call
// cancelled, the host going), which then takes them `slowStop` ms, as a
// check on a share that went away would.
struct Hold {
    std::mutex mutex;
    std::condition_variable_any wake;
    QString tool;
    int count = 0; // of `tool`, still to be held
    bool open = true;
    int waiting = 0; // held now
    int slowStop = 0;
    QStringList started;
    std::vector<std::thread::id> threads;

    void starting(const QString& name, const std::stop_token& stop)
    {
        std::unique_lock lock(mutex);
        started.push_back(name);
        threads.push_back(std::this_thread::get_id());
        if (name != tool || count == 0)
            return;
        --count;
        ++waiting;
        wake.wait(lock, stop, [this] { return open; });
        --waiting;
        const int pause = slowStop;
        lock.unlock();
        if (stop.stop_requested() && pause > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(pause));
    }
    void hold(const QString& name, int n = 1)
    {
        const std::lock_guard lock(mutex);
        tool = name;
        count = n;
        open = false;
    }
    void release()
    {
        {
            const std::lock_guard lock(mutex);
            open = true;
            count = 0;
        }
        wake.notify_all();
    }
    int held()
    {
        const std::lock_guard lock(mutex);
        return waiting;
    }
    int startedOf(const QString& name)
    {
        const std::lock_guard lock(mutex);
        return static_cast<int>(started.count(name));
    }
    bool ranOn(std::thread::id thread)
    {
        const std::lock_guard lock(mutex);
        return std::ranges::find(threads, thread) != threads.end();
    }
};

// Network shares taken to be out of reach now (Services::offline), as the
// share indexer would say of a server that went away. Any thread.
struct OfflineShares {
    std::mutex mutex;
    QStringList shares; // "\\host\share", case-folded

    void set(const QStringList& list)
    {
        const std::lock_guard lock(mutex);
        shares.clear();
        for (const QString& share : list)
            shares.push_back(share.toCaseFolded());
    }
    bool has(const QString& path)
    {
        const QString folded = path.toCaseFolded();
        const std::lock_guard lock(mutex);
        return std::ranges::any_of(shares, [&](const QString& share) {
            return folded == share || (folded.startsWith(share) && folded.at(share.size()) == u'\\');
        });
    }
};

// A folder on disk and the same in a hand-made index: the service is not
// started (it reads no volume), its index is filled by hand to mirror the
// folder, and the engines, host and server run on it as in the app.
struct Fixture {
    QTemporaryDir dir;
    QString root; // the folder, native
    std::unique_ptr<IndexService> service;
    std::unique_ptr<SearchEngine> engine;
    std::unique_ptr<IndexService> contentService; // ContentEngine::Locked: its own index
    std::unique_ptr<SearchEngine> contentEngine;
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
    // Shared with the host's services, which may outlive the test's frame
    // on the jobs' threads.
    std::shared_ptr<Hold> hold = std::make_shared<Hold>();
    std::shared_ptr<OfflineShares> offline = std::make_shared<OfflineShares>();

    explicit Fixture(bool withShell = true, bool withClipboard = true, ContentEngine content = ContentEngine::Own)
    {
        root = QDir::toNativeSeparators(dir.path());
        IndexService::Options options;
        options.content.enabled = false;
        service = std::make_unique<IndexService>(dir.path() + u"/index"_s, options);
        folder = addFolder(root);
        engine = std::make_unique<SearchEngine>(service.get());
        if (content == ContentEngine::Locked)
            contentService = std::make_unique<IndexService>(dir.path() + u"/content-index"_s, options);
        if (content != ContentEngine::None)
            contentEngine = std::make_unique<SearchEngine>(contentService ? contentService.get() : service.get());
        Services services;
        services.index = service.get();
        services.engine = engine.get();
        services.contentEngine = contentEngine.get();
        services.version = u"0.0-test"_s;
        services.log = [this](const QString& line) { log.push_back(line); };
        services.offline = [offline = offline](const QString& path) { return offline->has(path); };
        services.asideStarting = [hold = hold](const QString& tool, const std::stop_token& stop) { hold->starting(tool, stop); };
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
    ~Fixture()
    {
        hold->release();
        unrestrict();
    }

    // A network share in the index, as the share indexer keeps one: its
    // root ("\\host\share") and entries under it, in the index alone.
    EntryId addShare(const QString& share, const QStringList& relatives, FileTime modified)
    {
        const auto index = service->index();
        const auto lock = index->writeLock();
        const EntryId top = ShareIndexer::ensureRoot(*index, share.toStdWString());
        for (const QString& relative : relatives) {
            const QStringList parts = relative.split(u'\\', Qt::SkipEmptyParts);
            EntryId at = top;
            for (qsizetype i = 0; i < parts.size(); ++i) {
                const bool folder = i + 1 < parts.size() || relative.endsWith(u'\\');
                const std::string name = parts[i].toStdString();
                EntryId child = index->findChild(at, name);
                if (child == kNoEntry) {
                    child = index->add(at, name,
                        static_cast<std::uint8_t>(EntryFlag::Remote | (folder ? EntryFlag::Directory : 0)));
                    index->setModified(child, modified);
                }
                at = child;
            }
        }
        return top;
    }

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
        // What ws checks before relying on the command line's contract: an integer.
        const QJsonValue contract =
            result.value(u"capabilities"_s).toObject().value(u"experimental"_s).toObject().value(u"winshun"_s).toObject().value(u"cli"_s);
        QVERIFY(contract.isDouble());
        QCOMPARE(contract.toInt(), kCliContract);
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
    }

    // Win顺's clipboard history off (设置 → 剪贴板 → 记录剪贴板历史), AI's
    // switch for it on: there is nothing to read, so the three tools are off
    // and name the history's switch; index_status says it is not readable
    // (ws status shows that).
    void clipboardHistoryOff()
    {
        Fixture f;
        f.clips.push_back({1, u"text"_s, u"hello world"_s, 11, 1'700'000'000'000, 1'700'000'000'000, u"notepad"_s, {}, false, 0, 0});
        Policy policy = f.host->policy();
        policy.allowClipboard = true;
        policy.captureEnabled = true;
        policy.clipboardHistory = false;
        f.host->setPolicy(policy);
        const QStringList tools {u"clipboard_history"_s, u"clipboard_get"_s, u"screenshot_history"_s};
        for (const QString& tool : tools)
            QVERIFY2(!f.host->toolNames().contains(tool), qPrintable(tool));
        Session ai(f.server.get());
        QVERIFY(ai.initialize());
        Session cli(f.server.get());
        QVERIFY(cli.initialize(u"2025-11-25"_s, u"ws"_s));
        const auto status = [&](int id) {
            const auto reply = cli.exchange(callLine(id, u"index_status"_s));
            return reply ? outcomeOf(*reply).structured.value(u"ai"_s).toObject() : QJsonObject();
        };
        int id = 2;
        for (const QString& tool : tools) {
            const QJsonObject arguments = tool == u"clipboard_get" ? QJsonObject {{u"id"_s, 1}} : QJsonObject();
            auto reply = cli.exchange(callLine(id++, tool, arguments));
            QVERIFY(reply);
            Outcome out = outcomeOf(*reply);
            QVERIFY(out.isError);
            QCOMPARE(out.structured.value(u"error"_s).toString(), u"off"_s); // ws: exit 4
            QCOMPARE(out.structured.value(u"setting"_s).toString(), u"clipboard_history"_s); // ... naming the history's switch
            QVERIFY2(out.text.contains(u"设置 → 剪贴板 → “记录剪贴板历史”"), qPrintable(out.text));
            reply = ai.exchange(callLine(id++, tool, arguments));
            QVERIFY(reply);
            out = outcomeOf(*reply);
            QVERIFY(out.isError);
            QVERIFY2(out.text.contains(u"记录剪贴板历史"), qPrintable(out.text));
        }
        QJsonObject state = status(id++);
        QVERIFY(!state.value(u"clipboard_allowed"_s).toBool(true));
        QVERIFY(!state.value(u"clipboard_history"_s).toBool(true));

        // The history back on: there to read.
        policy.clipboardHistory = true;
        f.host->setPolicy(policy);
        auto reply = cli.exchange(callLine(id++, u"clipboard_history"_s));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 1);
        state = status(id++);
        QVERIFY(state.value(u"clipboard_allowed"_s).toBool());
        QVERIFY(state.value(u"clipboard_history"_s).toBool());

        // Both off: AI's own switch is the one to name.
        policy.allowClipboard = false;
        policy.clipboardHistory = false;
        f.host->setPolicy(policy);
        reply = ai.exchange(callLine(id++, u"clipboard_history"_s));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"AI 助手") && !out.text.contains(u"记录剪贴板历史"), qPrintable(out.text));
        QVERIFY(!out.structured.contains(u"setting"_s)); // an AI client reads the words
        // ... and so does the setting name it, to ws, whether the history is kept or not.
        for (const bool kept : {false, true}) {
            policy.clipboardHistory = kept;
            f.host->setPolicy(policy);
            reply = cli.exchange(callLine(id++, u"clipboard_history"_s));
            QVERIFY(reply);
            out = outcomeOf(*reply);
            QVERIFY(out.isError);
            QCOMPARE(out.structured.value(u"error"_s).toString(), u"off"_s);
            QCOMPARE(out.structured.value(u"setting"_s).toString(), u"ai_clipboard"_s);
        }
        // A tool with one switch names none.
        QCOMPARE(f.host->known(u"open_item"_s), ToolHost::Known::Off);
        reply = cli.exchange(callLine(id++, u"open_item"_s, {{u"path"_s, f.root}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QCOMPARE(out.structured.value(u"error"_s).toString(), u"off"_s);
        QVERIFY(!out.structured.contains(u"setting"_s));
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

    // ws's answers of thousands of items in hundreds of folders: the folders
    // are checked many at once, a batch of rows at a time (AsideContext::
    // seeAhead), and each item is decided as one checked alone: what a
    // folder that cannot be listed holds is left out, the rest come in the
    // engine's order.
    void manyFolders()
    {
        Fixture f;
        // 160 folders of 40 names (in the index alone: the folder's check
        // decides), a quarter of them in one that will be restricted: 6 400
        // items, two batches.
        for (int d = 0; d < 160; ++d) {
            const QString relative = (d % 4 == 0 ? u"locked\\d%1"_s : u"open\\d%1"_s).arg(d, 3, 10, u'0');
            const EntryId folder = f.subfolder(relative, 1'700'000'000);
            const auto index = f.service->index();
            const auto lock = index->writeLock();
            for (int i = 0; i < 40; ++i) {
                const QString name = u"kiwi-%1-%2.txt"_s.arg(d, 3, 10, u'0').arg(i, 2, 10, u'0');
                index->setModified(index->add(folder, name.toStdString(), 0), 1'700'000'000 + d * 40 + i);
            }
        }
        Session cli(f.server.get());
        QVERIFY(cli.initialize(u"2025-11-25"_s, u"ws"_s));
        int id = 2;
        const auto paths = [&] {
            const QJsonObject arguments {
                {u"query"_s, u"kiwi"_s}, {u"limit"_s, 50'000}, {u"size"_s, false}, {u"kind"_s, u"files"_s}};
            const auto reply = cli.exchange(callLine(id++, u"search_files"_s, arguments), 60000);
            QStringList out;
            if (reply) {
                for (const QJsonValue& item : outcomeOf(*reply).structured.value(u"items"_s).toArray())
                    out.push_back(item.toObject().value(u"path"_s).toString());
            }
            return out;
        };
        const QStringList all = paths();
        QCOMPARE(all.size(), 6'400);
        QVERIFY(f.restrict(u"locked"_s));
        QStringList open;
        for (const QString& path : all) {
            if (!path.contains(u"\\locked\\"_s))
                open.push_back(path);
        }
        QCOMPARE(open.size(), 4'800);
        QCOMPARE(paths(), open);
        QVERIFY2(f.log.last().contains(u"access checks"), qPrintable(f.log.last()));
    }

    // The times of the items (IsoTime.h): made by hand for the many of one
    // answer, the offset asked once a day, the same text as QDateTime's.
    void localTimes()
    {
        // At an offset: as QDateTime writes it there ("Z" for 0).
        const int offsets[] {0, 3'600, -3'600, 8 * 3'600, -5 * 3'600, 19'800, 20'700, -34'200, 50'400, -43'200, 2'700};
        for (qint64 secs = 1; secs <= 0xFFFF'FFFFll; secs += 9'999'991) {
            for (const int offset : offsets) {
                QCOMPARE(isoText(secs, offset),
                    QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC).toOffsetFromUtc(offset).toString(Qt::ISODate));
            }
        }
        for (const qint64 secs : {qint64 {1}, qint64 {86'399}, qint64 {86'400}, qint64 {951'782'400},
                 qint64 {4'102'444'799}, qint64 {0xFFFF'FFFFll}}) {
            for (const int offset : offsets) {
                QCOMPARE(isoText(secs, offset),
                    QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC).toOffsetFromUtc(offset).toString(Qt::ISODate));
            }
        }
        QCOMPARE(isoText(1'700'000'000, 0), u"2023-11-14T22:13:20Z"_s);
        QCOMPARE(isoText(1'700'000'000, 8 * 3'600), u"2023-11-15T06:13:20+08:00"_s);

        // The system's zone, as the tools gave each one before (isoTime);
        // nothing for 0 and before (not known).
        LocalTimes local;
        QVERIFY(local.iso(0).isNull());
        QVERIFY(local.iso(-86'400).isNull());
        for (qint64 secs = 1; secs <= 0xFFFF'FFFFll; secs += 86'399 * 3 + 1'234)
            QCOMPARE(local.iso(secs), isoTime(secs));

        // Zones that change their offset, by the hour around each change
        // since 2000 and at the second it comes: those in the middle of a UTC
        // day (Lord Howe moves 30 minutes at 15:30 UTC) go time by time.
        int changesSeen = 0;
        for (const char* id :
            {"Europe/Berlin", "America/St_Johns", "Australia/Lord_Howe", "America/Sao_Paulo", "Asia/Kathmandu"}) {
            const QTimeZone zone {QByteArray(id)};
            if (!zone.isValid())
                continue;
            LocalTimes times(
                [&](qint64 secs) { return zone.offsetFromUtc(QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC)); });
            const auto expected = [&](qint64 secs) {
                const QDateTime at = QDateTime::fromSecsSinceEpoch(secs, zone);
                return QJsonValue(at.toOffsetFromUtc(at.offsetFromUtc()).toString(Qt::ISODate));
            };
            const auto changes = zone.transitions(QDateTime::fromSecsSinceEpoch(946'684'800, QTimeZone::UTC),
                QDateTime::fromSecsSinceEpoch(1'893'456'000, QTimeZone::UTC));
            for (const QTimeZone::OffsetData& change : changes) {
                ++changesSeen;
                const qint64 at = change.atUtc.toSecsSinceEpoch();
                for (qint64 secs = at - 2 * 86'400; secs <= at + 2 * 86'400; secs += 3'600)
                    QCOMPARE(times.iso(secs), expected(secs));
                for (const qint64 secs : {at - 61, at - 1, at, at + 1, at + 61})
                    QCOMPARE(times.iso(secs), expected(secs));
            }
            for (qint64 secs = 946'684'800; secs < 1'893'456'000; secs += 86'399 * 11 + 7)
                QCOMPARE(times.iso(secs), expected(secs));
        }
        QVERIFY(changesSeen > 0);
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

        // Three in a row all answer: each finished aside once the engine is
        // done with it, so in any order.
        const qsizetype start = s.received.size();
        for (int i = 4; i <= 6; ++i)
            f.server->receive(s.id, callLine(i, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        QTRY_COMPARE(s.received.size(), start + 3);
        QList<int> ids;
        for (int i = 0; i < 3; ++i)
            ids.push_back(parseObject(s.received.at(start + i)).value(u"id"_s).toInt());
        std::ranges::sort(ids);
        QCOMPARE(ids, (QList<int> {4, 5, 6}));

        // A session that ends takes its calls with it.
        {
            Session other(f.server.get());
            QVERIFY(other.initialize());
            f.server->receive(other.id, callLine(7, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        }
        QTest::qWait(200);
        QCOMPARE(f.host->pendingCalls(), 0);
    }

    // A name search answers while a content search is under way: each kind
    // has an engine of its own (Services::contentEngine). The content
    // engine here reads an index of its own that the test holds locked: a
    // content search stops where it starts, after its opening (empty)
    // resultsReady, which must not finish a name search (the two engines'
    // request ids both start at 1). Cancelling and ending a session reach the
    // search on the right engine.
    void namesBesideContent()
    {
        Fixture f(true, true, ContentEngine::Locked);
        f.write(u"lane.txt"_s, "lane", 1'700'000'000);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        std::optional<std::unique_lock<std::shared_mutex>> lock(f.contentService->index()->writeLock());
        const QJsonObject content {{u"query"_s, u"lane"_s}, {u"timeout_ms"_s, 30000}};
        const qsizetype start = s.received.size();
        f.server->receive(s.id, callLine(2, u"search_content"_s, content));
        auto reply = s.exchange(callLine(3, u"search_files"_s, {{u"query"_s, u"lane"_s}}));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"id"_s).toInt(), 3);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"lane.txt"_s});
        QCOMPARE(s.received.size(), start + 1); // nothing for the content search yet
        QCOMPARE(f.host->pendingCalls(), 1);

        // A second content search waits behind the first; cancelled, the
        // first makes way for it. Names answer all along.
        f.server->receive(s.id, callLine(4, u"search_content"_s, content));
        QCOMPARE(f.host->pendingCalls(), 2);
        f.server->receive(s.id, jsonrpc::notification(u"notifications/cancelled"_s, {{u"requestId"_s, 2}}));
        QCOMPARE(f.host->pendingCalls(), 1);
        reply = s.exchange(callLine(5, u"search_files"_s, {{u"query"_s, u"lane"_s}}));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"id"_s).toInt(), 5);
        // A name search queued behind another and cancelled never answers;
        // the content search is left alone.
        const qsizetype mark = s.received.size();
        f.server->receive(s.id, callLine(6, u"search_files"_s, {{u"query"_s, u"lane"_s}}));
        f.server->receive(s.id, callLine(7, u"search_files"_s, {{u"query"_s, u"lane"_s}}));
        f.server->receive(s.id, jsonrpc::notification(u"notifications/cancelled"_s, {{u"requestId"_s, 7}}));
        QTRY_COMPARE(s.received.size(), mark + 1);
        QTest::qWait(200);
        QCOMPARE(s.received.size(), mark + 1);
        QCOMPARE(parseObject(s.received.last()).value(u"id"_s).toInt(), 6);
        QCOMPARE(f.host->pendingCalls(), 1);

        // Let go: the content search that is left answers (its index is
        // empty: nothing found, every candidate looked at), and only it.
        lock.reset();
        QTRY_COMPARE_WITH_TIMEOUT(s.received.size(), mark + 2, 10000);
        QCOMPARE(parseObject(s.received.last()).value(u"id"_s).toInt(), 4);
        Outcome out = outcomeOf(s.received.last());
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(out.structured.value(u"complete"_s).toBool());
        QTest::qWait(200);
        QCOMPARE(s.received.size(), mark + 2);
        QCOMPARE(f.host->pendingCalls(), 0);

        // A session that ends takes its content search with it.
        lock.emplace(f.contentService->index()->writeLock());
        {
            Session other(f.server.get());
            QVERIFY(other.initialize());
            f.server->receive(other.id, callLine(2, u"search_content"_s, content));
            QCOMPARE(f.host->pendingCalls(), 1);
        }
        QCOMPARE(f.host->pendingCalls(), 0);
        reply = s.exchange(callLine(8, u"search_files"_s, {{u"query"_s, u"lane"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"lane.txt"_s});
        lock.reset();
        // ... and the content engine takes the next one.
        reply = s.exchange(callLine(9, u"search_content"_s, {{u"query"_s, u"lane"_s}}), 10000);
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"id"_s).toInt(), 9);
        QVERIFY(outcomeOf(*reply).structured.value(u"complete"_s).toBool());
    }

    // Without an engine for content searches (the plugins' host), every
    // search queues on the one engine: a name search made just after a
    // content search waits for it, rather than cancelling it (an engine runs
    // only its newest request).
    void oneEngine()
    {
        Fixture f(true, true, ContentEngine::None);
        f.write(u"one.txt"_s, "the word kumquat", 1'700'000'000);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        const qsizetype start = s.received.size();
        f.server->receive(s.id, callLine(2, u"search_content"_s, {{u"query"_s, u"kumquat"_s}}));
        f.server->receive(s.id, callLine(3, u"search_files"_s, {{u"query"_s, u"one"_s}}));
        QTRY_COMPARE_WITH_TIMEOUT(s.received.size(), start + 2, 20000);
        // Each is finished aside once the engine is done with it: the two
        // answers may come in either order.
        const auto answerTo = [&](int id) {
            for (qsizetype i = start; i < s.received.size(); ++i) {
                if (parseObject(s.received.at(i)).value(u"id"_s).toInt() == id)
                    return outcomeOf(s.received.at(i));
            }
            return Outcome {true, u"(no answer)"_s};
        };
        const Outcome content = answerTo(2);
        QVERIFY2(!content.isError, qPrintable(content.text));
        QVERIFY(content.structured.value(u"complete"_s).toBool());
        QCOMPARE(names(content.structured), QStringList {u"one.txt"_s});
        QCOMPARE(names(answerTo(3).structured), QStringList {u"one.txt"_s});
    }

    // ws, the command line: known by its client name. Higher limits, items
    // without the name (the path ends with it), the result once (no text
    // copy), the kind of an error; not listed among the sessions, counted.
    void cliSession()
    {
        Fixture f;
        // 600 names in one folder, in the index alone (size: false reads no disk).
        const EntryId many = f.subfolder(u"many"_s, 1'700'000'000);
        {
            const auto index = f.service->index();
            const auto lock = index->writeLock();
            for (int i = 0; i < 600; ++i) {
                const EntryId id = index->add(many, u"item-%1.log"_s.arg(i, 3, 10, u'0').toStdString(), 0);
                index->setModified(id, 1'700'000'000 + i);
            }
        }
        Session ai(f.server.get());
        QVERIFY(ai.initialize());
        QSignalSpy listed(f.server.get(), &McpServer::sessionsChanged);
        QSignalSpy counted(f.server.get(), &McpServer::cliCalled);
        Session cli(f.server.get());
        QVERIFY(cli.initialize(u"2025-11-25"_s, u"ws"_s));

        const QJsonObject query {{u"query"_s, u"item"_s}, {u"limit"_s, 1000}, {u"size"_s, false}};
        auto reply = cli.exchange(callLine(2, u"search_files"_s, query));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 600); // an AI client: 200 at most
        QVERIFY(!out.structured.value(u"truncated"_s).toBool());
        QVERIFY(parseObject(*reply).value(u"result"_s).toObject().value(u"content"_s).toArray().isEmpty());
        QJsonObject item = out.structured.value(u"items"_s).toArray().first().toObject();
        QVERIFY2(item.value(u"path"_s).toString().startsWith(f.root + u"\\many\\item-"_s), qPrintable(item.value(u"path"_s).toString()));
        QCOMPARE(item.value(u"type"_s).toString(), u"file"_s);
        QVERIFY(item.contains(u"modified"_s));
        QVERIFY(!item.contains(u"name"_s));
        QVERIFY(!item.contains(u"size"_s));
        QCOMPARE(listed.count(), 0); // ws's sessions are not listed...
        QCOMPARE(counted.count(), 1); // ... but counted
        QVERIFY2(f.log.last().contains(u" ws search_files "), qPrintable(f.log.last()));

        // Its tools/list is the models' own: the limits there stay theirs.
        reply = cli.exchange(requestLine(90, u"tools/list"_s));
        QVERIFY(reply);
        const QJsonArray tools = parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray();
        const auto maximum = [&](const QString& tool, const QString& argument) {
            for (const QJsonValue& v : tools) {
                const QJsonObject t = v.toObject();
                if (t.value(u"name"_s).toString() == tool)
                    return t.value(u"inputSchema"_s).toObject().value(u"properties"_s).toObject().value(argument).toObject().value(u"maximum"_s).toInt();
            }
            return -1;
        };
        QCOMPARE(maximum(u"search_files"_s, u"limit"_s), 200);
        QCOMPARE(maximum(u"recent_files"_s, u"limit"_s), 200);
        QCOMPARE(maximum(u"list_folder"_s, u"limit"_s), 500);
        QCOMPARE(maximum(u"search_content"_s, u"limit"_s), 100);
        QCOMPARE(maximum(u"read_file_text"_s, u"max_chars"_s), 100'000);

        // The same asked by an AI client: its limit, names, and the text too.
        reply = ai.exchange(callLine(2, u"search_files"_s, query));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 200);
        QVERIFY(out.structured.value(u"truncated"_s).toBool());
        QVERIFY(!out.text.isEmpty());
        QVERIFY(out.structured.value(u"items"_s).toArray().first().toObject().contains(u"name"_s));

        // list_folder and recent_files past the AI's 500 and 200.
        reply = cli.exchange(callLine(3, u"list_folder"_s, {{u"path"_s, f.root + u"\\many"_s}, {u"limit"_s, 1000}, {u"size"_s, false}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 600);
        QVERIFY(!out.structured.value(u"items"_s).toArray().first().toObject().contains(u"name"_s));
        reply = ai.exchange(callLine(3, u"list_folder"_s, {{u"path"_s, f.root + u"\\many"_s}, {u"limit"_s, 1000}, {u"size"_s, false}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 500);
        reply = cli.exchange(callLine(4, u"recent_files"_s, {{u"folder"_s, f.root + u"\\many"_s}, {u"limit"_s, 1000}, {u"size"_s, false}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 600);
        QVERIFY(out.structured.value(u"items"_s).toArray().first().toObject().value(u"path"_s).toString().endsWith(u"item-599.log"_s));

        // search_content past the AI's 100 matches, read_file_text past its 100 000 characters.
        for (int i = 0; i < 120; ++i)
            f.write(u"docs\\note-%1.txt"_s.arg(i), "kumquat", 1'700'000'000);
        reply = cli.exchange(callLine(5, u"search_content"_s, {{u"query"_s, u"kumquat"_s}, {u"limit"_s, 1000}, {u"timeout_ms"_s, 30000}}), 40000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 120);
        item = out.structured.value(u"items"_s).toArray().first().toObject();
        QVERIFY(item.contains(u"snippet"_s) && !item.contains(u"name"_s));
        reply = ai.exchange(callLine(5, u"search_content"_s, {{u"query"_s, u"kumquat"_s}, {u"limit"_s, 1000}, {u"timeout_ms"_s, 30000}}), 40000);
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 100);
        const QString longText = f.write(u"long.txt"_s, QByteArray(150'000, 'a'), 1'700'000'000);
        reply = cli.exchange(callLine(6, u"read_file_text"_s, {{u"path"_s, longText}, {u"max_chars"_s, 200'000}}), 20000);
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"text"_s).toString().size(), 150'000);
        reply = ai.exchange(callLine(6, u"read_file_text"_s, {{u"path"_s, longText}, {u"max_chars"_s, 200'000}}), 20000);
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"text"_s).toString().size(), 100'000);

        // An error: the words for both, its kind for ws alone.
        reply = cli.exchange(callLine(7, u"list_folder"_s, {{u"path"_s, f.root + u"\\nowhere"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not in the index"), qPrintable(out.text));
        QCOMPARE(out.structured.value(u"error"_s).toString(), u"not_found"_s);
        reply = ai.exchange(callLine(7, u"list_folder"_s, {{u"path"_s, f.root + u"\\nowhere"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError && out.structured.isEmpty());
        reply = cli.exchange(callLine(8, u"read_file_text"_s, {{u"path"_s, f.root + u"\\missing.txt"_s}}), 20000);
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"error"_s).toString(), u"not_found"_s);
        reply = cli.exchange(callLine(9, u"open_item"_s, {{u"path"_s, f.root}})); // its switch is off
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"error"_s).toString(), u"off"_s);

        // Counted once per run (a session), however many calls reached a
        // tool; never listed, also as it goes.
        const auto sessions = f.server->sessions();
        QCOMPARE(sessions.size(), std::size_t {1});
        QCOMPARE(sessions.front().client, u"tst_mcp"_s);
        QCOMPARE(f.server->sessionCount(), 2);
        QCOMPARE(f.server->cliTally().calls, 1);
        QCOMPARE(f.server->cliTally().lastTool, u"read_file_text"_s);
        QVERIFY(f.server->cliTally().lastCall.isValid());
        QCOMPARE(counted.count(), 7); // told at every call that reached a tool
        const qsizetype before = listed.count();
        f.server->closeSession(cli.id);
        QCOMPARE(listed.count(), before);
        QCOMPARE(f.server->sessionCount(), 1);

        // A run is named by what it was run for, not by ws's index_status
        // probe before it; by the probe only when nothing else was asked.
        {
            Session run(f.server.get());
            QVERIFY(run.initialize(u"2025-11-25"_s, u"ws"_s));
            QVERIFY(run.exchange(callLine(2, u"index_status"_s)));
            QCOMPARE(f.server->cliTally().calls, 2);
            QVERIFY(run.exchange(callLine(3, u"search_files"_s, query)));
            QVERIFY(run.exchange(callLine(4, u"index_status"_s)));
            QCOMPARE(f.server->cliTally().calls, 2);
            QCOMPARE(f.server->cliTally().lastTool, u"search_files"_s);
        }
        {
            Session status(f.server.get());
            QVERIFY(status.initialize(u"2025-11-25"_s, u"ws"_s));
            QVERIFY(status.exchange(callLine(2, u"index_status"_s)));
            QVERIFY(status.exchange(callLine(3, u"index_status"_s)));
        }
        QCOMPARE(f.server->cliTally().calls, 3);
        QCOMPARE(f.server->cliTally().lastTool, u"index_status"_s); // ws status
    }

    // size: false reads no sizes from the disk (ws without -l: thousands of
    // reads saved); sorted by size, in the index's order, they come all the same.
    void sizeArgument()
    {
        Fixture f;
        const QString big = f.write(u"a-big.bin"_s, QByteArray(5000, 'x'), 1'700'000'000);
        const QString little = f.write(u"b-small.bin"_s, QByteArray(10, 'x'), 1'700'000'100);
        {
            const auto index = f.service->index();
            const auto lock = index->writeLock();
            index->setSize(index->findPath(big.toStdWString()), packSize(5000));
            index->setSize(index->findPath(little.toStdWString()), packSize(10));
        }
        Session s(f.server.get());
        QVERIFY(s.initialize());
        // The size of each item; -1: none.
        const auto sizes = [&](int id, const QString& tool, const QJsonObject& arguments) {
            QList<int> out;
            const auto reply = s.exchange(callLine(id, tool, arguments));
            if (!reply)
                return out;
            for (const QJsonValue& v : outcomeOf(*reply).structured.value(u"items"_s).toArray())
                out.push_back(v.toObject().contains(u"size"_s) ? v.toObject().value(u"size"_s).toInt() : -1);
            return out;
        };
        QCOMPARE(sizes(2, u"search_files"_s, {{u"query"_s, u"ext:bin"_s}, {u"sort"_s, u"name"_s}}), (QList<int> {5000, 10}));
        QCOMPARE(sizes(3, u"search_files"_s, {{u"query"_s, u"ext:bin"_s}, {u"sort"_s, u"name"_s}, {u"size"_s, false}}), (QList<int> {-1, -1}));
        QCOMPARE(sizes(4, u"search_files"_s, {{u"query"_s, u"ext:bin"_s}, {u"sort"_s, u"smallest"_s}, {u"size"_s, false}}), (QList<int> {10, 5000}));
        QCOMPARE(sizes(5, u"search_files"_s, {{u"query"_s, u"ext:bin"_s}, {u"sort"_s, u"largest"_s}, {u"size"_s, false}}), (QList<int> {5000, 10}));
        QCOMPARE(sizes(6, u"list_folder"_s, {{u"path"_s, f.root}}), (QList<int> {5000, 10}));
        QCOMPARE(sizes(7, u"list_folder"_s, {{u"path"_s, f.root}, {u"size"_s, false}}), (QList<int> {-1, -1}));
        QCOMPARE(sizes(8, u"recent_files"_s, {{u"folder"_s, f.root}}), (QList<int> {10, 5000}));
        QCOMPARE(sizes(9, u"recent_files"_s, {{u"folder"_s, f.root}, {u"size"_s, false}}), (QList<int> {-1, -1}));
        const auto reply = s.exchange(callLine(10, u"list_folder"_s, {{u"path"_s, f.root}, {u"size"_s, u"no"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        // In the schemas of the three.
        int described = 0;
        for (const QJsonValue& v : f.host->tools()) {
            const QJsonObject tool = v.toObject();
            const QJsonObject size = tool.value(u"inputSchema"_s).toObject().value(u"properties"_s).toObject().value(u"size"_s).toObject();
            if (!size.isEmpty()) {
                ++described;
                QCOMPARE(size.value(u"type"_s).toString(), u"boolean"_s);
                QVERIFY(size.value(u"default"_s).toBool());
                QVERIFY(size.value(u"description"_s).toString().contains(u"skips reading sizes from disk"));
            }
        }
        QCOMPARE(described, 3);
    }

    // search_files in some folders (ws find --in): only what lies in them,
    // the limit counting only that; folders AI must not see, and those the
    // caller cannot list, are not there for it; full paths only.
    void searchFilesInFolders()
    {
        Fixture f;
        for (const QString& folder : {u"甲"_s, u"乙"_s, u"丙"_s}) {
            for (int i = 0; i < 4; ++i)
                f.write(folder + u"\\plan-%1.txt"_s.arg(i), "p", 1'700'000'000 + i);
        }
        f.write(u"甲\\私\\plan-x.txt"_s, "p", 1'700'000'000);
        f.write(u"plan-top.txt"_s, "p", 1'700'000'000);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        const QString sep = u"\\"_s;
        const auto search = [&](int id, const QJsonArray& folders, int limit = 100) {
            const auto reply = s.exchange(callLine(id, u"search_files"_s, {{u"query"_s, u"plan"_s}, {u"folders"_s, folders}, {u"limit"_s, limit}}));
            return reply ? outcomeOf(*reply) : Outcome {true, u"(no answer)"_s};
        };
        const auto under = [&](const Outcome& out, const QStringList& folders) {
            for (const QJsonValue& v : out.structured.value(u"items"_s).toArray()) {
                const QString path = v.toObject().value(u"path"_s).toString();
                if (std::ranges::none_of(folders, [&](const QString& folder) { return path.startsWith(f.root + sep + folder + sep); }))
                    return false;
            }
            return true;
        };

        Outcome out = search(2, {f.root + u"\\甲"_s});
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 5); // and in the folders in it
        QVERIFY(under(out, {u"甲"_s}));
        // Two, one written with a separator at the end; the limit counts only theirs.
        out = search(3, {f.root + u"\\乙"_s, f.root + u"\\丙\\"_s});
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 8);
        QVERIFY(under(out, {u"乙"_s, u"丙"_s}));
        out = search(4, {f.root + u"\\乙"_s, f.root + u"\\丙"_s}, 3);
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 3);
        QVERIFY(out.structured.value(u"truncated"_s).toBool());
        QVERIFY(under(out, {u"乙"_s, u"丙"_s}));

        // A folder AI must not see: left out of one given, an error given itself.
        Policy policy = f.host->policy();
        policy.excludedPaths = {f.root + u"\\甲\\私"_s};
        f.host->setPolicy(policy);
        out = search(5, {f.root + u"\\甲"_s});
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 4);
        QVERIFY(under(out, {u"甲"_s}));
        for (const QJsonValue& v : out.structured.value(u"items"_s).toArray())
            QVERIFY(!v.toObject().value(u"path"_s).toString().contains(u"\\私\\"_s));
        out = search(6, {f.root + u"\\甲\\私"_s});
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not visible to AI"), qPrintable(out.text));
        // One the caller cannot list: as if it were not there.
        QVERIFY(f.restrict(u"丙"_s));
        out = search(7, {f.root + u"\\丙"_s});
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not in the index"), qPrintable(out.text));
        f.unrestrict();
        // Not there, a file, not a full path (relative to a current folder the
        // host does not know: ws makes its own full).
        out = search(8, {f.root + u"\\不在"_s});
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not in the index"), qPrintable(out.text));
        out = search(9, {f.root + u"\\乙\\plan-1.txt"_s});
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"is a file"), qPrintable(out.text));
        int id = 10;
        for (const QString& relative : {u"乙"_s, u".\\乙"_s, u"\\乙"_s, u"C:乙"_s}) {
            out = search(id++, {relative});
            QVERIFY2(out.isError && out.text.contains(u"full paths"), qPrintable(relative + u": "_s + out.text));
        }
    }

    // An answer longer than a line may be (pipe::kMaxLineBytes) would end
    // the connection at the other end with nothing said: an error that says
    // so instead, and the session goes on.
    void tooLarge()
    {
        Fixture f;
        ExternalTool big;
        big.name = u"big_answer"_s;
        big.title = u"Big"_s;
        big.description = u"An answer too large for a message"_s;
        big.inputSchema = {{u"type"_s, u"object"_s}};
        big.run = [](std::uint64_t, const QJsonObject&, std::function<void(ToolResult)> done) {
            done({{{u"blob"_s, QString(static_cast<qsizetype>(pipe::kMaxLineBytes), u'x')}}});
        };
        f.host->setExternalTools({big});
        Session s(f.server.get());
        QVERIFY(s.initialize(u"2025-11-25"_s, u"ws"_s));
        auto reply = s.exchange(callLine(2, u"big_answer"_s), 20000);
        QVERIFY(reply);
        QVERIFY(reply->size() < 4096);
        const Outcome out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"too large"), qPrintable(out.text));
        QCOMPARE(out.structured.value(u"error"_s).toString(), u"too_large"_s);
        QVERIFY2(f.log.last().contains(u"too large to send"), qPrintable(f.log.last()));
        reply = s.exchange(requestLine(3, u"ping"_s));
        QVERIFY(reply && parseObject(*reply).contains(u"result"_s));
    }

    // The work of a call that goes to the disk runs aside, never on the
    // thread that takes the calls (get_file_info and open_item too): three
    // jobs at a time, the rest waiting their turn, and one cancelled while
    // it waits never runs. A search's engine takes the next search as soon as
    // it answered the one before: a later call may answer first.
    void asideWork()
    {
        Fixture f;
        const QString file = f.write(u"aside.txt"_s, "a", 1'700'000'000);
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        const QJsonObject at {{u"path"_s, file}};

        // Held where it begins: not answered meanwhile.
        int id = 2;
        for (const QString& tool : {u"get_file_info"_s, u"open_item"_s}) {
            f.hold->hold(tool);
            const qsizetype before = s.received.size();
            f.server->receive(s.id, callLine(id++, tool, at));
            QCOMPARE(s.received.size(), before);
            QTRY_COMPARE(f.hold->held(), 1);
            QTest::qWait(50);
            QCOMPARE(s.received.size(), before);
            QCOMPARE(f.host->pendingCalls(), 1);
            f.hold->release();
            QTRY_COMPARE(s.received.size(), before + 1);
            const Outcome out = outcomeOf(s.received.last());
            QVERIFY2(!out.isError, qPrintable(out.text));
        }
        QCOMPARE(f.opened, QStringList {u"open "_s + file});

        // A search held where it is finished: the next one answers first.
        f.hold->hold(u"search_files"_s);
        qsizetype mark = s.received.size();
        f.server->receive(s.id, callLine(10, u"search_files"_s, {{u"query"_s, u"aside"_s}}));
        QTRY_COMPARE(f.hold->held(), 1);
        auto reply = s.exchange(callLine(11, u"search_files"_s, {{u"query"_s, u"aside"_s}}));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"id"_s).toInt(), 11);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"aside.txt"_s});
        f.hold->release();
        QTRY_COMPARE(s.received.size(), mark + 2);
        QCOMPARE(parseObject(s.received.last()).value(u"id"_s).toInt(), 10);
        QCOMPARE(names(outcomeOf(s.received.last()).structured), QStringList {u"aside.txt"_s});

        // Three at a time: with three held, a fourth waits its turn, and one
        // cancelled meanwhile never runs.
        const int infos = f.hold->startedOf(u"get_file_info"_s);
        f.hold->hold(u"list_folder"_s, 3);
        mark = s.received.size();
        for (int i = 20; i < 23; ++i)
            f.server->receive(s.id, callLine(i, u"list_folder"_s, {{u"path"_s, f.root}}));
        QTRY_COMPARE(f.hold->held(), 3);
        f.server->receive(s.id, callLine(23, u"get_file_info"_s, at));
        f.server->receive(s.id, callLine(24, u"get_file_info"_s, at));
        f.server->receive(s.id, jsonrpc::notification(u"notifications/cancelled"_s, {{u"requestId"_s, 24}}));
        QTest::qWait(300);
        QCOMPARE(f.hold->startedOf(u"get_file_info"_s), infos);
        QCOMPARE(s.received.size(), mark);
        QCOMPARE(f.host->pendingCalls(), 4);
        f.hold->release();
        QTRY_COMPARE(s.received.size(), mark + 4);
        QTest::qWait(200);
        QCOMPARE(s.received.size(), mark + 4);
        QCOMPARE(f.hold->startedOf(u"get_file_info"_s), infos + 1);
        QCOMPARE(f.host->pendingCalls(), 0);
        QVERIFY(!f.hold->ranOn(std::this_thread::get_id()));
    }

    // The host going tells all its work aside to stop at once, and only then
    // waits for it: three jobs that each take 600 ms to give up take about
    // that together, not one after the other; none answers.
    void stopAllThenJoin()
    {
        Fixture f;
        f.write(u"stop.txt"_s, "s", 1'700'000'000);
        {
            const std::lock_guard lock(f.hold->mutex);
            f.hold->slowStop = 600;
        }
        f.hold->hold(u"list_folder"_s, 3);
        int answered = 0;
        for (std::uint64_t id = 1; id <= 3; ++id)
            f.host->call(id, u"tst"_s, u"list_folder"_s, {{u"path"_s, f.root}}, [&](ToolResult) { ++answered; });
        QTRY_COMPARE(f.hold->held(), 3);
        f.server.reset(); // no sessions: the calls are the host's alone
        QElapsedTimer timer;
        timer.start();
        f.host.reset();
        const qint64 took = timer.elapsed();
        QVERIFY2(took >= 550 && took < 1400, qPrintable(QString::number(took)));
        QTest::qWait(50);
        QCOMPARE(answered, 0);
        QCOMPARE(f.hold->held(), 0);
    }

    // search_content's timeout counts from the call: one still waiting
    // behind another when it runs out answers then, having searched
    // nothing; one that waited part of it searches for the rest.
    void contentQueueTimeout()
    {
        Fixture f(true, true, ContentEngine::Locked);
        f.write(u"queue.txt"_s, "kumquat", 1'700'000'000);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        // The content engine's index locked: a search stays where it starts.
        std::optional<std::unique_lock<std::shared_mutex>> lock(f.contentService->index()->writeLock());
        const auto content = [](int timeout) { return QJsonObject {{u"query"_s, u"kumquat"_s}, {u"timeout_ms"_s, timeout}}; };
        const qsizetype start = s.received.size();
        f.server->receive(s.id, callLine(2, u"search_content"_s, content(30000)));
        QElapsedTimer timer;
        timer.start();
        f.server->receive(s.id, callLine(3, u"search_content"_s, content(800)));
        QTRY_COMPARE_WITH_TIMEOUT(s.received.size(), start + 1, 10000);
        QVERIFY2(timer.elapsed() >= 700, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(parseObject(s.received.last()).value(u"id"_s).toInt(), 3);
        Outcome out = outcomeOf(s.received.last());
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(!out.structured.value(u"complete"_s).toBool());
        QVERIFY(out.structured.value(u"queued"_s).toBool()); // nothing was searched: ws tells it from a search that ran out of time
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 0);
        QCOMPARE(out.structured.value(u"items"_s).toArray().size(), 0);
        QVERIFY2(out.structured.value(u"note"_s).toString().contains(u"did not start"), qPrintable(out.structured.value(u"note"_s).toString()));
        QCOMPARE(f.host->pendingCalls(), 1); // the first searches on

        // Waited 1.5 s of its 3: it searches for the other 1.5 s.
        timer.restart();
        f.server->receive(s.id, callLine(4, u"search_content"_s, content(3000)));
        QTest::qWait(1500);
        f.server->receive(s.id, jsonrpc::notification(u"notifications/cancelled"_s, {{u"requestId"_s, 2}}));
        QTRY_COMPARE_WITH_TIMEOUT(s.received.size(), start + 2, 10000);
        const qint64 took = timer.elapsed();
        QVERIFY2(took >= 2900 && took < 4000, qPrintable(QString::number(took)));
        QCOMPARE(parseObject(s.received.last()).value(u"id"_s).toInt(), 4);
        out = outcomeOf(s.received.last());
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(!out.structured.value(u"complete"_s).toBool());
        QVERIFY(!out.structured.contains(u"queued"_s)); // it waited, then searched
        QVERIFY2(out.structured.value(u"note"_s).toString().contains(u"timeout ran out"), qPrintable(out.structured.value(u"note"_s).toString()));
        lock.reset();
        QTRY_COMPARE(f.host->pendingCalls(), 0);
    }

    // truncated: only when there was more to give than was given (one more
    // seen past the limit, or the list cut to fit one answer), not when the
    // limit was just reached, nor for what is hidden from AI; list_folder's
    // total counts only what AI may see.
    void truncatedOnlyWhenMore()
    {
        Fixture f;
        for (int i = 0; i < 3; ++i)
            f.write(u"exact-%1.txt"_s.arg(i), "x", 1'700'000'000 + i);
        for (int i = 0; i < 20; ++i)
            f.subfolder(u"exact-dir-%1"_s.arg(i, 2, 10, u'0'), 1'700'000'000);
        f.write(u"private\\exact-p.txt"_s, "x", 1'700'000'100); // the newest, hidden from AI
        Policy policy = f.host->policy();
        policy.excludedPaths = {f.root + u"\\private"_s};
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        int id = 2;
        const auto ask = [&](const QString& tool, const QJsonObject& arguments) {
            const auto reply = s.exchange(callLine(id++, tool, arguments));
            return reply ? outcomeOf(*reply).structured : QJsonObject {};
        };
        const auto shown = [](const QJsonObject& out) {
            return std::pair {out.value(u"returned"_s).toInt(), out.value(u"truncated"_s).toBool()};
        };
        using Shown = std::pair<int, bool>;
        // Files: exactly the limit, all of them (the folders that match, and
        // the file hidden from AI, are not more of them); one fewer, more.
        const QJsonObject files {{u"query"_s, u"exact"_s}, {u"kind"_s, u"files"_s}};
        QJsonObject arguments = files;
        arguments.insert(u"limit"_s, 3);
        QCOMPARE(shown(ask(u"search_files"_s, arguments)), (Shown {3, false}));
        arguments.insert(u"limit"_s, 2);
        QCOMPARE(shown(ask(u"search_files"_s, arguments)), (Shown {2, true}));
        // Both kinds: 3 files and 20 folders.
        QCOMPARE(shown(ask(u"search_files"_s, {{u"query"_s, u"exact"_s}, {u"limit"_s, 23}})), (Shown {23, false}));
        QCOMPARE(shown(ask(u"search_files"_s, {{u"query"_s, u"exact"_s}, {u"limit"_s, 22}})), (Shown {22, true}));
        // recent_files: the newest is the hidden one; kept two per one asked
        // for, the rest are more.
        QCOMPARE(shown(ask(u"recent_files"_s, {{u"folder"_s, f.root}, {u"limit"_s, 3}})), (Shown {3, false}));
        QCOMPARE(shown(ask(u"recent_files"_s, {{u"folder"_s, f.root}, {u"limit"_s, 2}})), (Shown {2, true}));
        QCOMPARE(shown(ask(u"recent_files"_s, {{u"folder"_s, f.root}, {u"limit"_s, 1}})), (Shown {1, true}));
        // list_folder: 23 entries AI may see, beside the folder it may not.
        QJsonObject out = ask(u"list_folder"_s, {{u"path"_s, f.root}, {u"limit"_s, 23}});
        QCOMPARE(shown(out), (Shown {23, false}));
        QCOMPARE(out.value(u"total"_s).toInt(), 23);
        out = ask(u"list_folder"_s, {{u"path"_s, f.root}, {u"limit"_s, 22}});
        QCOMPARE(shown(out), (Shown {22, true}));
        QCOMPARE(out.value(u"total"_s).toInt(), 23);
    }

    // An answer is held to about 15 MB (5 for a model's, which goes out
    // twice, as text too): past that a list ends there, truncated, and a
    // text at next_offset. too_large is left for what cannot be cut.
    void answerBudget()
    {
        Fixture f;
        // 20 000 names of 257 characters, most three bytes: 17 MB of items for ws.
        const EntryId big = f.subfolder(u"big"_s, 1'700'000'000);
        const QString wide(240, QChar(0x20AC));
        {
            const auto index = f.service->index();
            const auto lock = index->writeLock();
            for (int i = 0; i < 20'000; ++i) {
                const EntryId entry = index->add(big, (u"budget-%1-"_s.arg(i, 5, 10, u'0') + wide + u".log"_s).toStdString(), 0);
                index->setModified(entry, 1'700'000'000 + i);
            }
        }
        // 3 million control characters, an escape of six bytes each: 18 MB of text.
        const QString control = f.write(u"control.txt"_s, QByteArray(3'000'000, '\x01'), 1'700'000'000);
        Session cli(f.server.get());
        QVERIFY(cli.initialize(u"2025-11-25"_s, u"ws"_s));

        const QString folder = f.root + u"\\big"_s;
        const std::pair<QString, QJsonObject> lists[] {
            {u"search_files"_s, {{u"query"_s, u"budget"_s}, {u"limit"_s, 50'000}, {u"size"_s, false}}},
            {u"list_folder"_s, {{u"path"_s, folder}, {u"limit"_s, 50'000}, {u"size"_s, false}}},
            {u"recent_files"_s, {{u"folder"_s, folder}, {u"limit"_s, 50'000}, {u"size"_s, false}}},
        };
        int id = 2;
        for (const auto& [tool, arguments] : lists) {
            const auto reply = cli.exchange(callLine(id++, tool, arguments), 60000);
            QVERIFY2(reply, qPrintable(tool));
            QVERIFY2(static_cast<std::size_t>(reply->size()) <= pipe::kMaxLineBytes, qPrintable(tool + u": "_s + QString::number(reply->size())));
            const Outcome out = outcomeOf(*reply);
            QVERIFY2(!out.isError, qPrintable(tool + u": "_s + out.text));
            const int returned = out.structured.value(u"returned"_s).toInt();
            QVERIFY2(returned >= 10'000 && returned < 20'000, qPrintable(tool + u": "_s + QString::number(returned)));
            QCOMPARE(out.structured.value(u"items"_s).toArray().size(), returned);
            QVERIFY2(out.structured.value(u"truncated"_s).toBool(), qPrintable(tool));
        }

        // A text: as much as fits, then the rest from next_offset.
        auto reply = cli.exchange(callLine(10, u"read_file_text"_s, {{u"path"_s, control}, {u"max_chars"_s, 4'000'000}}), 60000);
        QVERIFY(reply);
        QVERIFY2(static_cast<std::size_t>(reply->size()) <= pipe::kMaxLineBytes, qPrintable(QString::number(reply->size())));
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        const qsizetype first = out.structured.value(u"text"_s).toString().size();
        QVERIFY2(first >= 2'000'000 && first < 3'000'000, qPrintable(QString::number(first)));
        QCOMPARE(out.structured.value(u"chars"_s).toInt(), 3'000'000);
        QCOMPARE(out.structured.value(u"next_offset"_s).toInt(), first);
        reply = cli.exchange(callLine(11, u"read_file_text"_s, {{u"path"_s, control}, {u"max_chars"_s, 4'000'000}, {u"offset"_s, first}}), 60000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(out.structured.value(u"text"_s).toString().size(), 3'000'000 - first);
        QVERIFY(!out.structured.contains(u"next_offset"_s));

        // A model's 100 000 characters fit its own budget whole.
        Session ai(f.server.get());
        QVERIFY(ai.initialize());
        reply = ai.exchange(callLine(2, u"read_file_text"_s, {{u"path"_s, control}, {u"max_chars"_s, 100'000}}), 60000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(out.structured.value(u"text"_s).toString().size(), 100'000);
        QCOMPARE(out.structured.value(u"next_offset"_s).toInt(), 100'000);
    }

    // What a folder named in a call tells of a path the caller cannot see:
    // nothing. A file it can see is called a file; one in a folder it cannot
    // list is not in the index for it, as one on no disk at all
    // (search_files, search_content, list_folder, recent_files).
    void privacyOrder()
    {
        Fixture f;
        const QString seen = f.write(u"open\\seen.txt"_s, "kumquat", 1'700'000'000);
        const QString unseen = f.write(u"locked\\unseen.txt"_s, "kumquat", 1'700'000'000);
        QVERIFY(f.restrict(u"locked"_s));
        Session s(f.server.get());
        QVERIFY(s.initialize(u"2025-11-25"_s, u"ws"_s));
        int id = 2;
        const auto ask = [&](const QString& tool, const QString& path) {
            QJsonObject arguments {{u"query"_s, u"kumquat"_s}, {u"folders"_s, QJsonArray {path}}};
            if (tool == u"list_folder")
                arguments = {{u"path"_s, path}};
            else if (tool == u"recent_files")
                arguments = {{u"folder"_s, path}};
            const auto reply = s.exchange(callLine(id++, tool, arguments), 20000);
            return reply ? outcomeOf(*reply) : Outcome {true, u"(no answer)"_s};
        };
        for (const QString& tool : {u"search_files"_s, u"search_content"_s, u"list_folder"_s, u"recent_files"_s}) {
            Outcome out = ask(tool, seen);
            QVERIFY2(out.isError && out.text.contains(u"is a file"), qPrintable(tool + u": "_s + out.text));
            QVERIFY(out.structured.value(u"error"_s).toString().isEmpty());
            for (const QString& path : {unseen, f.root + u"\\locked\\nothing.txt"_s, f.root + u"\\nothing.txt"_s}) {
                out = ask(tool, path);
                QVERIFY2(out.isError && out.text.contains(u"not in the index"), qPrintable(tool + u" " + path + u": "_s + out.text));
                QCOMPARE(out.structured.value(u"error"_s).toString(), u"not_found"_s);
            }
        }
    }

    // A network share that cannot be reached now: nothing on it is looked
    // at (an access check, a size, a path opened there would wait for the
    // network), its entries are left out, and a folder or a path on it is
    // not there.
    void offlineShares()
    {
        Fixture f;
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        f.host->setPolicy(policy);
        const QString share = u"\\\\ws-test-gone\\share"_s;
        f.addShare(share, {u"remote-kumquat.txt"_s, u"docs\\remote-kumquat-2.txt"_s}, 1'700'000'500);
        f.offline->set({share});
        Session s(f.server.get());
        QVERIFY(s.initialize());
        const qsizetype logged = f.log.size();
        QElapsedTimer timer;
        timer.start();

        auto reply = s.exchange(callLine(2, u"search_files"_s, {{u"query"_s, u"remote-kumquat"_s}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 0);
        reply = s.exchange(callLine(3, u"recent_files"_s));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 0);

        // Named: not there.
        const QString docs = share + u"\\docs"_s;
        const QString remote = share + u"\\remote-kumquat.txt"_s;
        const std::pair<QString, QJsonObject> named[] {
            {u"search_files"_s, {{u"query"_s, u"kumquat"_s}, {u"folders"_s, QJsonArray {docs}}}},
            {u"search_content"_s, {{u"query"_s, u"kumquat"_s}, {u"folders"_s, QJsonArray {share}}}},
            {u"list_folder"_s, {{u"path"_s, docs}}},
            {u"recent_files"_s, {{u"folder"_s, share}}},
            {u"read_file_text"_s, {{u"path"_s, remote}}},
            {u"open_item"_s, {{u"path"_s, remote}}},
        };
        int id = 4;
        for (const auto& [tool, arguments] : named) {
            reply = s.exchange(callLine(id++, tool, arguments));
            QVERIFY2(reply, qPrintable(tool));
            out = outcomeOf(*reply);
            QVERIFY2(out.isError, qPrintable(tool));
            QVERIFY2(out.text.contains(u"cannot be reached now") || out.text.contains(u"no such file"), qPrintable(tool + u": "_s + out.text));
        }
        reply = s.exchange(callLine(id++, u"get_file_info"_s, {{u"path"_s, remote}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(!out.structured.value(u"exists"_s).toBool());
        QVERIFY2(out.structured.value(u"error"_s).toString().contains(u"cannot be reached now"), qPrintable(out.structured.value(u"error"_s).toString()));
        QVERIFY(f.opened.isEmpty());
        // None of it waited for the network, nor checked anything there.
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
        for (qsizetype i = logged; i < f.log.size(); ++i)
            QVERIFY2(!f.log.at(i).contains(u"access checks"), qPrintable(f.log.at(i)));
    }

    // A path is taken for where it leads: not a way into a folder hidden
    // from AI by a junction, a short (8.3) name, a device path (\\?\), or
    // this computer's own share by a network path. Without such folders a
    // link is followed: get_file_info says the path given, open_item opens
    // where it leads.
    void finalPaths()
    {
        Fixture f;
        wchar_t longName[4096] {};
        const DWORD longLength = ::GetLongPathNameW(f.root.toStdWString().c_str(), longName, 4096);
        if (QString::fromWCharArray(longName, longLength).compare(f.root, Qt::CaseInsensitive) != 0)
            QSKIP("The temporary folder is named by short names here: what paths lead to is compared in long names");
        const QString secret = f.write(u"private-folder\\p.txt"_s, "secret kumquat", 1'700'000'000);
        const QString open = f.write(u"open.txt"_s, "open", 1'700'000'000);
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        policy.excludedPaths = {f.root + u"\\private-folder"_s};
        f.host->setPolicy(policy);
        const QString link = f.root + u"\\link"_s;
        QCOMPARE(QProcess::execute(u"cmd.exe"_s, {u"/c"_s, u"mklink"_s, u"/J"_s, link, f.root + u"\\private-folder"_s}), 0);
        const auto unlink = qScopeGuard([&] { ::RemoveDirectoryW(link.toStdWString().c_str()); });
        QVERIFY(QFileInfo::exists(link + u"\\p.txt"_s));
        Session s(f.server.get());
        QVERIFY(s.initialize(u"2025-11-25"_s, u"ws"_s));
        int id = 2;
        const auto call = [&](const QString& tool, const QString& path) {
            const auto reply = s.exchange(callLine(id++, tool, {{u"path"_s, path}}), 20000);
            return reply ? outcomeOf(*reply) : Outcome {true, u"(no answer)"_s};
        };
        const auto shown = [](const Outcome& out) {
            return out.text + u" "_s + QString::fromUtf8(QJsonDocument(out.structured).toJson(QJsonDocument::Compact));
        };
        // Empty: hidden from all three tools, as it should be.
        const auto hidden = [&](const QString& path) -> QString {
            Outcome out = call(u"read_file_text"_s, path);
            if (!out.isError || out.structured.value(u"error"_s).toString() != u"not_found")
                return u"read_file_text: "_s + shown(out);
            out = call(u"get_file_info"_s, path);
            if (out.isError ? out.structured.value(u"error"_s).toString() != u"not_found" : out.structured.value(u"exists"_s).toBool())
                return u"get_file_info: "_s + shown(out);
            out = call(u"open_item"_s, path);
            if (!out.isError || out.structured.value(u"error"_s).toString() != u"not_found")
                return u"open_item: "_s + shown(out);
            return {};
        };
        for (const QString& path : {secret, link + u"\\p.txt"_s, link, link + u"\\..\\link\\p.txt"_s}) {
            const QString problem = hidden(path);
            QVERIFY2(problem.isEmpty(), qPrintable(path + u": "_s + problem));
        }
        // A short name, where the volume makes them.
        wchar_t shortName[4096] {};
        const DWORD shortLength = ::GetShortPathNameW(secret.toStdWString().c_str(), shortName, 4096);
        const QString shortPath = QString::fromWCharArray(shortName, shortLength);
        if (shortLength > 0 && shortLength < 4096 && !shortPath.contains(u"private-folder"_s, Qt::CaseInsensitive)) {
            const QString problem = hidden(shortPath);
            QVERIFY2(problem.isEmpty(), qPrintable(shortPath + u": "_s + problem));
        } else {
            qInfo("No 8.3 names on this volume: short names are not tried");
        }
        QVERIFY(f.opened.isEmpty());

        // Device paths: never, whatever they lead to.
        for (const QString& device : {u"\\\\?\\"_s + secret, u"\\\\.\\"_s + open, u"//?/"_s + QDir::fromNativeSeparators(open), u"\\??\\"_s + open}) {
            for (const QString& tool : {u"get_file_info"_s, u"read_file_text"_s, u"open_item"_s}) {
                const Outcome out = call(tool, device);
                QVERIFY2(out.isError && out.text.contains(u"Device paths"), qPrintable(tool + u" " + device + u": "_s + shown(out)));
            }
        }
        // This computer by a network path: not while folders are hidden from AI.
        wchar_t computer[256] {};
        DWORD size = 256;
        QVERIFY(::GetComputerNameExW(ComputerNameNetBIOS, computer, &size));
        const QString tail = f.root.left(1) + u"$"_s + f.root.mid(2) + u"\\open.txt"_s;
        for (const QString& host : {u"localhost"_s, u"127.0.0.1"_s, u"LocalHost."_s, QString::fromWCharArray(computer, size)}) {
            const QString unc = u"\\\\"_s + host + u"\\"_s + tail;
            for (const QString& tool : {u"get_file_info"_s, u"read_file_text"_s}) {
                const Outcome out = call(tool, unc);
                QVERIFY2(out.isError && out.text.contains(u"leads back"), qPrintable(tool + u" " + unc + u": "_s + shown(out)));
                QCOMPARE(out.structured.value(u"error"_s).toString(), u"not_found"_s);
            }
        }
        QVERIFY(f.opened.isEmpty());

        // No folder hidden: a link is followed.
        policy.excludedPaths.clear();
        f.host->setPolicy(policy);
        Outcome out = call(u"get_file_info"_s, link + u"\\p.txt"_s);
        QVERIFY2(out.structured.value(u"exists"_s).toBool(), qPrintable(shown(out)));
        QCOMPARE(out.structured.value(u"path"_s).toString(), link + u"\\p.txt"_s);
        QVERIFY(out.structured.value(u"indexed"_s).toBool()); // where it leads is in the index
        out = call(u"read_file_text"_s, link + u"\\p.txt"_s);
        QCOMPARE(out.structured.value(u"text"_s).toString(), u"secret kumquat"_s);
        out = call(u"open_item"_s, link + u"\\p.txt"_s);
        QVERIFY2(!out.isError, qPrintable(shown(out)));
        QCOMPARE(f.opened.size(), 1);
        QCOMPARE(f.opened.first().compare(u"open "_s + secret, Qt::CaseInsensitive), 0);
    }

    // The answer's JSON is made where the work was done (CallOptions::encode,
    // as McpServer asks): the same bytes on the wire as made on this thread,
    // and the result keeps only what its log line reads.
    void encodedAside()
    {
        Fixture f;
        f.write(u"enc.txt"_s, "e", 1'700'000'000);
        std::optional<ToolResult> got;
        const auto run = [&](std::uint64_t id, const QJsonObject& arguments, CallOptions options) {
            got.reset();
            f.host->call(id, u"tst"_s, u"list_folder"_s, arguments, [&](ToolResult r) { got = std::move(r); }, options);
            return QTest::qWaitFor([&] { return got.has_value(); }, 10000);
        };
        const QJsonObject list {{u"path"_s, f.root}};
        QVERIFY(run(1, list, {nullptr, false, true}));
        QVERIFY(!got->encoded.isEmpty());
        QCOMPARE(got->structured, (QJsonObject {{u"returned"_s, 1}}));
        const QByteArray bytes = got->encoded;
        const QJsonObject encoded = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(encoded.value(u"structuredContent"_s).toObject().value(u"items"_s).toArray().size(), 1);
        QCOMPARE(encoded.value(u"content"_s).toArray().size(), 1); // a model's: the text too
        // Not asked for: the whole result, made into JSON here (McpServer), the same.
        QVERIFY(run(2, list, {nullptr, false, false}));
        QVERIFY(got->encoded.isEmpty());
        QCOMPARE(QJsonDocument(callToolResult(*got, false)).toJson(QJsonDocument::Compact), bytes);
        for (const QJsonValue& id : {QJsonValue(7), QJsonValue(u"seven"_s), QJsonValue(1e15), QJsonValue(-2)})
            QCOMPARE(jsonrpc::responseWithResult(id, bytes), jsonrpc::response(id, encoded));
        // ws's: an error with its kind.
        QVERIFY(run(3, {{u"path"_s, f.root + u"\\nowhere"_s}}, {nullptr, true, true}));
        QVERIFY(got->isError);
        const QJsonObject error = QJsonDocument::fromJson(got->encoded).object();
        QVERIFY(error.value(u"isError"_s).toBool());
        QCOMPARE(error.value(u"structuredContent"_s).toObject().value(u"error"_s).toString(), u"not_found"_s);
    }

    // The work aside goes at a hard disk's pace while the user is there, as
    // the indexer's quiet mode does: background mode for a path on one, the
    // network and the user away at full speed; ended with it.
    void quietIo()
    {
        const QString drive = QDir::toNativeSeparators(QDir::rootPath()).left(2).toUpper();
        qputenv("WINSHUN_SPINNING", drive.toUtf8()); // this drive counts as a hard disk
        const auto unset = qScopeGuard([] { qunsetenv("WINSHUN_SPINNING"); });
        {
            QuietIo quiet(false);
            QVERIFY(!quiet.background());
            quiet.at(drive + u"\\Windows"_s);
            QVERIFY(quiet.background());
            quiet.at(drive + u"\\Windows\\System32"_s);
            QVERIFY(quiet.background());
            quiet.at(u"\\\\server\\share\\file"_s);
            QVERIFY(!quiet.background());
            quiet.at(drive + u"\\x"_s);
            QVERIFY(quiet.background());
        }
        {
            // Begun anew: the thread was let go of (begun twice, it fails).
            QuietIo again(false);
            again.at(drive + u"\\x"_s);
            QVERIFY(again.background());
        }
        {
            QuietIo away(true);
            away.at(drive + u"\\x"_s);
            QVERIFY(!away.background());
        }
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
