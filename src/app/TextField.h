#pragma once

#include <QString>

class QQuickItem;

// Win顺's text fields (QML TextInput, or anything that takes text the way an
// input method types it): what goes into them, and how.
namespace ws::textfield {

// Whether `item` takes typing: a text field, not read-only.
bool takesTyping(const QQuickItem* item);
// ... and on one line (a TextInput, not a TextEdit).
bool isOneLine(const QQuickItem* item);

// Several lines as one, as browsers paste them into a one-line field: each
// line trimmed, the empty ones dropped, the rest joined by spaces. Text on
// one line stays as it is.
QString oneLine(QString text);

// The caret and the selection of `item` where they were, as its input
// method queries gave them (Qt::ImAnchorPosition, Qt::ImCursorPosition).
void select(QQuickItem* item, int anchor, int cursor);
// Types `text` into `item` as its input method would: over the selection,
// as one step for Ctrl+Z.
void type(QQuickItem* item, const QString& text);

// Ctrl+V or Shift+Insert into `item`: when it is a one-line field and the
// clipboard holds several lines, they go in as one line and this returns
// true. A TextInput keeps the line breaks in its text: it shows them as
// spaces, but they are searched for, and shown as lines elsewhere. False:
// the field pastes for itself.
bool pasteOneLine(QQuickItem* item);

} // namespace ws::textfield
