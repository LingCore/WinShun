#pragma once

// JSON-RPC 2.0 messages, one per line of UTF-8 (as MCP's stdio transport
// frames them, and as Win顺's pipe does): parsed from a line, and made into
// one. Qt Core only: the relay (WinShunMcp.exe) uses it too.

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <optional>

namespace ws::jsonrpc {

// Standard error codes.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
// Server-defined: Win顺 (or its relay) cannot serve the request now.
inline constexpr int kUnavailable = -32000;
// Server-defined: the request was cancelled (notifications/cancelled, or the
// session ended).
inline constexpr int kCancelled = -32800;

struct Message {
    enum class Kind { Request, Notification, Response };
    Kind kind = Kind::Request;
    QJsonValue id; // Request, Response; a string or a number
    QString method; // Request, Notification
    QJsonObject params; // Request, Notification; empty when none (MCP's are objects)
    QJsonValue result; // Response, when it succeeded
    QJsonObject error; // Response, when it failed: code, message, data
    bool failed = false; // Response: it is an error

    bool isRequest() const noexcept { return kind == Kind::Request; }
    bool isNotification() const noexcept { return kind == Kind::Notification; }
    bool isResponse() const noexcept { return kind == Kind::Response; }
};

// One line (without its newline). `problem` says why it is not a message.
// A JSON-RPC batch (an array) is not accepted: MCP dropped them.
std::optional<Message> parse(const QByteArray& line, QString* problem = nullptr);

// The JSON of one message, compact, with its newline.
QByteArray serialize(const QJsonObject& message);
QByteArray request(const QJsonValue& id, const QString& method, const QJsonObject& params = {});
QByteArray notification(const QString& method, const QJsonObject& params = {});
QByteArray response(const QJsonValue& id, const QJsonValue& result);
// The same with the result made into compact JSON already (elsewhere, on
// another thread): byte for byte what response() writes.
QByteArray responseWithResult(const QJsonValue& id, const QByteArray& result);
QByteArray errorResponse(const QJsonValue& id, int code, const QString& message, const QJsonValue& data = {});
QJsonObject errorObject(int code, const QString& message, const QJsonValue& data = {});

// An id as a key (its JSON): "1", "\"abc\"".
QString idKey(const QJsonValue& id);

} // namespace ws::jsonrpc
