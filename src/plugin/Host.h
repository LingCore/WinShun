#pragma once

// The plugin system's face to the launcher (the plugin design, §2): on the
// GUI thread, like McpServer. A query goes in with its id; items come back
// as signals, and only for the newest query. Behind it: the catalog, the
// runner that starts plugin processes with the user's normal rights, a
// JSON-RPC session per resident plugin, zero-code commands done in place,
// debouncing of once plugins, timeouts, restarts with backoff (1, 5, 30 s;
// three crashes in five minutes and the plugin is off), idle exits, and a
// cap on resident processes.

#include "Catalog.h"
#include "Items.h"
#include "JsonRpc.h"
#include "McpTools.h"
#include "Runner.h"
#include "SearchTypes.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace ws::plugin {

// What answers a plugin's winshun/tool: the MCP tools (ToolHost) in the
// app; a fake in tests; nothing in wsbench.
struct ToolBridge {
    std::function<void(std::uint64_t callId, const QString& client, const QString& tool, const QJsonObject& arguments,
        std::function<void(mcp::ToolResult)> done)>
        call;
    std::function<void(std::uint64_t callId)> cancel;
    std::function<mcp::ToolHost::Known(const QString& tool)> known;
};
ToolBridge toolBridge(mcp::ToolHost* host);

// A strict plugin's data folder: a low integrity label (inherited by what is
// made in it), the only place of the user's such a plugin may write to.
// labelLow also works where the user lacks WRITE_OWNER (an unelevated copy's
// folder on a second disk): it grants the user, the folder's owner, full
// control first.
bool labelLow(const QString& path);
bool isLabelledLow(const QString& path); // an inherited low (or lower) label

inline constexpr int kToolFailed = -32002; // winshun/tool: the tool ran and failed (message says how)

