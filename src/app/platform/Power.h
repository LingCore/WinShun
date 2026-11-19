#pragma once

// The `power` tool of a plugin with the system:power permission (0.41.0): shut
// the computer down, restart it, put it to sleep or hibernate it. A plugin
// runs on a restricted token that has no SeShutdownPrivilege, so Win顺 does it
// with its own.

#include "McpTools.h"

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace ws::power {

enum class Action { Shutdown, Restart, Sleep, Hibernate };

// "shutdown", "restart", "sleep" or "hibernate"; nothing for any other text.
std::optional<Action> parseAction(const QString& name);
QString actionName(Action action);

// Does it for real and says why not: empty when it is done, or for sleep and
// hibernate when it is under way (the call returns after the computer woke).
// Shutdown and restart force nothing, so a program with unsaved work can still
// hold them up, as it can for Start's 关机.
using Performer = std::function<QString(Action)>;
// Tests: this stands in for the real one, so nothing happens to the computer;
// an empty one puts the real one back.
void setPerformerForTest(Performer performer);

// The tool. `arguments.action` is one of the four above. Shutdown and restart
// are answered when Windows took the request; sleep and hibernate when the
// request has failed or, if it has not within a moment, when the computer is
// on its way down. `done` is called once, on the calling thread.
void run(const QString& pluginId, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done);

} // namespace ws::power
