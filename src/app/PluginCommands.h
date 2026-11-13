#pragma once

// ws plugin (GitHub #251): the plugins from the command line, for a person
// in a terminal and for an AI agent with a shell (ToolHost's kPluginTools,
// which only ws is offered).
//
// What is done at once is what any program of the user's could do to the
// plugins folder and the settings file anyway: a package or a folder put in
// place (off until it is turned on, as from the page; over a version that is
// on, it is on), a plugin turned off, one turned on whose permissions the
// user has agreed to. What only the user may answer stays theirs, asked on
// the 插件 page in front of them as if they had clicked there: the
// permissions of a plugin turned on for the first time or asking for more, a
// package of the library (its download shows there, and a package nobody
// reviewed is asked about), a removal. The answer then is "asking": the
// command line does not wait for a person.
//
// Every answer is {state: done | asking | failed, message, …}; the message is
// for whoever reads the terminal, in Win顺's language.

#include "McpTools.h"
#include "Gallery.h"
#include "PluginInstalled.h"
#include "Settings.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

namespace ws {

namespace plugin {
class Host;
struct Entry;
}
class PluginUpdates;

class PluginCommands : public QObject {
    Q_OBJECT

public:
    struct Environment {
        plugin::Host* host = nullptr; // required; outlives this
        PluginUpdates* updates = nullptr; // the background updates, kept off a plugin being changed here; may be null
        std::function<QString()> locale; // "zh" | "en": the names' and the words'
        std::function<QList<PluginConfig>()> configs; // the settings' [Plugin.<id>] sections
        // A plugin's switch, saved and applied. On only for one that needs no
        // answer from the user (needsConsent() says).
        std::function<void(const QString& id, bool on)> setSwitch;
        // The 插件 page in front of the user, its switch for `id` turned on
        // there: the permission dialog comes up. False: no window to ask in.
        std::function<bool(const QString& id)> askEnable;
        std::function<bool(const QString& id)> askRemove; // likewise the page's 卸载 and its question
        // The library's 安装 / 更新到 for `id`, on the page (PluginGallery::installFor).
        std::function<void(const QString& id, std::function<void(PluginInstalled)> done)> installFromLibrary;
        // For the search: the library's list (its address, or a file), how to
        // fetch it, and the copy the page keeps for when it cannot be.
        QString librarySource;
        plugin::gallery::Fetcher web;
        QString keptList;
    };

    explicit PluginCommands(Environment environment, QObject* parent = nullptr);

    using Done = std::function<void(mcp::ToolResult)>;
    // `tool`: one of mcp::kPluginTools. `done` once, now or later, on the GUI thread.
    void run(const QString& tool, const QJsonObject& arguments, Done done);

    // Whether turning `entry` on is the user's to answer: never agreed to, a
    // version asking for more than was agreed to, or (in an elevated Win顺)
    // one that is to run as administrator, whose copy is confirmed on the page.
    bool needsConsent(const plugin::Entry& entry, const QList<PluginConfig>& configs) const;

private:
    void list(Done done) const;
    void search(const QJsonObject& arguments, Done done);
    void install(const QJsonObject& arguments, Done done);
    void installPath(const QString& path, Done done);
    void installListed(const QString& id, Done done);
    void setSwitch(const QJsonObject& arguments, Done done);
    void remove(const QJsonObject& arguments, Done done);

    QJsonObject describe(const plugin::Entry& entry, const QList<PluginConfig>& configs) const;
    QJsonObject describe(const QString& id) const; // empty: not installed
    QString nameOf(const plugin::Entry& entry) const;
    QString installedWords(const QJsonObject& plugin, const QString& replaced) const;

    Environment m_env;
};

} // namespace ws
