// WinShunMcp.exe: the MCP server AI clients (Claude Desktop, Claude Code,
// Cursor, VS Code …) start over stdio. It carries the JSON-RPC messages to
// the running Win顺 over its named pipe (Pipe.h), where the protocol and the
// tools live (McpServer), and brings the answers back. Of its own it does
// three things:
//
//   - connects when first needed and again whenever the pipe is gone: Win顺
//     may start after the AI client, or be restarted, without the client
//     noticing anything but a moment's wait;
//   - while Win顺 is not running, still answers initialize (so the client
//     does not give the server up), lists the tools it last saw, and answers
//     tool calls with an error the model can tell the user about; with
//     --start-winshun, the first tool call also starts Win顺 through its
//     autostart task (no UAC prompt) when it is not running at all;
//   - logs to stderr, which the clients keep (Claude Desktop: mcp-server-*.log).
//
//   WinShunMcp.exe [--pipe <name>] [--start-winshun]
//       --pipe: the pipe's name, for tests (default: Pipe.h's);
//       --start-winshun: a tool call may start Win顺 when it is not running

#include "JsonRpc.h"
#include "Launch.h"
#include "Pipe.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

constexpr const char* kProtocolVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
constexpr DWORD kConnectTimeoutMs = 2000; // for a free instance of a busy pipe
constexpr DWORD kHandshakeTimeoutMs = 5000;
constexpr int kStartWaitMs = 12000; // for Win顺 to come up after its task was run
const QLatin1StringView kRelayIdPrefix("winshun-relay-");

void logLine(const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    std::fputs("[WinShunMcp] ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    va_end(args);
}

// %LOCALAPPDATA%\WinShun\mcp-tools.json: the tools Win顺 last listed, for
// answering tools/list while it is not running. In a portable copy, in the
// "data" folder beside the program (Settings::portableFolder).
QString cachePath()
{
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length > 0 && length < std::size(exe)) {
        const QString portable = QFileInfo(QString::fromWCharArray(exe, static_cast<qsizetype>(length))).absolutePath()
            + u"/data"_s;
        if (QFileInfo(portable).isDir())
            return QDir::toNativeSeparators(portable + u"/mcp-tools.json"_s);
    }
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (local.isEmpty())
        return {};
    return QDir::toNativeSeparators(local + u"\\WinShun\\mcp-tools.json"_s);
}

using launch::startWinShun;
using launch::winShunRunning;

QString offlineText()
{
    if (winShunRunning()) {
        return u"Win顺 (WinShun) is running, but its AI connection is off (or this version of Win顺 has none), so "
               "this tool cannot answer. Ask the user to turn on 允许 AI 连接 (Allow AI assistants to connect) in "
               "Win顺's settings › AI 助手, or to update Win顺; then call the tool again. "
               "／Win顺 在运行，但没有开放 AI 连接（或这个版本还没有这个功能）。请在 Win顺 的设置 → AI 助手 里打开"
               "“允许 AI 连接”，或者更新 Win顺，然后再试一次。"_s;
    }
    return u"Win顺 (WinShun) is not running on this PC, so this tool cannot answer right now. "
           "Ask the user to start Win顺 (Start menu › Win顺, or the tray icon), then call the tool again: "
           "there is no need to restart the AI client. "
           "／Win顺 没有运行，这个工具暂时用不了。请启动 Win顺（开始菜单里的 Win顺），然后再试一次，不用重启 AI 客户端。"_s;
}

QJsonObject toolError(const QString& text)
{
    return {{u"content"_s, QJsonArray {QJsonObject {{u"type"_s, u"text"_s}, {u"text"_s, text}}}}, {u"isError"_s, true}};
}

QJsonObject toolValue(const QJsonObject& structured)
{
    return {{u"content"_s, QJsonArray {QJsonObject {{u"type"_s, u"text"_s},
                               {u"text"_s, QString::fromUtf8(QJsonDocument(structured).toJson(QJsonDocument::Compact))}}}},
        {u"structuredContent"_s, structured}, {u"isError"_s, false}};
}

// The one tool answered here while Win顺 is away: index_status, saying so.
QJsonObject statusTool()
{
    return {{u"name"_s, u"index_status"_s}, {u"title"_s, u"Win顺 status"_s},
        {u"description"_s, u"Whether Win顺 (WinShun) is running and its index is ready."_s},
        {u"inputSchema"_s, QJsonObject {{u"type"_s, u"object"_s}, {u"additionalProperties"_s, false}}},
        {u"annotations"_s,
            QJsonObject {{u"readOnlyHint"_s, true}, {u"destructiveHint"_s, false}, {u"idempotentHint"_s, true}, {u"openWorldHint"_s, false}}}};
}

