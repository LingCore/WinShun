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
// protocol. A joined, cancellable reader owns the synchronous read of `in`
// (stdin, or the plugin host's pipe when it runs inside the host).
class Protocol : public QObject {
public:
    using Request = std::function<void(const QJsonValue&, const QString&, const QJsonObject&)>;
    // `in` and `out`: Win32 handles, kept open by the caller while this lives.
    Protocol(void* in, void* out, QByteArray pending, Request request, std::function<void()> closed);
    ~Protocol() override;
    static std::optional<QJsonObject> first(void* in, QByteArray& pending);
    void respond(const QJsonValue& id, const QJsonValue& result);
    void fail(const QJsonValue& id, int code, const QString& message);
    void notify(const QString& method, const QJsonObject& params);

private:
    void received(const QByteArray& line);
    void write(QJsonObject message);
    Request m_request;
    std::function<void()> m_closed;
    void* m_out;
    std::jthread m_reader;
};

} // namespace ws::switchplugin