class Host : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString userPluginDir; // %LOCALAPPDATA%\WinShun\plugins
        QString bundledPluginDir; // <install dir>\plugins; may be empty
        QString dataRoot; // where <id>.data folders go; empty: userPluginDir
        QString logPath; // plugins.log; empty: no file
        QString runnerExe; // WinShunPluginHost.exe
        QString locale = QStringLiteral("zh");
        QString theme = QStringLiteral("light");
        QString version; // of Win顺
        int maxResident = 8;
    };

    explicit Host(Options options, QObject* parent = nullptr);
    ~Host() override; // shutdown()

    Catalog& catalog() noexcept { return m_catalog; }
    const Options& options() const noexcept { return m_options; }
    void setToolBridge(ToolBridge bridge) { m_bridge = std::move(bridge); }
    // The interface's language and theme changed: for the plugins started
    // from now on (the ones running keep what they were told).
    void setAppearance(const QString& locale, const QString& theme);

    // From the settings (stage 2 wires the INI): which plugins are on,
    // keywords the user changed, the [WebSearch] keywords, each plugin's
    // settings (resident ones hear settingsChanged).
    void setEnabled(const QString& id, bool on);
    void setKeywordOverrides(const QHash<QString, QString>& overrides) { m_catalog.setKeywordOverrides(overrides); }
    void setWebKeywords(const QList<std::pair<QString, QString>>& keywords) { m_catalog.setWebKeywords(keywords); }
    void setFavoriteKeywords(const QList<std::pair<QString, QString>>& keywords) { m_catalog.setFavoriteKeywords(keywords); }
    void setBuiltinKeywords(const QList<std::pair<QString, QString>>& keywords) { m_catalog.setBuiltinKeywords(keywords); }
    void setPluginSettings(const QString& id, const QJsonObject& settings);
    QJsonObject pluginSettings(const QString& id) const; // the manifest's defaults under the user's values

    // A query: true when some plugin took it (queryTaken() for each, then
    // results() and the rest follow for `queryId`). Every earlier query is
    // cancelled. Plugins only answer in 全部.
    bool query(const QString& text, Scope scope, std::uint64_t queryId, const QJsonObject& filters = {});
    void cancel(std::uint64_t queryId);
    void cancelAll();
    // An action on an item (one with no declarative open/copy, or any
    // other the launcher wants the plugin to do): actionDone() follows.
    // `item`: as the plugin gave it (itemToJson), sent back as it came.
    void action(std::uint64_t requestId, const QString& pluginId, const QString& commandId, const QString& actionId,
        const Item& item, const QString& text);
    // `files`: a `files` trigger chosen on the result menu (Catalog::
    // fileCommands; GitHub #113): the action "files" with these paths, no
    // item (resident: params trigger "files" and files; once: WS_TRIGGER and
    // WS_FILES).
    void action(std::uint64_t requestId, const QString& pluginId, const QString& commandId, const QString& actionId,
        const QJsonObject& item, const QString& text, const QStringList& files = {});
    // A command asked from outside the launcher (an AI's tool call through
    // AiBridge, the plugin design §6.2): one query, answered by `reply`
    // once, on this thread. Unlike query(), it cancels nothing of the
    // launcher's and is cancelled by none of its keys; a plugin not running
    // is started and the call waits for its initialize. The call's id, for
    // cancelCommand().
    using CommandReply = std::function<void(bool ok, const ws::plugin::Items& items, const QString& error)>;
    std::uint64_t callCommand(const QString& pluginId, const QString& commandId, const QString& text, int limit, CommandReply reply);
    void cancelCommand(std::uint64_t callId); // the reply is not given
    void preload(); // starts the resident plugins that want starting early (eager)
    void shutdown(bool grace = true); // on exit: shutdown requests, two seconds (not without grace), then everything ends
    // Ends a plugin's processes (before its folder is replaced or removed):
    // the resident one is asked to shut down, once ones are ended. It comes
    // back when next asked for. stateChanged() follows as they go.
    void stop(const QString& id);
    // The settings page's "重新加载": its process ends and, if it was
    // running, starts again; the catalog reads its folder again.
    void reload(const QString& id);
    // Developer mode (the plugin design, §5.3): a script changing under a
    // plugin's folder restarts its resident process (reloaded() tells), and
    // every line to and from the plugins is traced().
    void setDeveloperMode(bool on);
    bool developerMode() const noexcept { return m_developerMode; }

    struct State {
        enum class Status { Off, Ready, Starting, Running, Backoff, Crashed, Unusable };
        Status status = Status::Off;
        quint32 pid = 0;
        int crashes = 0; // within the last five minutes
        int timeouts = 0; // in a row, of the resident process
        QString error; // Unusable (manifest, missing program) or the last crash
    };
    State state(const QString& id) const;
    Runner::TokenSource tokenSource() const { return m_runner ? m_runner->tokenSource() : Runner::TokenSource::None; }
    QJsonObject runnerFacts() const { return m_runner ? m_runner->facts() : QJsonObject(); }
    bool runnerReady() const { return m_runner && m_runner->isReady(); }

    // Tests: the backoff delays multiplied by this (1 = the real ones).
    void setBackoffScale(double scale) { m_backoffScale = scale; }
    static int backoffDelayMs(int step); // 1000, 5000, 30000, 30000 …
    static constexpr int kToastGapMs = 2000;
    // A final answer's `rerun` (seconds): the same query is sent again then,
    // while it is still the launcher's (no new key, not hidden), its rows
    // merged in place. Within these bounds.
    static constexpr double kRerunMin = 0.5;
    static constexpr double kRerunMax = 60;
    // A resident plugin's winshun/refresh (what the launcher shows changed by
    // itself, GitHub #113): while its `rerun` is armed, the query is sent again
    // now, though not sooner than this after the last one went out (a plugin
    // saying it again and again gets one query, not a storm).
    static constexpr int kRefreshGapMs = 100;

signals:
    // A plugin is answering `queryId` (zero-code: at once, within query()).
    void queryTaken(std::uint64_t queryId, const QString& pluginId);
    // Items for `queryId` from one command. `partial`: more follow (a
    // query/partial); `replace`: these replace what came before for this
    // query, else they are appended.
    void results(std::uint64_t queryId, const QString& pluginId, const QString& commandId, const ws::plugin::Items& items,
        bool partial, bool replace);
    void queryFinished(std::uint64_t queryId, const QString& pluginId); // the final answer came (results() just before it)
    void queryFailed(std::uint64_t queryId, const QString& pluginId, const QString& why); // timed out, crashed, errored
    void progress(std::uint64_t queryId, const QString& pluginId, const QString& text);
    void notice(const QString& pluginId, const QString& text); // winshun/notify: for the footer
    // winshun/notify with "toast" from a plugin with the "notifications"
    // permission: a system notification. At most one per plugin every
    // kToastGapMs (a newer one waiting replaces the one before it); the
    // title begins with the plugin's name ("局域网传输：…"; one the user put
    // in "插件「…」：…", its id when the name shows nothing: ToastPolicy),
    // title and text cut to 63 and 200 characters, without the characters
    // that do not show or turn words around; no text, the title is the text. `click`: the effects of a click, only those
    // that cannot surprise, one of each (toastClick: its own list, a local
    // file or folder, a copy with clipboard:write; empty: nothing to do),
    // still to be held to the plugin's permissions like an action's
    // (OpenPolicy).
    void toastRequested(const QString& pluginId, const QString& title, const QString& text, const ws::plugin::Effects& click);
    // `pluginId`: whose action it was, so the launcher can hold a plugin-supplied
    // `open` effect to that plugin's permissions (OpenPolicy), whatever asked.
    void actionDone(std::uint64_t requestId, const QString& pluginId, const ws::plugin::Effects& effects, bool ok, const QString& error);
    void processStarted(const QString& pluginId, quint32 pid);
    void processExited(const QString& pluginId, quint32 code);
    void restartScheduled(const QString& pluginId, int delayMs); // the real delay
    void disabledAfterCrashes(const QString& pluginId);
    void stateChanged(const QString& pluginId);
    void logged(const QString& line); // what went to plugins.log
    // Developer mode: one line of traffic. `direction`: "→" (to the plugin),
    // "←" (from it), "stderr", or "·" (an event: started, exited, timed
    // out); `summary` is the method or id in a few words, `detail` the whole
    // line (passwords among the settings replaced by dots); `elapsedMs` the
    // time from a request to this, its response, else -1.
    void traced(const QString& pluginId, const QString& direction, const QString& summary, const QString& detail, int elapsedMs);
    void reloaded(const QString& pluginId); // developer mode: its scripts changed and it was restarted

