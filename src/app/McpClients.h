#pragma once

// The AI clients Win顺 can be added to as an MCP server (through
// WinShunMcp.exe, src/mcp): which are installed, which already name it, and
// writing it into their configuration (settings → AI 助手 → 连接).

#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>
#include <vector>

namespace ws::mcpclients {

struct Client {
    QString id; // claude-desktop | claude-code | cursor | vscode
    QString name; // "Claude Desktop"
    bool installed = false;
    bool connected = false; // its configuration names the relay next to this program
    QString configPath; // the file the connection is written to (or read from)
    QString note; // what happens after connecting ("重启 Claude Desktop 后生效")
    // Claude Code only: its Skill for the command line ws (see installSkill).
    bool skillInstalled = false;
    QString skillPath;
};

// WinShunMcp.exe next to this program, native; empty when it is not there.
QString relayPath();
std::vector<Client> detect();
QVariantList toVariant(const std::vector<Client>& clients); // for QML: {id, name, installed, connected, configPath, note, skillInstalled, skillPath}

struct Result {
    bool ok = false;
    QString message; // for the user, either way
};
// Writes the relay into the client's configuration (a JSON file merged, or
// Claude Code's own command run). `done` runs on the calling thread, later
// for Claude Code (its command takes a moment), at once for the others.
void connect(const QString& id, QObject* context, std::function<void(Result)> done);
// What to paste by hand: the JSON for the client's configuration file, or
// Claude Code's command; the generic JSON for an unknown id.
QString snippet(const QString& id);

// Claude Code's Skill: a SKILL.md that teaches it the command line ws (and
// saves the context an MCP server's tools cost). It lives in
// %CLAUDE_CONFIG_DIR%\skills\winshun, else in %USERPROFILE%\.claude\skills\winshun.
QString skillPath(); // the file, native
// Has `wsExe skill --install claude` write it, with the user's normal rights
// (never elevated: the file must be theirs), off the GUI thread. `done`
// runs on the GUI thread, and not at all once `context` is gone (at once,
// before this returns, when there is no ws.exe). The Skill carries ws.exe's full path, so
// it works with the folder not on the PATH too.
void installSkill(const QString& wsExe, QObject* context, std::function<void(Result)> done);

} // namespace ws::mcpclients
