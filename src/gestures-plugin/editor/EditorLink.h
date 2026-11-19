// The window's side of its link to the plugin (EditorHost.h has the lines):
// one JSON object a line, UTF-8, on stdin (from the plugin) and stdout (to
// it). stdin is read on a thread of its own and handed to the GUI thread;
// stdout carries these lines and nothing else (Qt's messages go to stderr,
// which the plugin host logs).
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>

#include <optional>

namespace ws::gestureseditor {

class EditorLink : public QObject {
    Q_OBJECT

public:
    // `pending`: what was read past the first line (readFirst).
    explicit EditorLink(QByteArray pending, QObject* parent = nullptr);

    // The first line ("init"), read before Qt is set up: its look says how
    // (the renderer, the language). The bytes after it go to `rest`.
    static std::optional<QJsonObject> readFirst(QByteArray* rest);

    void send(const QJsonObject& message);

signals:
    void received(const QJsonObject& message);
    void closed(); // stdin ended: the plugin has gone

private:
    void line(const QByteArray& text);
};

} // namespace ws::gestureseditor
