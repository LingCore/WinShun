#include "McpServer.h"

#include "JsonRpc.h"
#include "Pipe.h"
#include "PipeServer.h"
#include "Win32Util.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::mcp {

struct McpServer::Session {
    SessionId id = 0;
    std::function<void(const QByteArray& line)> sink;
    bool initialized = false; // initialize was answered
    bool ready = false; // notifications/initialized came
    QString protocolVersion;
    QString client;
    QString clientVersion;
    bool cli = false; // ws, the command line (kCliClient)
    bool cliCounted = false; // this run of ws is in the tally
    bool cliNamed = false; // it called a tool other than index_status: that one names it
    QDateTime since;
    int calls = 0;
    QString lastTool;
    QDateTime lastCall;
    std::map<QString, std::uint64_t> inflight; // request id (idKey) -> the host's call id
    // The client's own token (Visibility): what it may see. Invalid for
    // sessions without one: the host's default token then.
    win32::UniqueHandle token;

    // In sessions(): an AI client that said who it is. Not before (a
    // connection is nobody yet), never ws (many a minute: cliTally()).
    bool listed() const noexcept { return initialized && !cli; }
};

McpServer::McpServer(ToolHost* host, QString version, QObject* parent)
    : QObject(parent)
    , m_host(host)
    , m_version(std::move(version))
{
    connect(m_host, &ToolHost::toolsChanged, this, &McpServer::toolsChanged);
}

McpServer::~McpServer()
{
    stop();
    for (auto& [id, session] : m_sessions) {
        for (const auto& [key, call] : session->inflight)
            m_host->cancel(call);
    }
}

bool McpServer::listen(const std::wstring& name, DWORD* error)
{
    if (m_pipe)
        return true;
    pipe::Server::Callbacks callbacks;
    // On the pipe's threads: over to this one, in order.
    callbacks.connected = [this](pipe::Server::Id id) {
        QMetaObject::invokeMethod(this, [this, id] {
            auto session = std::make_unique<Session>();
            session->id = id;
            session->since = QDateTime::currentDateTime();
            session->sink = [this, id](const QByteArray& line) {
                if (m_pipe)
                    m_pipe->send(id, line);
            };
            m_sessions[id] = std::move(session); // listed once it says who it is (initialize)
        }, Qt::QueuedConnection);
    };
    callbacks.identified = [this](pipe::Server::Id id, HANDLE token) {
        QMetaObject::invokeMethod(this, [this, id, token] {
            if (const auto it = m_sessions.find(id); it != m_sessions.end())
                it->second->token.reset(token);
            else if (token)
                ::CloseHandle(token);
        }, Qt::QueuedConnection);
    };
    callbacks.line = [this](pipe::Server::Id id, QByteArray line) {
        QMetaObject::invokeMethod(this, [this, id, line = std::move(line)] {
            if (const auto it = m_sessions.find(id); it != m_sessions.end())
                handle(*it->second, line);
        }, Qt::QueuedConnection);
    };
    callbacks.disconnected = [this](pipe::Server::Id id) {
        QMetaObject::invokeMethod(this, [this, id] { endSession(id); }, Qt::QueuedConnection);
    };
    auto server = std::make_unique<pipe::Server>(name, std::move(callbacks));
    if (!server->start(error))
        return false;
    m_pipe = std::move(server);
    return true;
}

void McpServer::stop()
{
    if (!m_pipe)
        return;
    m_pipe->stop(); // the connections' threads have ended; their last callbacks are queued
    std::vector<SessionId> ids;
    for (const auto& [id, session] : m_sessions) {
        if (id < m_nextLocalId)
            ids.push_back(id);
    }
    for (const SessionId id : ids)
        endSession(id);
    m_pipe.reset();
}

McpServer::SessionId McpServer::openSession(std::function<void(const QByteArray& line)> sink)
{
    auto session = std::make_unique<Session>();
    session->id = m_nextLocalId++;
    session->since = QDateTime::currentDateTime();
    session->sink = std::move(sink);
    const SessionId id = session->id;
    m_sessions[id] = std::move(session); // listed once it says who it is (initialize)
    return id;
}