private:
    struct Plugin;
    struct Pending;
    struct OnceRun;
    struct CommandCall;
    enum class RunKind { Query, Action };

    Plugin& plugin(const QString& id);
    const Entry* entry(const QString& id) const;
    bool ensureRunner();
    void onRunnerExited();
    void onStarted(std::uint64_t instance, quint32 pid);
    void onFailed(std::uint64_t instance, const QString& stage, quint32 error);
    void onLine(std::uint64_t instance, const QByteArray& line);
    void onInstanceExited(std::uint64_t instance, quint32 code, const QStringList& stderrTail, bool overlong);

    // Resident plugins.
    void startResident(Plugin& p);
    void sendQuery(Plugin& p, const Pending& q);
    void onQueryReply(const QString& id, std::uint64_t queryId, const jsonrpc::Message& response);
    void onQueryTimeout(const QString& id);
    void stopResident(Plugin& p, bool restartAfter);
    void crashed(Plugin& p, const QString& why);
    void evictIfCrowded(const QString& starting);
    void dropInflight(Plugin& p, bool tellPlugin);
    void stopRerun(Plugin& p);
    void onRefresh(const QString& id);
    void onToolCall(const QString& id, const QJsonValue& requestId, const QString& tool, const QJsonObject& arguments);
    void onToast(const QString& id, const QString& title, const QString& text, const QJsonArray& click);
    void showToast(Plugin& p);
    bool opensOwnList(const QString& id, const QString& query) const; // a toast click's setQuery (ToastPolicy)

    // Once plugins.
    void launchOnce(Plugin& p, const Pending& q);
    void finishOnce(Plugin& p, std::uint64_t instance, quint32 code, const QStringList& stderrTail, bool overlong);

    // Command calls (callCommand).
    void sendCommand(Plugin& p, CommandCall& call);
    void finishCommand(std::uint64_t callId, bool ok, const Items& items, const QString& error);
    void flushCommandCalls(Plugin& p); // the plugin is ready: the calls that waited for it go
    void failCommandCalls(const QString& pluginId, const QString& why); // empty id: every plugin's

    Runner::Spec specFor(Plugin& p, const Entry& e, RunKind kind, const Pending* q, QString* error);
    QString dataDirFor(const Entry& e);
    void log(const QString& line);
    void touch(Plugin& p);
    // Developer mode's trace of a line (see traced()); `line` is a JSON-RPC
    // message or anything else.
    void trace(Plugin& p, const QString& direction, const QByteArray& line);
    void traceEvent(const QString& id, const QString& text);
    QString redact(const QString& id, QString text) const; // passwords among the plugin's settings replaced

    Options m_options;
    Catalog m_catalog;
    ToolBridge m_bridge;
    std::unique_ptr<Runner> m_runner;
    int m_runnerRestarts = 0;
    QTimer m_runnerBackoff;
    std::map<QString, std::unique_ptr<Plugin>> m_plugins;
    std::map<std::uint64_t, QString> m_owners; // instance -> plugin
    QHash<QString, QJsonObject> m_settings;
    std::uint64_t m_nextToolCall = 1;
    std::map<std::uint64_t, std::unique_ptr<CommandCall>> m_commandCalls;
    std::uint64_t m_nextCommandCall = std::uint64_t {1} << 40; // apart from the launcher's query ids
    double m_backoffScale = 1;
    bool m_shuttingDown = false;
    bool m_developerMode = false;
};

} // namespace ws::plugin
