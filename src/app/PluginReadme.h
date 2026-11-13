#pragma once

#include <QByteArray>
#include <QString>

namespace ws::readme {
struct Document { QString html; QString error; };
bool exists(const QString& directory);
Document load(const QString& directory);
// The same passive text from bytes in hand: `extension` says what they are (md, txt, else HTML).
Document render(const QByteArray& bytes, const QString& extension);
// A README.md downloaded (the 插件库's 说明): no more than 1 MB.
Document fromMarkdown(const QByteArray& bytes);
bool webLink(const QString& link);
}
