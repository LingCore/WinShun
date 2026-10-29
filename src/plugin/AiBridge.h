#pragma once

// The plugins' commands as MCP tools (the plugin design, §6.2): for every
// enabled plugin whose manifest says `ai.expose`, and that the user has
// not switched off for the AI, each command is a tool
// `plugin_<id>_<command>` (a query; readOnlyHint) -- and, when the user
// allows actions, `plugin_<id>_<command>_run` (an action on one of the
// items; destructiveHint). The tools are handed to the AI's ToolHost as
// external tools and rebuilt as the catalog or the settings change; a
// plugin turned off keeps its tools known but off, so the model is told
// which plugin to ask the user about.

#include "Host.h"
#include "McpTools.h"

#include <QObject>
#include <QSet>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>

namespace ws::plugin {

class AiBridge : public QObject {
    Q_OBJECT

public:
    struct Options {
        bool enabled = false; // 设置 → AI 助手 → "AI 可以使用插件"
        bool actions = false; // "允许 AI 执行插件动作"
        QSet<QString> excluded; // plugins whose card switch "给 AI 用" is off
        bool operator==(const Options&) const = default;
    };

    AiBridge(Host* host, mcp::ToolHost* tools, QObject* parent = nullptr);
    ~AiBridge() override;

    void setOptions(Options options); // rebuilds when they differ
    const Options& options() const noexcept { return m_options; }
    void rebuild(); // the catalog changed (connected to it here already)

    // "plugin_winshun_tools_calc", "…_run": [a-z0-9_] only.
    static QString toolName(const QString& pluginId, const QString& commandId, bool run);
    // The tools a plugin would offer, by name, for the settings page.
    static QStringList toolNamesOf(const Manifest& manifest, bool actions);

private:
    struct ActionWait {
        std::function<void(mcp::ToolResult)> done;
        QString pluginId;
    };
    void onActionDone(std::uint64_t requestId, const QString& pluginId, const Effects& effects, bool ok, const QString& error);
    mcp::ExternalTool queryTool(const Entry& e, const Command& c, bool offered, const QString& offReason);
    mcp::ExternalTool actionTool(const Entry& e, const Command& c, bool offered, const QString& offReason);

    Host* m_host;
    mcp::ToolHost* m_tools;
    Options m_options;
    std::map<std::uint64_t, std::uint64_t> m_calls; // the ToolHost's call id -> the host's command call
    std::map<std::uint64_t, ActionWait> m_actions; // our action request id -> its tool call
    std::uint64_t m_nextAction = std::uint64_t {1} << 50; // apart from the launcher's request ids
};

} // namespace ws::plugin
