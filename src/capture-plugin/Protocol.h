#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <optional>

namespace ws::captureplugin {

// The plugin's side of Win顺's plugin protocol (docs/plugins.md, 常驻):
// JSON-RPC 2.0 on stdin and stdout, a line each. stdin is read on a thread
// of its own and handed to the GUI thread; stdout carries the protocol and
// nothing else (Qt's messages go to stderr, which the host logs).
class Protocol : public QObject {
    Q_OBJECT
public:
    // `pending`: what was read past the first line (readFirst).
    explicit Protocol(QByteArray pending, QObject* parent = nullptr);
    ~Protocol() override;

    // The first line, the host's initialize request, read before Qt is set
    // up: its params say how (the renderer, the language). The bytes after
    // it go to `rest`.
    static std::optional<QJsonObject> readFirst(QByteArray* rest);

    void respond(const QJsonValue& id, const QJsonValue& result);
    void respondError(const QJsonValue& id, int code, const QString& message);
    void notify(const QString& method, const QJsonObject& params = {});
    // A request to Win顺: `reply` gets the result, or the error's message.
    using Reply = std::function<void(const QJsonValue& result, const QString& error)>;
    void request(const QString& method, const QJsonObject& params, Reply reply = {});
    // winshun/tool: one of Win顺's tools.
    void tool(const QString& name, const QJsonObject& arguments, Reply reply = {});
    void log(const QString& level, const QString& text);

signals:
    void requested(const QJsonValue& id, const QString& method, const QJsonObject& params);
    void notified(const QString& method, const QJsonObject& params);
    void closed(); // stdin ended: Win顺 has gone

private:
    void received(const QByteArray& line);
    void write(const QJsonObject& message);

    std::map<QString, Reply> m_replies;
    quint64 m_nextId = 1;
};

} // namespace ws::captureplugin
