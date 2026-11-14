#pragma once

#include <QJsonObject>
#include <QStringList>

namespace ws::uifont {

// Load the bundled face once; an empty preference always returns to it.
QJsonObject initialize();
// The installed fonts, by the names Windows lists them under for this user.
QStringList families();
// The name among families() of the font `family` names, in whichever of its
// languages ("霞鹜文楷" for "LXGW WenKai" on a Chinese system); empty when no
// such font is installed.
QString listedFamily(const QString& family);
bool available(const QString& family);
// listedFamily, or the bundled default's family when there is none.
QString resolvedFamily(const QString& family);
void apply(const QString& family);

} // namespace ws::uifont