void McpServer::closeSession(SessionId id)
{
    endSession(id);
}

void McpServer::receive(SessionId id, const QByteArray& line)
{
    if (const auto it = m_sessions.find(id); it != m_sessions.end())
        handle(*it->second, line);
}

void McpServer::endSession(SessionId id)
{
    const auto it = m_sessions.find(id);
    if (it == m_sessions.end())
        return;
    for (const auto& [key, call] : it->second->inflight)
        m_host->cancel(call);
    const bool listed = it->second->listed();
    m_sessions.erase(it);
    if (listed)
        emit sessionsChanged();
}

std::vector<McpServer::SessionInfo> McpServer::sessions() const
{
    std::vector<SessionInfo> out;
    for (const auto& [id, session] : m_sessions) {
        if (session->listed())
            out.push_back({id, session->client, session->clientVersion, session->since, session->calls, session->lastTool,
                session->lastCall, session->token.valid()});
    }
    // Initialized without clientInfo: nameless, last.
    std::ranges::stable_sort(out, [](const SessionInfo& a, const SessionInfo& b) { return !a.client.isEmpty() && b.client.isEmpty(); });
    return out;
}

void McpServer::handle(Session& session, const QByteArray& line)
{
    QString problem;
    const std::optional<jsonrpc::Message> message = jsonrpc::parse(line, &problem);
    if (!message) {
        session.sink(jsonrpc::errorResponse(QJsonValue::Null, jsonrpc::kInvalidRequest, u"Invalid request: "_s + problem));
        return;
    }
    switch (message->kind) {
    case jsonrpc::Message::Kind::Request:
        handleRequest(session, message->id, message->method, message->params);
        break;
    case jsonrpc::Message::Kind::Notification:
        handleNotification(session, message->method, message->params);
        break;
    case jsonrpc::Message::Kind::Response:
        break; // we send no requests
    }
}

void McpServer::handleRequest(Session& session, const QJsonValue& id, const QString& method, const QJsonObject& params)
{
    const auto reply = [&](const QJsonObject& result) { session.sink(jsonrpc::response(id, result)); };
    const auto fail = [&](int code, const QString& text) { session.sink(jsonrpc::errorResponse(id, code, text)); };

    if (method == u"initialize") {
        const QString requested = params.value(u"protocolVersion"_s).toString();
        QString version = QString::fromLatin1(kProtocolVersions[0]);
        for (const char* known : kProtocolVersions) {
            if (requested == QLatin1StringView(known))
                version = requested;
        }
        const QJsonObject client = params.value(u"clientInfo"_s).toObject();
        QString name = client.value(u"title"_s).toString();
        if (name.isEmpty())
            name = client.value(u"name"_s).toString();
        const bool wasListed = session.listed();
        if (!name.isEmpty()) { // initialized again without clientInfo: the name stays
            // ws is known by its name (whatever title it gives), and logged so.
            session.cli = client.value(u"name"_s).toString() == QLatin1StringView(kCliClient);
            session.client = session.cli ? QString::fromLatin1(kCliClient) : name;
            session.clientVersion = client.value(u"version"_s).toString();
        }
        session.protocolVersion = version;
        session.initialized = true;
        reply({{u"protocolVersion"_s, version},
            {u"capabilities"_s,
                QJsonObject {{u"tools"_s, QJsonObject {{u"listChanged"_s, true}}}, {u"prompts"_s, QJsonObject {{u"listChanged"_s, false}}},
                    // For ws (kCliContract); any client may see it, as it is said before the client is known.
                    {u"experimental"_s, QJsonObject {{QString::fromLatin1(kServerName), QJsonObject {{u"cli"_s, kCliContract}}}}}}},
            {u"serverInfo"_s,
                QJsonObject {{u"name"_s, QString::fromLatin1(kServerName)}, {u"title"_s, u"Win顺 · WinShun"_s}, {u"version"_s, m_version},
                    {u"description"_s, u"Search files, folders and text on this PC with Win顺"_s},
                    {u"websiteUrl"_s, u"https://github.com/LingCore/WinShun"_s}}},
            {u"instructions"_s, ToolHost::instructions()}});
        if (wasListed || session.listed())
            emit sessionsChanged();
        return;
    }
    if (method == u"ping") {
        reply({});
        return;
    }
    if (!session.initialized) {
        fail(jsonrpc::kUnavailable, u"Not initialized: send initialize first"_s);
        return;
    }
    if (method == u"tools/list") {
        reply({{u"tools"_s, m_host->tools()}});
    } else if (method == u"tools/call") {
        callTool(session, id, params);
    } else if (method == u"resources/list") {
        reply({{u"resources"_s, QJsonArray()}});
    } else if (method == u"resources/templates/list") {
        reply({{u"resourceTemplates"_s, QJsonArray()}});
    } else if (method == u"prompts/list") {
        reply({{u"prompts"_s, prompts()}});
    } else if (method == u"prompts/get") {
        getPrompt(session, id, params);
    } else if (method == u"logging/setLevel") {
        reply({});
    } else {
        fail(jsonrpc::kMethodNotFound, u"Method not found: "_s + method);
    }
}

