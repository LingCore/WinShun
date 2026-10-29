#include "Protocol.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QPointer>

#include <windows.h>

#include <string>
#include <thread>

using namespace Qt::StringLiterals;

namespace ws::captureplugin {

namespace {

// Lines the reader thread has split off, as they came.
bool readLine(std::string& pending, QByteArray* line)
{
    const std::size_t nl = pending.find('\n');
    if (nl == std::string::npos)
        return false;
    *line = QByteArray(pending.data(), static_cast<qsizetype>(nl)).trimmed();
    pending.erase(0, nl + 1);
    return true;
}

} // namespace

std::optional<QJsonObject> Protocol::readFirst(QByteArray* rest)
{
    std::string pending;
    char buffer[65536];
    DWORD got = 0;
    QByteArray line;
    while (!readLine(pending, &line)) {
        if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof buffer, &got, nullptr) || got == 0)
            return std::nullopt;
        pending.append(buffer, got);
    }
    *rest = QByteArray(pending.data(), static_cast<qsizetype>(pending.size()));
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject())
        return std::nullopt;
    return document.object();
}

Protocol::Protocol(QByteArray pending, QObject* parent)
    : QObject(parent)
{
    // Detached: blocked in ReadFile until Win顺 closes the pipe, which ends
    // the process anyway (shutdown, or the job's end).
    std::thread([self = QPointer<Protocol>(this), start = std::string(pending.constData(), pending.size())]() mutable {
        std::string buffered = std::move(start);
        char buffer[65536];
        DWORD got = 0;
        for (;;) {
            QByteArray line;
            while (readLine(buffered, &line)) {
                if (!line.isEmpty())
                    QMetaObject::invokeMethod(qApp, [self, line] {
                        if (self)
                            self->received(line);
                    }, Qt::QueuedConnection);
            }
            if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof buffer, &got, nullptr) || got == 0)
                break;
            buffered.append(buffer, got);
        }
        QMetaObject::invokeMethod(qApp, [self] {
            if (self)
                emit self->closed();
        }, Qt::QueuedConnection);
    }).detach();
}

Protocol::~Protocol() = default;

void Protocol::received(const QByteArray& line)
{
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (!document.isObject()) {
        qWarning().noquote() << "Protocol: not a JSON object:" << error.errorString();
        return;
    }
    const QJsonObject message = document.object();
    const QJsonValue id = message.value(u"id"_s);
    const QString method = message.value(u"method"_s).toString();
    if (method.isEmpty()) {
        // An answer to one of ours.
        const auto it = m_replies.find(id.toString());
        if (it == m_replies.end())
            return;
        const Reply reply = std::move(it->second);
        m_replies.erase(it);
        if (!reply)
            return;
        const QJsonObject failure = message.value(u"error"_s).toObject();
        if (!failure.isEmpty())
            reply({}, failure.value(u"message"_s).toString(u"error %1"_s.arg(failure.value(u"code"_s).toInt())));
        else
            reply(message.value(u"result"_s), {});
        return;
    }
    const QJsonObject params = message.value(u"params"_s).toObject();
    if (id.isUndefined() || id.isNull())
        emit notified(method, params);
    else
        emit requested(id, method, params);
}

void Protocol::write(const QJsonObject& message)
{
    QJsonObject full = message;
    full.insert(u"jsonrpc"_s, u"2.0"_s);
    const QByteArray line = QJsonDocument(full).toJson(QJsonDocument::Compact) + '\n';
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    qsizetype done = 0;
    while (done < line.size()) {
        DWORD written = 0;
        if (!::WriteFile(out, line.constData() + done, static_cast<DWORD>(line.size() - done), &written, nullptr) || written == 0)
            return; // Win顺 has gone
        done += written;
    }
}

void Protocol::respond(const QJsonValue& id, const QJsonValue& result)
{
    write({{u"id"_s, id}, {u"result"_s, result}});
}

void Protocol::respondError(const QJsonValue& id, int code, const QString& message)
{
    write({{u"id"_s, id}, {u"error"_s, QJsonObject {{u"code"_s, code}, {u"message"_s, message}}}});
}

void Protocol::notify(const QString& method, const QJsonObject& params)
{
    write({{u"method"_s, method}, {u"params"_s, params}});
}

void Protocol::request(const QString& method, const QJsonObject& params, Reply reply)
{
    const QString id = u"c%1"_s.arg(m_nextId++);
    m_replies[id] = std::move(reply);
    write({{u"id"_s, id}, {u"method"_s, method}, {u"params"_s, params}});
}

void Protocol::tool(const QString& name, const QJsonObject& arguments, Reply reply)
{
    request(u"winshun/tool"_s, {{u"name"_s, name}, {u"arguments"_s, arguments}},
        [name, reply = std::move(reply)](const QJsonValue& result, const QString& error) {
            if (!error.isEmpty())
                qWarning().noquote() << "Win顺's tool" << name << "failed:" << error;
            if (reply)
                reply(result, error);
        });
}

void Protocol::log(const QString& level, const QString& text)
{
    notify(u"winshun/log"_s, {{u"level"_s, level}, {u"text"_s, text}});
}

} // namespace ws::captureplugin
