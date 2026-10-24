#include "JsonRpc.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

using namespace Qt::StringLiterals;

namespace ws::jsonrpc {

namespace {

bool validId(const QJsonValue& id)
{
    return id.isString() || id.isDouble();
}

} // namespace

std::optional<Message> parse(const QByteArray& line, QString* problem)
{
    const auto fail = [&](const QString& why) {
        if (problem)
            *problem = why;
        return std::nullopt;
    };
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError)
        return fail(u"not JSON: "_s + error.errorString());
    if (document.isArray())
        return fail(u"batches are not supported"_s);
    if (!document.isObject())
        return fail(u"not a JSON object"_s);
    const QJsonObject object = document.object();
    if (object.value(u"jsonrpc"_s).toString() != u"2.0")
        return fail(u"jsonrpc is not \"2.0\""_s);

    Message message;
    const QJsonValue id = object.value(u"id"_s);
    const QJsonValue method = object.value(u"method"_s);
    if (method.isString()) {
        message.method = method.toString();
        const QJsonValue params = object.value(u"params"_s);
        if (params.isObject())
            message.params = params.toObject();
        else if (!params.isUndefined() && !params.isNull())
            return fail(u"params is not an object"_s);
        if (id.isUndefined() || id.isNull()) {
            message.kind = Message::Kind::Notification;
        } else if (validId(id)) {
            message.kind = Message::Kind::Request;
            message.id = id;
        } else {
            return fail(u"id is not a string or a number"_s);
        }
        return message;
    }
    if (!method.isUndefined())
        return fail(u"method is not a string"_s);

    // A response: result or error, with the request's id (null with a
    // parse error, when the id could not be read).
    message.kind = Message::Kind::Response;
    if (!validId(id) && !id.isNull())
        return fail(u"a response needs an id"_s);
    message.id = id;
    const QJsonValue result = object.value(u"result"_s);
    const QJsonValue failure = object.value(u"error"_s);
    if (failure.isObject()) {
        message.failed = true;
        message.error = failure.toObject();
        if (!message.error.value(u"code"_s).isDouble() || !message.error.value(u"message"_s).isString())
            return fail(u"error needs a code and a message"_s);
        return message;
    }
    if (result.isUndefined())
        return fail(u"neither a method, a result nor an error"_s);
    message.result = result;
    return message;
}

QByteArray serialize(const QJsonObject& message)
{
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    // The framing forbids a newline inside a message: Qt's compact output has
    // none (every newline in a string is written as \n).
    line.append('\n');
    return line;
}

QByteArray request(const QJsonValue& id, const QString& method, const QJsonObject& params)
{
    QJsonObject message {{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, id}, {u"method"_s, method}};
    if (!params.isEmpty())
        message.insert(u"params"_s, params);
    return serialize(message);
}

QByteArray notification(const QString& method, const QJsonObject& params)
{
    QJsonObject message {{u"jsonrpc"_s, u"2.0"_s}, {u"method"_s, method}};
    if (!params.isEmpty())
        message.insert(u"params"_s, params);
    return serialize(message);
}

QByteArray response(const QJsonValue& id, const QJsonValue& result)
{
    return serialize({{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, id}, {u"result"_s, result}});
}

QByteArray responseWithResult(const QJsonValue& id, const QByteArray& result)
{
    // As serialize() writes the object above: its keys in order.
    const QByteArray key = idKey(id).toUtf8();
    QByteArray line;
    line.reserve(result.size() + key.size() + 40);
    line.append(R"({"id":)").append(key).append(R"(,"jsonrpc":"2.0","result":)").append(result).append("}\n");
    return line;
}

QJsonObject errorObject(int code, const QString& message, const QJsonValue& data)
{
    QJsonObject error {{u"code"_s, code}, {u"message"_s, message}};
    if (!data.isUndefined() && !data.isNull())
        error.insert(u"data"_s, data);
    return error;
}

QByteArray errorResponse(const QJsonValue& id, int code, const QString& message, const QJsonValue& data)
{
    return serialize({{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, id.isUndefined() ? QJsonValue(QJsonValue::Null) : id},
        {u"error"_s, errorObject(code, message, data)}});
}

QString idKey(const QJsonValue& id)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray {id}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

} // namespace ws::jsonrpc
