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

enum class Action { Shutdown, Restart, Sleep, Hibernate, Lock, SignOut };

// "shutdown", "restart", "sleep", "hibernate", "lock" or "signout"; nothing for any other text.
std::optional<Action> parseAction(const QString& name);
QString actionName(Action action);

// Does it for real and says why not: empty when it is done, or for sleep and
// hibernate when it is under way (the call returns after the computer woke).
// Shutdown, restart and sign-out force nothing, so a program with unsaved work
// can still hold them up, as it can for Start's 关机.
using Performer = std::function<QString(Action)>;
// Tests: this stands in for the real one, so nothing happens to the computer;
// an empty one puts the real one back.
void setPerformerForTest(Performer performer);

// Launcher-initiated power/session command (GitHub #428): runs all six actions
// through the same performer (and test hook). `done` is called once on the
// calling thread with an error message, or empty on success / suspend started.
void execute(Action action, std::function<void(QString error)> done);

// The plugin tool (`system:power`). `arguments.action` is one of the four
// documented plugin actions (shutdown, restart, sleep, hibernate). Shutdown and
// restart are answered when Windows took the request; sleep and hibernate when
// the request has failed or, if it has not within a moment, when the computer
// is on its way down. `done` is called once, on the calling thread.
void run(const QString& pluginId, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done);

} // namespace ws::power