// The prompts: one, "setup", where there are settings tools to set up with.
QJsonArray McpServer::prompts() const
{
    if (m_host->known(u"get_settings"_s) != ToolHost::Known::Yes)
        return {};
    return {QJsonObject {{u"name"_s, u"setup"_s}, {u"title"_s, u"配置 Win顺 / Set up Win顺"_s},
        {u"description"_s, u"Ask a few questions about how you use your computer, then propose and apply settings for Win顺."_s},
        {u"arguments"_s,
            QJsonArray {QJsonObject {{u"name"_s, u"focus"_s},
                {u"description"_s, u"An area to concentrate on, such as the clipboard, search, shortcuts or appearance (optional)"_s},
                {u"required"_s, false}}}}}};
}

void McpServer::getPrompt(Session& session, const QJsonValue& id, const QJsonObject& params)
{
    const QString name = params.value(u"name"_s).toString();
    if (name != u"setup" || prompts().isEmpty()) {
        session.sink(jsonrpc::errorResponse(id, jsonrpc::kInvalidParams, u"Unknown prompt: "_s + name));
        return;
    }
    const QString text = ToolHost::setupPrompt(params.value(u"arguments"_s).toObject().value(u"focus"_s).toString());
    session.sink(jsonrpc::response(id,
        QJsonObject {{u"description"_s, u"Set up Win顺 for how you use your computer"_s},
            {u"messages"_s,
                QJsonArray {QJsonObject {{u"role"_s, u"user"_s},
                    {u"content"_s, QJsonObject {{u"type"_s, u"text"_s}, {u"text"_s, text}}}}}}}));
}

void McpServer::handleNotification(Session& session, const QString& method, const QJsonObject& params)
{
    if (method == u"notifications/initialized") {
        session.ready = true;
    } else if (method == u"notifications/cancelled") {
        const QString key = jsonrpc::idKey(params.value(u"requestId"_s));
        if (const auto it = session.inflight.find(key); it != session.inflight.end()) {
            m_host->cancel(it->second);
            session.inflight.erase(it);
        }
    }
    // roots/list_changed and the rest: nothing to do
}

