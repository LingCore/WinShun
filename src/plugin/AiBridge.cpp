#include "AiBridge.h"

#include <QJsonArray>

#include <algorithm>
#include <memory>

using namespace Qt::StringLiterals;

namespace ws::plugin {

namespace {

QString sanitized(const QString& text)
{
    QString out;
    for (const QChar c : text) {
        if ((c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9'))
            out += c;
        else if (c >= u'A' && c <= u'Z')
            out += c.toLower();
        else
            out += u'_';
    }
    return out;
}

QJsonObject typed(const char* type, const QString& description)
{
    return {{u"type"_s, QString::fromLatin1(type)}, {u"description"_s, description}};
}

// The item of a plugin as the model sees it: no `data` (opaque, and the
// plugin's own), nothing to act on but the id.
QJsonObject itemJson(const Item& item)
{
    QJsonObject o {{u"id"_s, item.id}, {u"title"_s, item.title}};
    if (!item.subtitle.isEmpty())
        o.insert(u"subtitle"_s, item.subtitle);
    switch (item.open.kind) {
    case Open::Kind::Url:
        o.insert(u"url"_s, item.open.target);
        break;
    case Open::Kind::Path:
        o.insert(u"path"_s, item.open.target);
        break;
    case Open::Kind::None:
        break;
    }
    if (!item.copy.isEmpty())
        o.insert(u"copy"_s, item.copy);
    if (!item.previewText.isEmpty())
        o.insert(u"preview"_s, item.previewText);
    if (!item.valid)
        o.insert(u"informational"_s, true);
    return o;
}

QJsonObject itemsResult(const Items& items)
{
    QJsonArray array;
    for (const Item& item : items)
        array.push_back(itemJson(item));
    return {{u"items"_s, array}, {u"returned"_s, array.size()}};
}

QString describe(const Entry& e, const Command& c)
{
    const QString own = c.ai.value(u"description"_s).toString();
    const QString name = e.manifest->name.pick(u"en"_s);
    if (!own.isEmpty())
        return u"%1 (the '%2' plugin of Win顺)"_s.arg(own, name);
    const QString description = c.description.pick(u"en"_s).isEmpty() ? c.title.pick(u"en"_s) : c.description.pick(u"en"_s);
    return u"%1: the '%2' command of the '%3' plugin of Win顺. Returns the rows the command would show in the launcher."_s.arg(
        description, c.title.pick(u"en"_s), name);
}

QJsonObject queryInput(const Command& c)
{
    const QJsonValue own = c.ai.value(u"parameters"_s);
    if (own.isObject() && own.toObject().value(u"type"_s).toString() == u"object")
        return own.toObject();
    QString hint = c.hint.pick(u"en"_s);
    QJsonObject properties {{u"query"_s, typed("string", hint.isEmpty() ? u"The words typed after the keyword; may be empty"_s : u"The words typed after the keyword: %1"_s.arg(hint))},
        {u"limit"_s, QJsonObject {{u"type"_s, u"integer"_s}, {u"description"_s, u"Rows at most"_s}, {u"minimum"_s, 1}, {u"maximum"_s, 200}, {u"default"_s, 50}}}};
    return {{u"type"_s, u"object"_s}, {u"properties"_s, properties}, {u"additionalProperties"_s, false}};
}

QJsonObject itemsOutput()
{
    QJsonObject item {{u"type"_s, u"object"_s},
        {u"properties"_s,
            QJsonObject {{u"id"_s, typed("string", u"The row's id, for the _run tool"_s)}, {u"title"_s, typed("string", u"The row's title; for a calculation or conversion, the result"_s)},
                {u"subtitle"_s, typed("string", u"Details"_s)}, {u"url"_s, typed("string", u"The address the row opens, if any"_s)},
                {u"path"_s, typed("string", u"The file or program the row opens, if any"_s)}, {u"copy"_s, typed("string", u"What Enter copies, if different from the title"_s)},
                {u"informational"_s, typed("boolean", u"True for a row that is a message, not a result"_s)}}}};
    return {{u"type"_s, u"object"_s}, {u"properties"_s, QJsonObject {{u"items"_s, QJsonObject {{u"type"_s, u"array"_s}, {u"items"_s, item}}}, {u"returned"_s, typed("integer", u"How many"_s)}}}};
}

} // namespace

AiBridge::AiBridge(Host* host, mcp::ToolHost* tools, QObject* parent)
    : QObject(parent)
    , m_host(host)
    , m_tools(tools)
{
    connect(&m_host->catalog(), &Catalog::changed, this, &AiBridge::rebuild);
    connect(m_host, &Host::actionDone, this, &AiBridge::onActionDone);
    rebuild();
}

AiBridge::~AiBridge()
{
    if (m_tools)
        m_tools->setExternalTools({});
}

QString AiBridge::toolName(const QString& pluginId, const QString& commandId, bool run)
{
    return u"plugin_"_s + sanitized(pluginId) + u'_' + sanitized(commandId) + (run ? u"_run"_s : QString());
}

QStringList AiBridge::toolNamesOf(const Manifest& manifest, bool actions)
{
    QStringList names;
    for (const Command& c : manifest.commands) {
        names.push_back(toolName(manifest.id, c.id, false));
        if (actions && !c.zeroCode())
            names.push_back(toolName(manifest.id, c.id, true));
    }
    return names;
}

void AiBridge::setOptions(Options options)
{
    if (m_options == options)
        return;
    m_options = std::move(options);
    rebuild();
}

void AiBridge::rebuild()
{
    std::vector<mcp::ExternalTool> tools;
    if (m_options.enabled) {
        for (const Entry& e : m_host->catalog().entries()) {
            if (!e.manifest || !e.manifest->aiExpose || m_options.excluded.contains(e.id))
                continue;
            const bool offered = m_host->catalog().enabled(e.id);
            // Off, or a newer version waiting for the user's OK on more
            // permissions (Catalog::setConfirmed): the words say which.
            const QStringList awaiting = m_host->catalog().awaitingConsent(e.id);
            const QString name = e.manifest->name.pick(u"en"_s);
            QString offReason;
            if (!offered && awaiting.isEmpty())
                offReason = u"The plugin '%1' is turned off in Win顺's settings (设置 → 插件). Ask the user to turn it on there, then call this again."_s.arg(name);
            else if (!offered)
                offReason = u"The plugin '%1' waits for the user to allow permissions its installed version asks for (%2). Ask the user to allow them in Win顺's settings (设置 → 插件: turn its switch on), then call this again."_s.arg(
                    name, awaiting.join(u", "_s));
            for (const Command& c : e.manifest->commands) {
                tools.push_back(queryTool(e, c, offered, offReason));
                if (m_options.actions && !c.zeroCode())
                    tools.push_back(actionTool(e, c, offered, offReason));
            }
        }
    }
    m_tools->setExternalTools(std::move(tools));
}

mcp::ExternalTool AiBridge::queryTool(const Entry& e, const Command& c, bool offered, const QString& offReason)
{
    mcp::ExternalTool t;
    t.name = toolName(e.id, c.id, false);
    t.title = e.manifest->name.pick(u"en"_s) + u": "_s + c.title.pick(u"en"_s);
    t.description = describe(e, c);
    t.inputSchema = queryInput(c);
    t.outputSchema = itemsOutput();
    t.readOnly = true;
    t.openWorld = e.manifest->permissions.has(Permission::Network);
    t.offered = offered;
    t.offReason = offReason;
    const QString pluginId = e.id;
    const QString commandId = c.id;
    t.run = [this, pluginId, commandId](std::uint64_t callId, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done) {
        const QString query = arguments.value(u"query"_s).toString();
        const int limit = std::clamp(static_cast<int>(arguments.value(u"limit"_s).toDouble(50)), 1, kMaxItems);
        auto answered = std::make_shared<bool>(false); // a zero-code command answers within the call
        const std::uint64_t commandCall = m_host->callCommand(pluginId, commandId, query, limit, [this, callId, done, answered](bool ok, const Items& items, const QString& error) {
            *answered = true;
            m_calls.erase(callId);
            if (!ok)
                done({{}, true, u"The plugin did not answer: %1"_s.arg(error)});
            else
                done({itemsResult(items)});
        });
        if (!*answered)
            m_calls[callId] = commandCall;
    };
    t.cancel = [this](std::uint64_t callId) {
        if (const auto it = m_calls.find(callId); it != m_calls.end()) {
            m_host->cancelCommand(it->second);
            m_calls.erase(it);
        }
    };
    return t;
}

mcp::ExternalTool AiBridge::actionTool(const Entry& e, const Command& c, bool offered, const QString& offReason)
{
    mcp::ExternalTool t;
    t.name = toolName(e.id, c.id, true);
    t.title = e.manifest->name.pick(u"en"_s) + u": "_s + c.title.pick(u"en"_s) + u" (run)"_s;
    QStringList actions;
    for (const Action& a : c.actions)
        actions.push_back(u"'%1' (%2)"_s.arg(a.id, a.title.pick(u"en"_s)));
    t.description = u"Runs an action of the '%1' command of the '%2' plugin of Win顺 on one of its rows: first call %3 with the same query, then give the row's id here.%4"_s.arg(
        c.title.pick(u"en"_s), e.manifest->name.pick(u"en"_s), toolName(e.id, c.id, false),
        actions.isEmpty() ? u" The action is what Enter does in the launcher."_s : u" Actions: %1; the first is what Enter does."_s.arg(actions.join(u", "_s)));
    QJsonObject properties {{u"query"_s, typed("string", u"The same query as for the listing"_s)}, {u"item_id"_s, typed("string", u"The id of the row to act on"_s)},
        {u"action"_s, typed("string", u"The action's id; omitted: the first (Enter)"_s)}};
    t.inputSchema = {{u"type"_s, u"object"_s}, {u"properties"_s, properties}, {u"required"_s, QJsonArray {u"item_id"_s}}, {u"additionalProperties"_s, false}};
    t.outputSchema = {{u"type"_s, u"object"_s},
        {u"properties"_s,
            QJsonObject {{u"done"_s, typed("boolean", u"Whether the plugin did it"_s)}, {u"message"_s, typed("string", u"What the plugin said (its notification), if anything"_s)},
                {u"effects"_s, QJsonObject {{u"type"_s, u"array"_s}, {u"items"_s, typed("string", u"The effects the plugin asked Win顺 for"_s)}}}}}};
    t.readOnly = false;
    t.openWorld = e.manifest->permissions.has(Permission::Network);
    t.offered = offered;
    t.offReason = offReason;
    const QString pluginId = e.id;
    const QString commandId = c.id;
    t.run = [this, pluginId, commandId](std::uint64_t callId, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done) {
        const QString query = arguments.value(u"query"_s).toString();
        const QString itemId = arguments.value(u"item_id"_s).toString();
        QString actionId = arguments.value(u"action"_s).toString();
        if (itemId.isEmpty()) {
            done({{}, true, u"Missing required argument 'item_id'"_s});
            return;
        }
        const Entry* e = m_host->catalog().find(pluginId);
        const Command* command = e && e->manifest ? e->manifest->command(commandId) : nullptr;
        if (!command) {
            done({{}, true, u"No such command"_s});
            return;
        }
        if (actionId.isEmpty())
            actionId = command->actions.isEmpty() ? u"default"_s : command->actions.first().id;
        // The row again, as the plugin gave it (its data included), then the action on it.
        auto answered = std::make_shared<bool>(false);
        const std::uint64_t commandCall = m_host->callCommand(pluginId, commandId, query, kMaxItems,
            [this, callId, done, pluginId, commandId, itemId, actionId, query, answered](bool ok, const Items& items, const QString& error) {
                *answered = true;
                m_calls.erase(callId);
                if (!ok) {
                    done({{}, true, u"The plugin did not answer: %1"_s.arg(error)});
                    return;
                }
                const Item* item = nullptr;
                for (const Item& i : items) {
                    if (i.id == itemId)
                        item = &i;
                }
                if (!item) {
                    done({{}, true, u"No row with id '%1' for this query; list first and use an id from the answer"_s.arg(itemId)});
                    return;
                }
                if (!item->valid) {
                    done({{}, true, u"The row '%1' is a message, not something to act on"_s.arg(item->title)});
                    return;
                }
                // What Win顺 itself would do for the row, done here for the model's benefit only in words.
                if (item->open.kind != Open::Kind::None || !item->setQuery.isEmpty() || !item->searchQuery.isEmpty()) {
                    const bool first = actionId == u"default"_s || (item->actions.isEmpty() ? false : item->actions.first().id == actionId);
                    if (first) {
                        done({{}, true, u"That row's Enter opens '%1' in the launcher rather than asking the plugin; use open_item for a path, or give the user the address."_s.arg(
                                 item->open.target.isEmpty() ? item->setQuery + item->searchQuery : item->open.target)});
                        return;
                    }
                }
                const std::uint64_t requestId = m_nextAction++;
                m_actions[requestId] = {done, pluginId};
                m_host->action(requestId, pluginId, commandId, actionId, *item, query);
            });
        if (!*answered)
            m_calls[callId] = commandCall;
    };
    t.cancel = [this](std::uint64_t callId) {
        if (const auto it = m_calls.find(callId); it != m_calls.end()) {
            m_host->cancelCommand(it->second);
            m_calls.erase(it);
        }
    };
    return t;
}

void AiBridge::onActionDone(std::uint64_t requestId, const QString& pluginId, const Effects& effects, bool ok, const QString& error)
{
    Q_UNUSED(pluginId) // AiBridge keys by its own request map; attribution is for the launcher
    const auto it = m_actions.find(requestId);
    if (it == m_actions.end())
        return;
    const ActionWait wait = std::move(it->second);
    m_actions.erase(it);
    if (!ok) {
        wait.done({{}, true, u"The plugin did not do it: %1"_s.arg(error)});
        return;
    }
    QJsonArray kinds;
    QString message;
    for (const Effect& e : effects) {
        switch (e.kind) {
        case Effect::Kind::Hide: kinds.push_back(u"hide"_s); break;
        case Effect::Kind::Keep: kinds.push_back(u"keep"_s); break;
        case Effect::Kind::SetQuery: kinds.push_back(u"setQuery: "_s + e.text); break;
        case Effect::Kind::Notify:
            kinds.push_back(u"notify: "_s + e.text);
            message = e.text;
            break;
        case Effect::Kind::Copy: kinds.push_back(u"copy: "_s + e.text); break;
        case Effect::Kind::Open: kinds.push_back(u"open: "_s + e.open.target); break;
        case Effect::Kind::Replace: kinds.push_back(u"replace: %1 rows"_s.arg(e.items.size())); break;
        }
    }
    QJsonObject out {{u"done"_s, true}, {u"effects"_s, kinds}};
    if (!message.isEmpty())
        out.insert(u"message"_s, message);
    wait.done({out});
}

} // namespace ws::plugin
