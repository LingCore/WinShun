#pragma once
#include <QString>
#include <optional>
#include <functional>
namespace ws::contentopen {
std::optional<QString> arguments(const QString& program, const QString& path, int line);
void open(const QString& path, int line, std::function<void(bool)> done = {});
}