void McpServer::callTool(Session& session, const QJsonValue& id, const QJsonObject& params)
{
    const QString name = params.value(u"name"_s).toString();
    const QJsonValue arguments = params.value(u"arguments"_s);
    if (name.isEmpty() || (!arguments.isUndefined() && !arguments.isNull() && !arguments.isObject())) {
        session.sink(jsonrpc::errorResponse(id, jsonrpc::kInvalidParams, u"tools/call needs a tool name and an arguments object"_s));
        return;
    }
    switch (m_host->known(name)) {
    case ToolHost::Known::No:
        session.sink(jsonrpc::errorResponse(id, jsonrpc::kInvalidParams, u"Unknown tool: "_s + name));
        return;
    case ToolHost::Known::Off: {
        const QString reason = m_host->offReason(name);
        session.sink(jsonrpc::response(id,
            toolResult({{}, true,
                           reason.isEmpty()
                               ? u"The tool '%1' is turned off in Win顺's settings (设置 → AI 助手). Ask the user to turn it on there, then call it again."_s.arg(name)
                               : reason,
                           QString::fromLatin1(kErrorOff)},
                session.cli)));
        return;
    }
    case ToolHost::Known::Yes:
        break;
    }
    const std::uint64_t callId = m_nextCall++;
    const QString key = jsonrpc::idKey(id);
    session.inflight[key] = callId;
    if (session.cli) {
        // One per run of ws, named by what it was run for: its own
        // index_status probe (asked before most commands) names it only
        // when nothing else was asked.
        if (!std::exchange(session.cliCounted, true))
            ++m_cliTally.calls;
        const bool probe = name == u"index_status";
        if (!probe || !session.cliNamed)
            m_cliTally.lastTool = name;
        session.cliNamed = session.cliNamed || !probe;
        m_cliTally.lastCall = QDateTime::currentDateTime();
        emit cliCalled();
    } else {
        ++session.calls;
        session.lastTool = name;
        session.lastCall = QDateTime::currentDateTime();
        emit sessionsChanged();
    }
    const SessionId sessionId = session.id;
    m_host->call(callId, session.client, name, arguments.toObject(), [this, sessionId, id, key, name](ToolResult result) {
        const auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end())
            return; // the session ended meanwhile
        it->second->inflight.erase(key);
        it->second->sink(response(*it->second, id, name, result));
    }, {session.token.valid() ? session.token.get() : nullptr, session.cli});
}

// The answer to a tools/call. One longer than a line may be (pipe::
// kMaxLineBytes) would end the connection at the other end (the relay, ws)
// with nothing said: it becomes an error that says what to ask for instead.
QByteArray McpServer::response(const Session& session, const QJsonValue& id, const QString& tool, const ToolResult& result) const
{
    QByteArray line = jsonrpc::response(id, toolResult(result, session.cli));
    if (static_cast<std::size_t>(line.size()) <= pipe::kMaxLineBytes)
        return line;
    const QString size = QString::number(static_cast<double>(line.size()) / (1 << 20), 'f', 1);
    m_host->log(u"%1 %2 %3 → the answer, %4 MB, is too large to send: an error was sent instead"_s.arg(
        QDateTime::currentDateTime().toString(Qt::ISODate), session.client, tool, size));
    const ToolResult tooLarge {{}, true,
        u"The answer is too large to send: %1 MB, where one message holds at most %2 MB. Ask for less: a smaller limit (or max_chars), fewer folders, a more specific query, or size: false."_s
            .arg(size)
            .arg(pipe::kMaxLineBytes >> 20),
        QString::fromLatin1(kErrorTooLarge)};
    return jsonrpc::response(id, toolResult(tooLarge, session.cli));
}

QJsonObject McpServer::toolResult(const ToolResult& result, bool cli)
{
    if (result.isError) {
        QJsonObject out {{u"content"_s, QJsonArray {QJsonObject {{u"type"_s, u"text"_s}, {u"text"_s, result.errorText}}}}, {u"isError"_s, true}};
        // ws acts on the kind (its exit codes); an AI client reads the words,
        // and might hold a structured error against the tool's outputSchema.
        if (cli && !result.errorCode.isEmpty())
            out.insert(u"structuredContent"_s, QJsonObject {{u"error"_s, result.errorCode}});
        return out;
    }
    // ws reads the structured result alone: the same again as text would
    // double the answer (and its 50 000 items).
    if (cli)
        return {{u"content"_s, QJsonArray()}, {u"structuredContent"_s, result.structured}, {u"isError"_s, false}};
    const QString text = QString::fromUtf8(QJsonDocument(result.structured).toJson(QJsonDocument::Compact));
    return {{u"content"_s, QJsonArray {QJsonObject {{u"type"_s, u"text"_s}, {u"text"_s, text}}}},
        {u"structuredContent"_s, result.structured}, {u"isError"_s, false}};
}

void McpServer::toolsChanged()
{
    for (const auto& [id, session] : m_sessions) {
        // ws asks one thing and goes: it keeps no list of the tools.
        if (session->initialized && !session->cli)
            session->sink(jsonrpc::notification(u"notifications/tools/list_changed"_s));
    }
}

} // namespace ws::mcp
