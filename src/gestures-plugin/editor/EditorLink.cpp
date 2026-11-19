#include "EditorLink.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QPointer>

#include <windows.h>

#include <string>
#include <thread>

namespace ws::gestureseditor {

namespace {

// A line the reader thread has split off, as it came.
bool takeLine(std::string& pending, QByteArray* line)
{
    const std::size_t newline = pending.find('\n');
    if (newline == std::string::npos)
        return false;
    *line = QByteArray(pending.data(), static_cast<qsizetype>(newline)).trimmed();
    pending.erase(0, newline + 1);
    return true;
}

} // namespace

std::optional<QJsonObject> EditorLink::readFirst(QByteArray* rest)
{
    std::string pending;
    char buffer[65536];
    DWORD got = 0;
    QByteArray line;
    while (!takeLine(pending, &line)) {
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

EditorLink::EditorLink(QByteArray pending, QObject* parent)
    : QObject(parent)
{
    // Detached: blocked in ReadFile until the plugin closes the pipe, which
    // ends the process anyway.
    std::thread([self = QPointer<EditorLink>(this), start = std::string(pending.constData(), static_cast<std::size_t>(pending.size()))]() mutable {
        std::string buffered = std::move(start);
        char buffer[65536];
        DWORD got = 0;
        for (;;) {
            QByteArray text;
            while (takeLine(buffered, &text)) {
                if (!text.isEmpty()) {
                    QMetaObject::invokeMethod(qApp, [self, text] {
                        if (self)
                            self->line(text);
                    }, Qt::QueuedConnection);
                }
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

void EditorLink::line(const QByteArray& text)
{
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(text, &error);
    if (!document.isObject()) {
        qWarning().noquote() << "EditorLink: not a JSON object:" << error.errorString();
        return;
    }
    emit received(document.object());
}

void EditorLink::send(const QJsonObject& message)
{
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    qsizetype done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        if (!::WriteFile(out, bytes.constData() + done, static_cast<DWORD>(bytes.size() - done), &written, nullptr) || written == 0)
            return; // the plugin has gone
        done += written;
    }
}

} // namespace ws::gestureseditor
