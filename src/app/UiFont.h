#pragma once

#include <QJsonObject>
#include <QStringList>

namespace ws::uifont {

// Load the bundled face once; an empty preference always returns to it.
QJsonObject initialize();
QStringList families();
bool available(const QString& family);
QString resolvedFamily(const QString& family);
void apply(const QString& family);

} // namespace ws::uifont
