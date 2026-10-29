#include "Session.h"

using namespace Qt::StringLiterals;

namespace ws::plugin {

Session::Session(Sink sink)
    : m_sink(std::move(sink))
{
}

std::uint64_t Session::request(const QString& method, const QJsonObject& params, Reply reply)
{
    const std::uint64_t id = m_nextId++;
    m_pending.emplace(id, std::move(reply));
    QByteArray line = jsonrpc::request(static_cast<double>(id), method, params);
    line.chop(1);
    m_sink(line);
    return id;
}

void Session::notify(const QString& method, const QJsonObject& params)
{
    QByteArray line = jsonrpc::notification(method, params);
    line.chop(1);
    m_sink(line);
}

void Session::forget(std::uint64_t id)
{
    m_pending.erase(id);
}

void Session::respond(const QJsonValue& id, const QJsonValue& result)
{
    QByteArray line = jsonrpc::response(id, result);
    line.chop(1);
    m_sink(line);
}

void Session::respondError(const QJsonValue& id, int code, const QString& message)
{
    QByteArray line = jsonrpc::errorResponse(id, code, message);
    line.chop(1);
    m_sink(line);
}

void Session::receive(const QByteArray& line)
{
    QString problem;
    const std::optional<jsonrpc::Message> message = jsonrpc::parse(line, &problem);
    if (!message) {
        if (m_handlers.protocolError)
            m_handlers.protocolError(problem, line);
        return;
    }
    switch (message->kind) {
    case jsonrpc::Message::Kind::Response: {
        if (!message->id.isDouble()) {
            if (m_handlers.protocolError)
                m_handlers.protocolError(u"a response with an id that is not one of ours"_s, line);
            return;
        }
        const auto it = m_pending.find(static_cast<std::uint64_t>(message->id.toDouble()));
        if (it == m_pending.end())
            return; // forgotten, or answered twice
        Reply reply = std::move(it->second);
        m_pending.erase(it);
        reply(*message);
        return;
    }
    case jsonrpc::Message::Kind::Notification: {
        const QString& method = message->method;
        if (method == u"query/partial") {
            if (m_handlers.partial)
                m_handlers.partial(message->params);
        } else if (method == u"winshun/log") {
            if (m_handlers.log)
                m_handlers.log(message->params.value(u"level"_s).toString(u"info"_s), message->params.value(u"text"_s).toString());
        } else if (method == u"winshun/notify") {
            const QJsonObject& params = message->params;
            if (params.value(u"toast"_s).toBool() && m_handlers.toast)
                m_handlers.toast(params.value(u"title"_s).toString(), params.value(u"text"_s).toString(), params.value(u"click"_s).toArray());
            else if (m_handlers.notify)
                m_handlers.notify(params.value(u"text"_s).toString());
        } else if (method == u"winshun/progress") {
            if (m_handlers.progress)
                m_handlers.progress(static_cast<std::uint64_t>(message->params.value(u"queryId"_s).toDouble()),
                    message->params.value(u"text"_s).toString());
        } else if (method == u"winshun/refresh") {
            if (m_handlers.refresh)
                m_handlers.refresh();
        }
        // Others: nothing to do.
        return;
    }
    case jsonrpc::Message::Kind::Request: {
        if (message->method == u"winshun/tool") {
            const QString name = message->params.value(u"name"_s).toString();
            const QJsonValue arguments = message->params.value(u"arguments"_s);
            if (name.isEmpty() || (!arguments.isUndefined() && !arguments.isNull() && !arguments.isObject())) {
                respondError(message->id, jsonrpc::kInvalidParams, u"winshun/tool needs a name and an arguments object"_s);
                return;
            }
            if (m_handlers.toolCall)
                m_handlers.toolCall(message->id, name, arguments.toObject());
            else
                respondError(message->id, jsonrpc::kUnavailable, u"Tools are not available"_s);
            return;
        }
        respondError(message->id, jsonrpc::kMethodNotFound, u"Method not found: "_s + message->method);
        return;
    }
    }
}

void Session::failAll(int code, const QString& message)
{
    std::map<std::uint64_t, Reply> pending = std::move(m_pending);
    m_pending.clear();
    for (auto& [id, reply] : pending) {
        jsonrpc::Message response;
        response.kind = jsonrpc::Message::Kind::Response;
        response.id = static_cast<double>(id);
        response.failed = true;
        response.error = jsonrpc::errorObject(code, message);
        reply(response);
    }
}

} // namespace ws::plugin
