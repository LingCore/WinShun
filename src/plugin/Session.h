#pragma once

// A resident plugin's JSON-RPC session (the plugin design, appendix A):
// requests to the plugin with their replies waited for by id, its
// notifications and requests to Win顺 handed to handlers. Lines come in
// through receive() (its stdout) and go out through the sink (its stdin).
// Lives on the Host's thread.

#include "JsonRpc.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>

namespace ws::plugin {

// Server-defined errors Win顺 answers a plugin with.
inline constexpr int kPermissionDenied = -32001;

class Session {
public:
    using Sink = std::function<void(const QByteArray& line)>; // one message, without its newline
    using Reply = std::function<void(const jsonrpc::Message& response)>; // result, or failed with error

    struct Handlers {
        std::function<void(const QJsonObject& params)> partial; // query/partial
        std::function<void(const QString& level, const QString& text)> log; // winshun/log
        std::function<void(const QString& text)> notify; // winshun/notify
        // winshun/notify with "toast": true (a system notification; the Host
        // checks the permission): its title, text and the effects of a click.
        std::function<void(const QString& title, const QString& text, const QJsonArray& click)> toast;
        std::function<void(std::uint64_t queryId, const QString& text)> progress; // winshun/progress
        std::function<void()> refresh; // winshun/refresh: what the launcher shows changed by itself
        // winshun/tool: answer with respond() or respondError(), by id.
        std::function<void(const QJsonValue& id, const QString& tool, const QJsonObject& arguments)> toolCall;
        std::function<void(const QString& problem, const QByteArray& line)> protocolError;
    };

    explicit Session(Sink sink);
    void setHandlers(Handlers handlers) { m_handlers = std::move(handlers); }

    // A request; the id it got. `reply` runs once, when the plugin
    // answers, or at once with kCancelled from failAll().
    std::uint64_t request(const QString& method, const QJsonObject& params, Reply reply);
    void notify(const QString& method, const QJsonObject& params = {});
    void forget(std::uint64_t id); // the reply, should it come, is dropped
    void respond(const QJsonValue& id, const QJsonValue& result);
    void respondError(const QJsonValue& id, int code, const QString& message);

    void receive(const QByteArray& line); // one line of the plugin's stdout
    // The plugin is gone: every reply waited for gets `code`.
    void failAll(int code, const QString& message);
    int pending() const { return static_cast<int>(m_pending.size()); }

private:
    Sink m_sink;
    Handlers m_handlers;
    std::uint64_t m_nextId = 1;
    std::map<std::uint64_t, Reply> m_pending;
};

} // namespace ws::plugin