class Relay {
public:
    Relay(std::wstring pipeName, bool mayStart)
        : m_pipeName(std::move(pipeName))
        , m_mayStart(mayStart)
    {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        m_cache = loadCache();
    }

    ~Relay()
    {
        disconnect();
    }

    int run()
    {
        logLine("pipe %ls", m_pipeName.c_str());
        QByteArray line;
        while (readStdin(line)) {
            if (line.trimmed().isEmpty())
                continue;
            handle(line);
        }
        logLine("stdin closed: exiting");
        return 0;
    }

private:
    // ---- stdio -------------------------------------------------------------

    bool readStdin(QByteArray& line)
    {
        for (;;) {
            if (const qsizetype at = m_in.indexOf('\n'); at >= 0) {
                line = m_in.left(at);
                if (line.endsWith('\r'))
                    line.chop(1);
                m_in.remove(0, at + 1);
                return true;
            }
            char chunk[64 * 1024];
            DWORD got = 0;
            if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), chunk, sizeof chunk, &got, nullptr) || got == 0)
                return false;
            m_in.append(chunk, static_cast<qsizetype>(got));
        }
    }

    void writeOut(const QByteArray& line)
    {
        const std::lock_guard lock(m_outMutex);
        std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
        std::fflush(stdout);
        logLine("-> %lld bytes", static_cast<long long>(line.size()));
    }

    // ---- the pipe ------------------------------------------------------------

    bool connected()
    {
        const std::lock_guard lock(m_mutex);
        return m_online;
    }

    // Connects if not connected, and brings the session up (the client's
    // initialize, sent again) when the client has initialized already.
    bool ensureConnected()
    {
        if (connected())
            return true;
        DWORD error = 0;
        if (!m_client.connect(m_pipeName, kConnectTimeoutMs, &error)) {
            if (error != ERROR_FILE_NOT_FOUND)
                logLine("cannot open the pipe (%lu)", error);
            return false;
        }
        logLine("connected to Win顺");
        {
            const std::lock_guard lock(m_mutex);
            m_online = true;
            m_handshakeDone = false;
            m_pending.clear();
        }
        if (m_reader.joinable())
            m_reader.join(); // the previous connection's reader has ended
        m_reader = std::thread([this] { readPipe(); });
        if (m_clientInitialized && !handshake()) {
            logLine("Win顺 did not answer initialize");
            disconnect();
            return false;
        }
        return true;
    }

    // The client initialized before this connection: initialize again for it.
    bool handshake()
    {
        {
            const std::lock_guard lock(m_mutex);
            m_handshakeAnswered = false;
        }
        const QString id = kRelayIdPrefix + QString::number(++m_relayIds);
        if (!m_client.write(jsonrpc::request(id, u"initialize"_s, m_clientInit)))
            return false;
        std::unique_lock lock(m_mutex);
        if (!m_handshakeCv.wait_for(lock, std::chrono::milliseconds(kHandshakeTimeoutMs), [&] { return m_handshakeAnswered || !m_online; }))
            return false;
        if (!m_online)
            return false;
        m_handshakeDone = true;
        lock.unlock();
        m_client.write(jsonrpc::notification(u"notifications/initialized"_s));
        return true;
    }

    void disconnect()
    {
        m_client.close();
        {
            const std::lock_guard lock(m_mutex);
            m_online = false;
        }
        m_handshakeCv.notify_all();
        if (m_reader.joinable() && std::this_thread::get_id() != m_reader.get_id())
            m_reader.join();
    }

    // Everything Win顺 sends: answers to the client's requests go to stdout;
    // the relay's own are taken care of. On a broken pipe, what was still
    // pending is answered here.
    void readPipe()
    {
        QByteArray line;
        while (m_client.readLine(line)) {
            const std::optional<jsonrpc::Message> message = jsonrpc::parse(line);
            if (!message) {
                logLine("unreadable line from Win顺 (%lld bytes)", static_cast<long long>(line.size()));
                continue;
            }
            if (message->isResponse()) {
                const QString key = jsonrpc::idKey(message->id);
                if (message->id.isString() && message->id.toString().startsWith(kRelayIdPrefix)) {
                    const std::lock_guard lock(m_mutex);
                    m_handshakeAnswered = true;
                    m_handshakeCv.notify_all();
                    continue;
                }
                QString method;
                {
                    const std::lock_guard lock(m_mutex);
                    if (const auto it = m_pending.find(key); it != m_pending.end()) {
                        method = it->second;
                        m_pending.erase(it);
                    }
                }
                if (method == u"tools/list" && !message->failed)
                    saveCache(message->result.toObject().value(u"tools"_s).toArray());
                else if (method == u"initialize" && !message->failed)
                    m_handshakeDone = true;
            }
            writeOut(line + '\n');
        }
        // Gone: whatever was pending is answered from here.
        std::map<QString, QString> pending;
        {
            const std::lock_guard lock(m_mutex);
            m_online = false;
            pending.swap(m_pending);
        }
        m_handshakeCv.notify_all();
        logLine("Win顺 went away (%zu requests pending)", pending.size());
        for (const auto& [key, method] : pending) {
            const QJsonValue id = QJsonDocument::fromJson((u"["_s + key + u"]"_s).toUtf8()).array().at(0);
            if (method == u"initialize")
                writeOut(jsonrpc::response(id, localInitialize()));
            else if (method == u"tools/call")
                writeOut(jsonrpc::response(id, toolError(u"Win顺 (WinShun) stopped while answering. "_s + offlineText())));
            else
                writeOut(jsonrpc::errorResponse(id, jsonrpc::kUnavailable, u"Win顺 (WinShun) stopped while answering"_s));
        }
    }

    // Sends a request on: its answer comes back through readPipe(). False
    // when the pipe is gone (the caller answers locally).
    bool forward(const QByteArray& line, const QJsonValue& id, const QString& method)
    {
        if (!id.isUndefined()) {
            const std::lock_guard lock(m_mutex);
            m_pending[jsonrpc::idKey(id)] = method;
        }
        if (m_client.write(line + '\n'))
            return true;
        if (!id.isUndefined()) {
            const std::lock_guard lock(m_mutex);
            m_pending.erase(jsonrpc::idKey(id));
        }
        return false;
    }

    // ---- answering here --------------------------------------------------------

    QJsonObject localInitialize() const
    {
        QString version = QString::fromLatin1(kProtocolVersions[0]);
        const QString requested = m_clientInit.value(u"protocolVersion"_s).toString();
        for (const char* known : kProtocolVersions) {
            if (requested == QLatin1StringView(known))
                version = requested;
        }
        return {{u"protocolVersion"_s, version}, {u"capabilities"_s,
                QJsonObject {{u"tools"_s, QJsonObject {{u"listChanged"_s, true}}}, {u"prompts"_s, QJsonObject {{u"listChanged"_s, false}}}}},
            {u"serverInfo"_s,
                QJsonObject {{u"name"_s, u"winshun"_s}, {u"title"_s, u"Win顺 · WinShun"_s}, {u"version"_s, QStringLiteral(WINSHUN_VERSION)},
                    {u"description"_s, u"Search files, folders and text on this PC with Win顺"_s}}},
            {u"instructions"_s,
                u"Win顺 (WinShun) searches the files, folders and text on this PC. It is not running at the moment: "
                "its tools answer with an error until the user starts it (Start menu › Win顺); then they work again "
                "without restarting this client. index_status tells whether it is running."_s}};
    }

    QJsonArray loadCache() const
    {
        QFile file(cachePath());
        if (cachePath().isEmpty() || !file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).array();
    }

    void saveCache(const QJsonArray& tools)
    {
        {
            const std::lock_guard lock(m_mutex);
            m_cache = tools;
        }
        const QString path = cachePath();
        if (path.isEmpty() || tools.isEmpty())
            return;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(QJsonDocument(tools).toJson(QJsonDocument::Compact));
    }

    QJsonArray offlineTools()
    {
        const std::lock_guard lock(m_mutex);
        QJsonArray tools = m_cache;
        bool hasStatus = false;
        for (const QJsonValue& tool : tools)
            hasStatus = hasStatus || tool.toObject().value(u"name"_s).toString() == u"index_status";
        if (!hasStatus)
            tools.push_back(statusTool());
        return tools;
    }

    // ---- one line from the client ------------------------------------------------

    void handle(const QByteArray& line)
    {
        QString problem;
        const std::optional<jsonrpc::Message> message = jsonrpc::parse(line, &problem);
        if (!message) {
            writeOut(jsonrpc::errorResponse(QJsonValue::Null, jsonrpc::kParseError, u"Parse error: "_s + problem));
            return;
        }
        if (message->isNotification()) {
            logLine("<- %s", qUtf8Printable(message->method));
            if (connected() && !forward(line, QJsonValue::Undefined, message->method))
                logLine("could not forward %s", qUtf8Printable(message->method));
            return;
        }
        if (message->isResponse()) {
            if (connected())
                forward(line, QJsonValue::Undefined, {});
            return;
        }

        const QJsonValue id = message->id;
        const QString& method = message->method;
        logLine("<- %s %s", qUtf8Printable(method), qUtf8Printable(jsonrpc::idKey(id)));
        if (method == u"initialize") {
            m_clientInit = message->params;
            const QJsonObject client = message->params.value(u"clientInfo"_s).toObject();
            logLine("client: %s %s", qUtf8Printable(client.value(u"name"_s).toString()), qUtf8Printable(client.value(u"version"_s).toString()));
            // The client's own initialize goes to Win顺 as it is (so Win顺's
            // answer, instructions and all, is the client's): the connection
            // made here must not initialize on its behalf first.
            m_clientInitialized = false;
            const bool online = ensureConnected();
            m_clientInitialized = true;
            if (online && !m_handshakeDone && forward(line, id, method))
                return; // Win顺 answers; readPipe() notes the session is up
            writeOut(jsonrpc::response(id, localInitialize()));
            return;
        }
        if (method == u"ping") {
            writeOut(jsonrpc::response(id, QJsonObject()));
            return;
        }
        if (method == u"tools/call") {
            const QString tool = message->params.value(u"name"_s).toString();
            if (!ensureConnected() && m_mayStart && !m_triedStart && !winShunRunning()) {
                // The first call with Win顺 not running at all: start it, as
                // the taskbar button would, and wait a little for it. Never
                // for one that runs without the pipe: its task would only
                // hand the running copy a "show", and the launcher would pop up.
                m_triedStart = true;
                if (startWinShun()) {
                    logLine("started Win顺 (autostart task or program); waiting for it");
                    const ULONGLONG until = ::GetTickCount64() + kStartWaitMs;
                    while (!ensureConnected() && ::GetTickCount64() < until)
                        ::Sleep(300);
                } else {
                    logLine("Win顺 is not running and could not be started");
                }
            }
            if (connected() && forward(line, id, method))
                return;
            if (tool == u"index_status") {
                // `running` is the truth (the model reads the field, not
                // only the hint): a Win顺 that runs without the pipe is running.
                writeOut(jsonrpc::response(id,
                    toolValue({{u"running"_s, winShunRunning()}, {u"connected"_s, false}, {u"hint"_s, offlineText()}})));
            } else {
                writeOut(jsonrpc::response(id, toolError(offlineText())));
            }
            return;
        }
        if (ensureConnected() && forward(line, id, method))
            return;
        // Offline: what can be answered without Win顺.
        if (method == u"tools/list")
            writeOut(jsonrpc::response(id, QJsonObject {{u"tools"_s, offlineTools()}}));
        else if (method == u"resources/list")
            writeOut(jsonrpc::response(id, QJsonObject {{u"resources"_s, QJsonArray()}}));
        else if (method == u"resources/templates/list")
            writeOut(jsonrpc::response(id, QJsonObject {{u"resourceTemplates"_s, QJsonArray()}}));
        else if (method == u"prompts/list")
            writeOut(jsonrpc::response(id, QJsonObject {{u"prompts"_s, QJsonArray()}}));
        else if (method == u"logging/setLevel")
            writeOut(jsonrpc::response(id, QJsonObject()));
        else
            writeOut(jsonrpc::errorResponse(id, jsonrpc::kMethodNotFound, u"Method not found: "_s + method));
    }

    std::wstring m_pipeName;
    pipe::Client m_client;
    std::thread m_reader;
    QByteArray m_in; // stdin, read but not yet a whole line
    std::mutex m_outMutex; // stdout
    std::mutex m_mutex; // the state below
    std::condition_variable m_handshakeCv;
    bool m_online = false;
    std::atomic<bool> m_handshakeDone {false}; // this connection's session is initialized
    bool m_handshakeAnswered = false;
    std::map<QString, QString> m_pending; // forwarded request ids -> method
    QJsonArray m_cache; // the tools last listed
    QJsonObject m_clientInit; // the client's initialize params
    bool m_clientInitialized = false;
    bool m_mayStart = false; // --start-winshun
    bool m_triedStart = false;
    int m_relayIds = 0;
};

} // namespace

int main()
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::wstring pipeName = pipe::defaultName();
    bool mayStart = false;
    int count = 0;
    if (LPWSTR* args = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) {
            const std::wstring_view arg = args[i];
            if (arg == L"--pipe" && i + 1 < count) {
                pipeName = args[++i];
            } else if (arg == L"--start-winshun") {
                mayStart = true;
            } else if (arg == L"--version") {
                std::printf("WinShunMcp %s\n", WINSHUN_VERSION);
                ::LocalFree(args);
                return 0;
            } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
                std::printf("WinShunMcp.exe [--pipe <name>] [--start-winshun]\n"
                            "The MCP server (stdio) of Win顺 · WinShun; add it to your AI client.\n"
                            "  --start-winshun   a tool call may start Win顺 (through its autostart task) when it is not running\n");
                ::LocalFree(args);
                return 0;
            }
        }
        ::LocalFree(args);
    }
    Relay relay(std::move(pipeName), mayStart);
    return relay.run();
}
