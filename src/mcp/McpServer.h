#pragma once

// The MCP server: sessions over the pipe (one per relay, so one per AI
// client), the protocol's lifecycle and the tools/* methods, answered with
// the ToolHost's tools. Lives on one thread with an event loop (the GUI
// thread in Win顺); the pipe's threads hand it lines through the queue.

#include "McpTools.h"

#include <QByteArray>
#include <QJsonArray>
#include <QDateTime>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace ws::pipe {
class Server;
}

namespace ws::mcp {

// The protocol revisions answered. The client's is echoed when it is one of
// these; else the newest.
inline constexpr const char* kProtocolVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
inline constexpr const char* kServerName = "winshun";
// What ws may rely on, said in the initialize result as
// capabilities.experimental.winshun = {"cli": kCliContract}: the command
// line's limits, the byte budget (truncated), folders, answers without the
// text copy, and the error kinds not_found / off / too_large. Raised
// whenever any of that changes; ws checks it before relying on any of it.
inline constexpr int kCliContract = 1;

class McpServer : public QObject {
    Q_OBJECT

public:
    using SessionId = std::uint64_t;
    struct SessionInfo {
        SessionId id = 0;
        QString client; // clientInfo.title, else its name
        QString clientVersion;
        QDateTime since;
        int calls = 0;
        QString lastTool;
        QDateTime lastCall;
        bool identified = false; // it brought a token of its own (pipe sessions); else the default one is used
    };
    // What ws, the command line, has asked since the server started. Each
    // run of ws is a session of its own, a few milliseconds long, many a
    // minute: they are counted here, not listed (sessions()).
    struct CliTally {
        int calls = 0; // runs of ws that called a tool: one each, however many calls it made
        // The last run's tool: index_status only when the run asked nothing
        // else (ws status), as ws asks it first to see that it may go on.
        QString lastTool;
        QDateTime lastCall;
    };
    // The client name ws gives (clientInfo.name). It changes formats and
    // limits only (CallOptions::cli), never what may be seen: any program
    // could say it.
    static constexpr const char* kCliClient = "ws";

    McpServer(ToolHost* host, QString version, QObject* parent = nullptr);
    ~McpServer() override;

    // The pipe: start serving on `name` (Pipe.h); false with the Win32 code
    // when it cannot be made.
    bool listen(const std::wstring& name, DWORD* error = nullptr);
    void stop(); // ends the sessions and the pipe
    bool listening() const noexcept { return m_pipe != nullptr; }

    // Sessions without a pipe (tests): what the server sends goes to `sink`.
    SessionId openSession(std::function<void(const QByteArray& line)> sink);
    void closeSession(SessionId id);
    void receive(SessionId id, const QByteArray& line); // one message

    // The AI clients' sessions, once they introduced themselves (initialize);
    // never ws's (cliTally()).
    std::vector<SessionInfo> sessions() const;
    int sessionCount() const { return static_cast<int>(m_sessions.size()); } // every connection, ws's too
    const CliTally& cliTally() const noexcept { return m_cliTally; }

signals:
    void sessionsChanged(); // what sessions() lists changed: one came, went, or called a tool
    void cliCalled(); // ws called a tool (cliTally())

private:
    struct Session;
    void handle(Session& session, const QByteArray& line);
    void handleRequest(Session& session, const QJsonValue& id, const QString& method, const QJsonObject& params);
    void handleNotification(Session& session, const QString& method, const QJsonObject& params);
    void callTool(Session& session, const QJsonValue& id, const QJsonObject& params);
    QJsonArray prompts() const;
    void getPrompt(Session& session, const QJsonValue& id, const QJsonObject& params);
    void endSession(SessionId id);
    void toolsChanged();
    static QJsonObject toolResult(const ToolResult& result, bool cli);
    QByteArray response(const Session& session, const QJsonValue& id, const QString& tool, const ToolResult& result) const;

    ToolHost* m_host;
    QString m_version;
    std::unique_ptr<pipe::Server> m_pipe;
    std::map<SessionId, std::unique_ptr<Session>> m_sessions;
    SessionId m_nextLocalId = SessionId {1} << 40; // in-process sessions, apart from the pipe's ids
    std::uint64_t m_nextCall = 1;
    CliTally m_cliTally;
};

} // namespace ws::mcp
