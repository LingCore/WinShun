#include "TextField.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QQuickItem>
#include <QStringList>
#include <QStringTokenizer>

namespace ws::textfield {

bool takesTyping(const QQuickItem* item)
{
    // A read-only TextInput or TextEdit says no.
    return item && item->inputMethodQuery(Qt::ImEnabled).toBool();
}

bool isOneLine(const QQuickItem* item)
{
    return takesTyping(item) && !(item->inputMethodQuery(Qt::ImHints).toInt() & Qt::ImhMultiLine);
}

QString oneLine(QString text)
{
    if (!text.contains(u'\n') && !text.contains(u'\r'))
        return text;
    text.replace(u'\r', u'\n');
    QStringList lines;
    for (QStringView line : text.tokenize(u'\n', Qt::SkipEmptyParts)) {
        line = line.trimmed();
        if (!line.isEmpty())
            lines.append(line.toString());
    }
    return lines.join(u' ');
}

void select(QQuickItem* item, int anchor, int cursor)
{
    QInputMethodEvent event(QString(), {QInputMethodEvent::Attribute(QInputMethodEvent::Selection, anchor, cursor - anchor)});
    QCoreApplication::sendEvent(item, &event);
}

void type(QQuickItem* item, const QString& text)
{
    if (text.isEmpty())
        return;
    QInputMethodEvent event;
    event.setCommitString(text);
    QCoreApplication::sendEvent(item, &event);
}

bool pasteOneLine(QQuickItem* item)
{
    if (!isOneLine(item))
        return false;
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.contains(u'\n') && !text.contains(u'\r'))
        return false;
    type(item, oneLine(text));
    return true;
}

} // namespace ws::textfield
