#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>

#include <functional>
#include <optional>
#include <thread>

namespace ws::switchplugin {

// The provider's small Qt event-loop adapter for the public JSON-RPC
// protocol. A joined, cancellable reader owns the synchronous stdin read.
class Protocol : public QObject {
public:
    using Request = std::function<void(const QJsonValue&, const QString&, const QJsonObject&)>;
    Protocol(QByteArray pending, Request request, std::function<void()> closed);
    ~Protocol() override;
    static std::optional<QJsonObject> first(QByteArray& pending);
    void respond(const QJsonValue& id, const QJsonValue& result);
    void fail(const QJsonValue& id, int code, const QString& message);
    void notify(const QString& method, const QJsonObject& params);

private:
    void received(const QByteArray& line);
    void write(QJsonObject message);
    Request m_request;
    std::function<void()> m_closed;
    std::jthread m_reader;
};

} // namespace ws::switchplugin
