#include "PluginCommands.h"

#include "Catalog.h"
#include "Host.h"
#include "Install.h"
#include "PluginUpdates.h"
#include "Updates.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QPointer>
#include <QScopeGuard>
#include <QThreadPool>

#include <algorithm>
#include <optional>

using namespace Qt::StringLiterals;

namespace ws {

namespace gallery = plugin::gallery;
namespace install = plugin::install;

namespace {

constexpr int kLeaveMs = 2500; // how long a plugin's program gets to leave before its files are replaced, as on the page

// The answers. A failure is an answer too (never ToolResult::isError, whose
// words are for a model): ws prints the message and picks its exit code by
// `code` ("not_found": no such plugin, file or folder).
mcp::ToolResult finished(const QString& message, QJsonObject more = {})
{
    more.insert(u"state"_s, u"done"_s);
    if (!message.isEmpty())
        more.insert(u"message"_s, message);
    return {more};
}

mcp::ToolResult asking(const QString& message, const QJsonObject& plugin)
{
    return {{{u"state"_s, u"asking"_s}, {u"message"_s, message}, {u"plugin"_s, plugin}}};
}

mcp::ToolResult failed(const QString& message, const QString& code = {})
{
    QJsonObject answer {{u"state"_s, u"failed"_s}, {u"message"_s, message}};
    if (!code.isEmpty())
        answer.insert(u"code"_s, code);
    return {answer};
}

QString statusName(plugin::Host::State::Status status)
{
    using Status = plugin::Host::State::Status;
    switch (status) {
    case Status::Off: return u"off"_s;
    case Status::Ready: return u"ready"_s;
    case Status::Starting: return u"starting"_s;
    case Status::Running: return u"running"_s;
    case Status::Backoff: return u"restarting"_s;
    case Status::Crashed: return u"crashed"_s;
    case Status::Unusable: return u"unusable"_s;
    }
    return u"off"_s;
}

QString reviewName(gallery::Review review)
{
    switch (review) {
    case gallery::Review::Official: return u"official"_s;
    case gallery::Review::Reviewed: return u"reviewed"_s;
    case gallery::Review::UnreviewedNative: return u"unreviewed-native"_s;
    case gallery::Review::Unspecified: break;
    }
    return u"unspecified"_s;
}

QString stateName(gallery::State state)
{
    switch (state) {
    case gallery::State::Install: return u"install"_s;
    case gallery::State::Update: return u"update"_s;
    case gallery::State::Installed: return u"installed"_s;
    case gallery::State::Bundled: return u"bundled"_s;
    case gallery::State::NeedsNewerApp: return u"needs-newer-winshun"_s;
    }
    return u"install"_s;
}

const PluginConfig* configOf(const QList<PluginConfig>& configs, const QString& id)
{
    const auto it = std::ranges::find_if(configs, [&](const PluginConfig& c) { return c.id == id; });
    return it == configs.end() ? nullptr : &*it;
}

// The library's list as fetched for a search, or the copy kept.
struct Listed {
    std::optional<gallery::Index> index;
    QDateTime keptAt; // valid: the kept copy, from then
};

Listed fetchList(const QString& source, const gallery::Fetcher& web, const QString& kept)
{
    Listed listed;
    const gallery::Fetched fetched = gallery::fetch(source, gallery::kMaxIndexBytes + 1, web, {},
        [&](const QByteArray& bytes) { return bytes.size() <= gallery::kMaxIndexBytes && gallery::parse(bytes, source, nullptr).has_value(); });
    if (fetched.status == 200 && fetched.body.size() <= gallery::kMaxIndexBytes)
        listed.index = gallery::parse(fetched.body, source, nullptr);
    if (listed.index || kept.isEmpty())
        return listed;
    QFile file(kept);
    if (file.open(QIODevice::ReadOnly)) {
        listed.index = gallery::parse(file.read(gallery::kMaxIndexBytes), source, nullptr);
        if (listed.index)
            listed.keptAt = QFileInfo(file).lastModified();
    }
    return listed;
}

} // namespace

PluginCommands::PluginCommands(Environment environment, QObject* parent)
    : QObject(parent)
    , m_env(std::move(environment))
{
    Q_ASSERT(m_env.host && m_env.locale && m_env.configs && m_env.setSwitch);
}

void PluginCommands::run(const QString& tool, const QJsonObject& arguments, Done done)
{
    if (tool == u"plugins_list")
        return list(std::move(done));
    if (tool == u"plugins_search")
        return search(arguments, std::move(done));
    if (tool == u"plugins_install")
        return install(arguments, std::move(done));
    if (tool == u"plugins_switch")
        return setSwitch(arguments, std::move(done));
    if (tool == u"plugins_remove")
        return remove(arguments, std::move(done));
    done(failed(tr("这个版本的 Win顺 不认这个命令，请更新 Win顺")));
}

QString PluginCommands::nameOf(const plugin::Entry& entry) const
{
    const QString name = entry.manifest ? entry.manifest->name.pick(m_env.locale()) : QString();
    return name.isEmpty() ? entry.id : name;
}

bool PluginCommands::needsConsent(const plugin::Entry& entry, const QList<PluginConfig>& configs) const
{
    if (!entry.manifest)
        return false;
    const PluginConfig* config = configOf(configs, entry.id);
    const QStringList confirmed = config ? config->confirmed : QStringList();
    return confirmed.isEmpty() || !install::unconfirmed(*entry.manifest, confirmed).isEmpty()
        || (entry.manifest->permissions.has(plugin::Permission::Elevated) && m_env.host->hostElevated());
}

QJsonObject PluginCommands::describe(const plugin::Entry& entry, const QList<PluginConfig>& configs) const
{
    const PluginConfig* config = configOf(configs, entry.id);
    const bool on = config && config->enabled;
    const QStringList waitingFor = on ? m_env.host->catalog().awaitingConsent(entry.id) : QStringList();
    const plugin::Host::State state = m_env.host->state(entry.id);
    QJsonObject o {{u"id"_s, entry.id},
        {u"name"_s, nameOf(entry)},
        {u"source"_s, entry.source == plugin::Entry::Source::Bundled ? u"bundled"_s : u"user"_s},
        {u"enabled"_s, on},
        // on | off | waiting (on, but its newer version waits for the user's yes) | unusable (its plugin.json)
        {u"state"_s, !entry.usable() ? u"unusable"_s : !waitingFor.isEmpty() ? u"waiting"_s : on ? u"on"_s : u"off"_s},
        {u"status"_s, statusName(state.status)}, // of its program
        {u"folder"_s, QDir::toNativeSeparators(entry.dir)}};
    if (entry.manifest) {
        o.insert(u"version"_s, entry.manifest->version);
        o.insert(u"description"_s, entry.manifest->description.pick(m_env.locale()));
        o.insert(u"permissions"_s, QJsonArray::fromStringList(entry.manifest->permissions.names()));
    }
    if (!waitingFor.isEmpty())
        o.insert(u"waiting_for"_s, QJsonArray::fromStringList(waitingFor));
    if (const QString error = entry.usable() ? state.error : entry.error; !error.isEmpty())
        o.insert(u"error"_s, error);
    return o;
}

QJsonObject PluginCommands::describe(const QString& id) const
{
    const plugin::Entry* entry = m_env.host->catalog().find(id);
    return entry ? describe(*entry, m_env.configs()) : QJsonObject();
}

void PluginCommands::list(Done done) const
{
    const QList<PluginConfig> configs = m_env.configs();
    QJsonArray plugins;
    for (const plugin::Entry& entry : m_env.host->catalog().entries())
        plugins.push_back(describe(entry, configs));
    done(finished({}, {{u"plugins"_s, plugins}}));
}

void PluginCommands::search(const QJsonObject& arguments, Done done)
{
    const QString filter = arguments.value(u"query"_s).toString().trimmed();
    QThreadPool::globalInstance()->start(
        [self = QPointer(this), filter, source = m_env.librarySource, web = m_env.web, kept = m_env.keptList, done = std::move(done)]() mutable {
            Listed listed = fetchList(source, web, kept);
            QMetaObject::invokeMethod(qApp, [self, filter, listed = std::move(listed), done = std::move(done)] {
                if (!self)
                    return; // Win顺 is leaving
                if (!listed.index)
                    return done(failed(tr("拿不到插件库的列表：连不上 GitHub。检查网络，或在 Win顺 的设置里搜“网络代理”，给它配一个")));
                plugin::Host* host = self->m_env.host;
                const QString locale = self->m_env.locale();
                QJsonArray plugins;
                for (const gallery::Item& item : listed.index->items) {
                    const QString words = QStringList {item.id, item.name.zh, item.name.en, item.description.zh, item.description.en,
                        item.keywords.join(u' '), item.author}.join(u'\n').toLower();
                    if (!gallery::fitsFilter(words, filter))
                        continue;
                    const gallery::Local local = gallery::localOf(host->catalog(), host->options().bundledPluginDir, item.id);
                    QJsonObject o {{u"id"_s, item.id}, {u"name"_s, item.name.pick(locale)}, {u"description"_s, item.description.pick(locale)},
                        {u"version"_s, item.version}, {u"author"_s, item.author}, {u"review"_s, reviewName(item.review)},
                        {u"permissions"_s, QJsonArray::fromStringList(item.permissions)},
                        // install | update | installed | bundled | needs-newer-winshun
                        {u"state"_s, stateName(gallery::stateOf(item, local, host->options().version))}};
                    if (!local.userVersion.isEmpty())
                        o.insert(u"installed"_s, local.userVersion);
                    if (!item.minWinShun.isEmpty())
                        o.insert(u"min_winshun"_s, item.minWinShun);
                    plugins.push_back(o);
                }
                const QLocale shown(locale.startsWith(u"en"_s) ? QLocale::English : QLocale::Chinese);
                done(finished(listed.keptAt.isValid()
                        ? tr("连不上插件库，这是 %1 存下的列表").arg(shown.toString(listed.keptAt, QLocale::ShortFormat))
                        : QString(),
                    {{u"plugins"_s, plugins}}));
            }, Qt::QueuedConnection);
        });
}

void PluginCommands::install(const QJsonObject& arguments, Done done)
{
    const QString path = arguments.value(u"path"_s).toString();
    const QString id = arguments.value(u"id"_s).toString().trimmed();
    if (!path.isEmpty())
        return installPath(path, std::move(done));
    if (!id.isEmpty())
        return installListed(id, std::move(done));
    done(failed(tr("要装哪个插件：给一个插件包、插件文件夹，或插件库里的 id")));
}

// A package or a folder of the user's own (one they made, or downloaded):
// in place at once, as the page's 从压缩包安装 puts it, minus the question
// about the version already there, which running the command answered.
void PluginCommands::installPath(const QString& path, Done done)
{
    plugin::Host& host = *m_env.host;
    const QString userDir = host.options().userPluginDir;
    std::optional<install::Candidate> candidate = install::inspect(path);
    if (!candidate)
        return done(failed(tr("找不到 %1").arg(QDir::toNativeSeparators(path)), u"not_found"_s));
    const auto discard = qScopeGuard([&] { install::discard(*candidate); }); // nothing once it was moved in
    if (!candidate->manifest)
        return done(failed(tr("不能安装 %1：%2").arg(QFileInfo(path).fileName(), candidate->error)));
    const QString id = candidate->manifest->id;
    const QString name = candidate->manifest->name.pick(m_env.locale());
    // The user's own files under that name are not put aside unasked (GitHub
    // #253): the page asks, and renames them.
    if (install::obstacle(userDir, id) == install::Obstacle::Foreign) {
        return done(failed(tr("没有安装 %1：%2 已经是一个文件夹，但不是装好的插件。在 Win顺 的 设置 → 插件 里从压缩包安装，会先问你怎么处理它")
                               .arg(name, QDir::toNativeSeparators(QDir(userDir).filePath(id)))));
    }
    if (m_env.updates && !m_env.updates->claim(id))
        return done(failed(tr("Win顺 正在更新这个插件，稍后再试")));
    const auto release = qScopeGuard([this, id] {
        if (m_env.updates)
            m_env.updates->release(id);
    });
    const plugin::Entry* before = host.catalog().find(id);
    const QString replaced = before && before->source == plugin::Entry::Source::User && before->manifest ? before->manifest->version : QString();
    // Runs events while the plugin's program leaves: nothing of the catalog is held across it.
    const QString error = plugin::updates::replace(host, *candidate, userDir, kLeaveMs);
    if (!error.isEmpty())
        return done(failed(tr("没有安装 %1：%2").arg(name, error)));
    host.catalog().scan();
    if (m_env.updates)
        m_env.updates->forget(id); // what was kept for the version before
    host.preload(); // one that starts with Win顺 starts again now

    const QJsonObject plugin = describe(id);
    done(finished(installedWords(plugin, replaced), {{u"plugin"_s, plugin}}));
}

// What is said of a plugin just put in place (`plugin`: describe()'s), and
// what to do next: `replaced` is the version it was put over, if any.
QString PluginCommands::installedWords(const QJsonObject& plugin, const QString& replaced) const
{
    const QString id = plugin.value(u"id"_s).toString();
    const QString name = plugin.value(u"name"_s).toString();
    const QString version = plugin.value(u"version"_s).toString();
    const QString what = replaced.isEmpty() || replaced == version ? tr("已安装 %1 %2").arg(name, version)
                                                                    : tr("已安装 %1 %2（原来是 %3）").arg(name, version, replaced);
    const QString state = plugin.value(u"state"_s).toString();
    if (state == u"waiting")
        return what + tr("。新版本要更多权限，先停着；请用户同意：ws plugin enable %1").arg(id);
    if (state == u"on")
        return what + tr("，开着，已经换上");
    if (state == u"off")
        return what + tr("，还关着；打开它：ws plugin enable %1").arg(id);
    return what;
}

void PluginCommands::installListed(const QString& id, Done done)
{
    if (!plugin::validPluginId(id))
        return done(failed(tr("找不到 %1：它不是文件或文件夹，也不是插件的 id").arg(id), u"not_found"_s));
    if (!m_env.installFromLibrary)
        return done(failed(tr("这里装不了插件库里的插件")));
    // The user's copy as it is now: the page says "installed" of one that was up to date already.
    const QString before = describe(id).value(u"source"_s).toString() == u"user" ? describe(id).value(u"version"_s).toString() : QString();
    m_env.installFromLibrary(id, [self = QPointer(this), id, before, done = std::move(done)](const PluginInstalled& installed) {
        if (!self)
            return;
        const QJsonObject plugin = self->describe(id);
        switch (installed.step) {
        case PluginInstalled::Step::Failed:
            return done(failed(installed.message));
        case PluginInstalled::Step::Asking:
            return done(asking(tr("Win顺 正在问用户要不要装 %1 %2；用户在 Win顺 的窗口里回答之后才会装").arg(id, installed.version), plugin));
        case PluginInstalled::Step::Done:
            break;
        }
        if (!before.isEmpty() && before == plugin.value(u"version"_s).toString()) { // nothing to do: the page's words for it
            const QString next = plugin.value(u"state"_s).toString() == u"off" ? tr("。打开它：ws plugin enable %1").arg(id) : QString();
            return done(finished(installed.message + next, {{u"plugin"_s, plugin}}));
        }
        done(finished(self->installedWords(plugin, before), {{u"plugin"_s, plugin}}));
    });
}

void PluginCommands::setSwitch(const QJsonObject& arguments, Done done)
{
    const QString id = arguments.value(u"id"_s).toString().trimmed();
    const bool on = arguments.value(u"on"_s).toBool();
    const plugin::Entry* entry = m_env.host->catalog().find(id);
    if (!entry)
        return done(failed(tr("没有装 %1；ws plugin list 列出装了哪些").arg(id), u"not_found"_s));
    const QString name = nameOf(*entry);
    const QList<PluginConfig> configs = m_env.configs();
    const PluginConfig* config = configOf(configs, id);
    const bool was = config && config->enabled;
    if (!on) {
        if (was)
            m_env.setSwitch(id, false);
        return done(finished(was ? tr("已关闭 %1").arg(name) : tr("%1 本来就关着").arg(name), {{u"plugin"_s, describe(id)}}));
    }
    if (!entry->usable())
        return done(failed(tr("%1 用不了：%2").arg(name, entry->error)));
    if (was && m_env.host->catalog().awaitingConsent(id).isEmpty())
        return done(finished(tr("%1 本来就开着").arg(name), {{u"plugin"_s, describe(id)}}));
    if (!needsConsent(*entry, configs)) {
        m_env.setSwitch(id, true);
        return done(finished(tr("已打开 %1").arg(name), {{u"plugin"_s, describe(id)}}));
    }
    // The user's to answer: the page's own switch, with its dialog. (`entry`
    // is not used past this: the page may read the catalog again.)
    if (!m_env.askEnable || !m_env.askEnable(id))
        return done(failed(tr("打不开 Win顺 的设置窗口，没法请用户确认 %1 的权限").arg(name)));
    const QJsonObject plugin = describe(id);
    if (plugin.value(u"state"_s).toString() == u"on") // nothing was asked (its copy to run as administrator is being made)
        return done(finished(tr("已打开 %1").arg(name), {{u"plugin"_s, plugin}}));
    done(asking(tr("Win顺 正在请用户确认 %1 要的权限；用户在 Win顺 的窗口里同意之后它才打开").arg(name), plugin));
}

void PluginCommands::remove(const QJsonObject& arguments, Done done)
{
    const QString id = arguments.value(u"id"_s).toString().trimmed();
    const plugin::Entry* entry = m_env.host->catalog().find(id);
    if (!entry)
        return done(failed(tr("没有装 %1；ws plugin list 列出装了哪些").arg(id), u"not_found"_s));
    const QString name = nameOf(*entry);
    if (entry->source == plugin::Entry::Source::Bundled)
        return done(failed(tr("%1 随 Win顺 自带，不能卸载；不想用就关掉：ws plugin disable %2").arg(name, id)));
    const QJsonObject plugin = describe(id);
    if (!m_env.askRemove || !m_env.askRemove(id))
        return done(failed(tr("打不开 Win顺 的设置窗口，没法请用户确认卸载 %1").arg(name)));
    done(asking(tr("Win顺 正在问用户要不要卸载 %1；用户在 Win顺 的窗口里确认之后才卸载").arg(name), plugin));
}

} // namespace ws
