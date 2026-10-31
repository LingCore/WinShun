#include "SwitchProtocol.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QPointer>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::switchplugin {
namespace {
constexpr qsizetype kMaxLine = 16 << 20;
bool takeLine(QByteArray& pending, QByteArray& line)
{
    const qsizetype end = pending.indexOf('\n');
    if (end < 0)
        return false;
    line = pending.first(end).trimmed();
    pending.remove(0, end + 1);
    return true;
}
} // namespace

std::optional<QJsonObject> Protocol::first(QByteArray& pending)
{
    char chunk[16384];
    QByteArray line;
    while (!takeLine(pending, line)) {
        DWORD got = 0;
        if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), chunk, sizeof chunk, &got, nullptr) || got == 0)
            return std::nullopt;
        pending.append(chunk, got);
        if (pending.size() > kMaxLine)
            return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject())
        return std::nullopt;
    return document.object();
}

Protocol::Protocol(QByteArray pending, Request request, std::function<void()> closed)
    : m_request(std::move(request)), m_closed(std::move(closed))
{
    const QPointer self(this);
    m_reader = std::jthread([self, pending = std::move(pending)](std::stop_token stop) mutable {
        char chunk[16384];
        while (!stop.stop_requested()) {
            QByteArray line;
            while (!stop.stop_requested() && takeLine(pending, line)) {
                if (line.size() > kMaxLine) {
                    QMetaObject::invokeMethod(self, [self] { if (self) self->m_closed(); }, Qt::QueuedConnection);
                    return;
                }
                if (!line.isEmpty())
                    QMetaObject::invokeMethod(self, [self, line = std::move(line)] {
                        if (self)
                            self->received(line);
                    }, Qt::QueuedConnection);
            }
            if (stop.stop_requested())
                break;
            DWORD got = 0;
            if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), chunk, sizeof chunk, &got, nullptr) || got == 0)
                break;
            pending.append(chunk, got);
            if (pending.size() > kMaxLine)
                break;
        }
        if (!stop.stop_requested())
            QMetaObject::invokeMethod(self, [self] { if (self) self->m_closed(); }, Qt::QueuedConnection);
    });
}

Protocol::~Protocol()
{
    if (!m_reader.joinable())
        return;
    m_reader.request_stop();
    // Retrying also covers the small gap between the stop check and ReadFile.
    // Never leave a detached reader referring to a destroyed QCoreApplication.
    const HANDLE thread = m_reader.native_handle();
    while (::WaitForSingleObject(thread, 0) == WAIT_TIMEOUT) {
        ::CancelSynchronousIo(thread);
        ::WaitForSingleObject(thread, 10);
    }
    m_reader.join();
}

void Protocol::received(const QByteArray& line)
{
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject())
        return;
    const QJsonObject message = document.object();
    if (message.value(u"jsonrpc"_s) != u"2.0"_s || !message.value(u"method"_s).isString())
        return;
    m_request(message.value(u"id"_s), message.value(u"method"_s).toString(), message.value(u"params"_s).toObject());
}

void Protocol::write(QJsonObject message)
{
    message.insert(u"jsonrpc"_s, u"2.0"_s);
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    qsizetype at = 0;
    while (at < line.size()) {
        DWORD put = 0;
        if (!::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), line.constData() + at,
                static_cast<DWORD>(line.size() - at), &put, nullptr) || put == 0) {
            m_closed();
            return;
        }
        at += put;
    }
}

void Protocol::respond(const QJsonValue& id, const QJsonValue& result) { write({{u"id"_s, id}, {u"result"_s, result}}); }
void Protocol::fail(const QJsonValue& id, int code, const QString& message)
{
    write({{u"id"_s, id}, {u"error"_s, QJsonObject {{u"code"_s, code}, {u"message"_s, message}}}});
}
void Protocol::notify(const QString& method, const QJsonObject& params) { write({{u"method"_s, method}, {u"params"_s, params}}); }

} // namespace ws::switchplugin
