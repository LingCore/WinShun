#pragma once

#include <QString>

namespace ws::readme {
struct Document { QString html; QString error; };
bool exists(const QString& directory);
Document load(const QString& directory);
bool webLink(const QString& link);
}
