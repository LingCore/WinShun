#include "PluginSettings.h"

#include "AiBridge.h"
#include "Catalog.h"
#include "FileIconProvider.h"
#include "Host.h"
#include "Secrets.h"
#include "Settings.h"
#include "SettingsEditor.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QStyleHints>
#include <QUrl>

#include <windows.h>
#include <psapi.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr auto kConfirmedNone = u"none"; // Confirmed= of a plugin that asks for no permission

bool darkMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

QString memoryText(quint32 pid)
{
    if (!pid)
        return {};
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    PROCESS_MEMORY_COUNTERS counters {sizeof(counters)};
    QString text;
    if (::GetProcessMemoryInfo(process, &counters, sizeof(counters)))
        text = u"%1 MB"_s.arg(QString::number(static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0), 'f', 0));
    ::CloseHandle(process);
    return text;
}

// A choice's options: strings, or {value, title}.
void choiceOptions(const plugin::Setting& s, const QString& locale, QStringList* values, QStringList* titles)
{
    for (const QJsonValue& o : s.options) {
        if (o.isObject()) {
            const QJsonObject object = o.toObject();
            const QString value = object.value(u"value"_s).toVariant().toString();
            const QJsonValue title = object.value(u"title"_s);
            QString text;
            if (title.isObject())
                text = plugin::Text {title.toObject().value(u"zh"_s).toString(), title.toObject().value(u"en"_s).toString()}.pick(locale);
            else
                text = title.toVariant().toString();
            values->push_back(value);
            titles->push_back(text.isEmpty() ? value : text);
        } else {
            const QString value = o.toVariant().toString();
            values->push_back(value);
            titles->push_back(value);
        }
    }
}

QString sanitizedName(const QString& text)
{
    QString out;
    for (const QChar c : text.toLower()) {
        if ((c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') || c == u'-' || c == u'_')
            out += c;
    }
    return out;
}

} // namespace

// ---- the cards ----------------------------------------------------------------

int PluginListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_cards.size());
}

QVariant PluginListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_cards.size()))
        return {};
    const Card& c = m_cards[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole: return c.id;
    case NameRole: return c.name;
    case VersionRole: return c.version;
    case AuthorRole: return c.author;
    case HomepageRole: return c.homepage;
    case DescriptionRole: return c.description;
    case BundledRole: return c.bundled;
    case IconRole: return c.icon;
    case EnabledRole: return c.enabled;
    case UsableRole: return c.usable;
    case StatusRole: return c.status;
    case ToneRole: return c.tone;
    case ErrorRole: return c.error;
    case ResidentRole: return c.resident;
    case KeywordsRole: return c.keywords;
    case PermissionsRole: return c.permissions;
    case SettingsRole: return c.settings;
    case HasReadmeRole: return c.hasReadme;
    case HasDataRole: return c.hasData;
    case AiExposeRole: return c.aiExpose;
    case AiEnabledRole: return c.aiEnabled;
    case AiNoteRole: return c.aiNote;
    }
    return {};
}

QHash<int, QByteArray> PluginListModel::roleNames() const
{
    return {{IdRole, "pluginId"}, {NameRole, "name"}, {VersionRole, "version"}, {AuthorRole, "author"}, {HomepageRole, "homepage"},
        {DescriptionRole, "description"}, {BundledRole, "bundled"}, {IconRole, "icon"}, {EnabledRole, "pluginEnabled"}, {UsableRole, "usable"},
        {StatusRole, "status"}, {ToneRole, "tone"}, {ErrorRole, "error"}, {ResidentRole, "resident"}, {KeywordsRole, "keywords"},
        {PermissionsRole, "permissions"}, {SettingsRole, "settings"}, {HasReadmeRole, "hasReadme"}, {HasDataRole, "hasData"},
        {AiExposeRole, "aiExpose"}, {AiEnabledRole, "aiEnabled"}, {AiNoteRole, "aiNote"}};
}

bool PluginListModel::setCards(std::vector<Card> cards)
{
    bool sameIds = cards.size() == m_cards.size();
    for (std::size_t i = 0; sameIds && i < cards.size(); ++i)
        sameIds = cards[i].id == m_cards[i].id;
    if (!sameIds) {
        beginResetModel();
        m_cards = std::move(cards);
        endResetModel();
        return true;
    }
    for (std::size_t i = 0; i < cards.size(); ++i) {
        Card& old = m_cards[i];
        Card& fresh = cards[i];
        if (old == fresh)
            continue;
        // What the state line says changes often (memory, timeouts); the
        // rest only when the user or the manifest changed something.
        Card compared = fresh;
        compared.status = old.status;
        compared.tone = old.tone;
        compared.error = old.error;
        const bool stateOnly = compared == old;
        old = std::move(fresh);
        const QModelIndex at = index(static_cast<int>(i));
        if (stateOnly)
            emit dataChanged(at, at, {StatusRole, ToneRole, ErrorRole});
        else
            emit dataChanged(at, at);
    }
    return false;
}

const PluginListModel::Card* PluginListModel::card(const QString& id) const
{
    for (const Card& c : m_cards) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

// ---- the log ------------------------------------------------------------------

int PluginLogModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_shown.size());
}

QVariant PluginLogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_shown.size()))
        return {};
    const Line& l = m_all[static_cast<std::size_t>(m_shown[static_cast<std::size_t>(index.row())])];
    switch (role) {
    case TimeRole: return l.time;
    case PluginRole: return l.plugin;
    case DirectionRole: return l.direction;
    case SummaryRole: return l.summary;
    case DetailRole: return l.detail;
    case ElapsedRole: return l.elapsedMs;
    }
    return {};
}

QHash<int, QByteArray> PluginLogModel::roleNames() const
{
    return {{TimeRole, "time"}, {PluginRole, "plugin"}, {DirectionRole, "direction"}, {SummaryRole, "summary"}, {DetailRole, "detail"},
        {ElapsedRole, "elapsed"}};
}

void PluginLogModel::append(Line line)
{
    if (m_all.size() >= static_cast<std::size_t>(kMaxLines)) {
        // The oldest goes: with it the first shown row, if it was shown.
        const bool wasShown = !m_shown.empty() && m_shown.front() == 0;
        if (wasShown)
            beginRemoveRows({}, 0, 0);
        m_all.erase(m_all.begin());
        if (wasShown)
            m_shown.erase(m_shown.begin());
        for (int& i : m_shown)
            --i;
        if (wasShown)
            endRemoveRows();
    }
    m_all.push_back(std::move(line));
    if (shown(m_all.back())) {
        const int row = static_cast<int>(m_shown.size());
        beginInsertRows({}, row, row);
        m_shown.push_back(static_cast<int>(m_all.size()) - 1);
        endInsertRows();
    }
}

void PluginLogModel::clear()
{
    beginResetModel();
    m_all.clear();
    m_shown.clear();
    endResetModel();
}

void PluginLogModel::setFilter(const QString& pluginId)
{
    if (m_filter == pluginId)
        return;
    m_filter = pluginId;
    rebuild();
}

void PluginLogModel::rebuild()
{
    beginResetModel();
    m_shown.clear();
    for (std::size_t i = 0; i < m_all.size(); ++i) {
        if (shown(m_all[i]))
            m_shown.push_back(static_cast<int>(i));
    }
    endResetModel();
}

QStringList PluginLogModel::plugins() const
{
    QStringList ids;
    for (const Line& l : m_all) {
        if (!ids.contains(l.plugin))
            ids.push_back(l.plugin);
    }
    ids.sort();
    return ids;
}

// ---- the page's view-model ------------------------------------------------------

struct PluginSettings::Preview {
    std::vector<PluginListModel::Card> cards;
};

PluginSettings::PluginSettings(SettingsEditor* editor, QObject* parent)
    : QObject(parent)
    , m_editor(editor)
{
    m_memoryTimer.setInterval(2000);
    connect(&m_memoryTimer, &QTimer::timeout, this, &PluginSettings::readMemory);
    connect(editor, &SettingsEditor::changed, this, [this] {
        if (m_host)
            rebuild();
    });
}

PluginSettings::~PluginSettings()
{
    if (m_pendingInstall)
        plugin::install::discard(*m_pendingInstall);
}

void PluginSettings::setHost(plugin::Host* host)
{
    if (m_host)
        disconnect(m_host, nullptr, this, nullptr);
    m_host = host;
    if (!host) {
        rebuild();
        return;
    }
    m_userDir = QDir::toNativeSeparators(host->options().userPluginDir);
    m_dataRoot = host->options().dataRoot.isEmpty() ? m_userDir : QDir::toNativeSeparators(host->options().dataRoot);
    m_logPath = host->options().logPath;
    connect(host, &plugin::Host::stateChanged, this, [this](const QString&) { rebuild(); });
    connect(&host->catalog(), &plugin::Catalog::changed, this, &PluginSettings::rebuild);
    connect(host, &plugin::Host::traced, this,
        [this](const QString& pluginId, const QString& direction, const QString& summary, const QString& detail, int elapsedMs) {
            const bool known = m_log.plugins().contains(pluginId);
            m_log.append({QDateTime::currentDateTime().toString(u"HH:mm:ss.zzz"_s), pluginId, direction, summary, detail, elapsedMs});
            if (!known)
                emit logPluginsChanged();
        });
    rebuild();
}

void PluginSettings::setLocale(const QString& locale)
{
    m_locale = locale;
    rebuild();
}

QString PluginSettings::pluginName(const QString& id) const
{
    if (m_host) {
        if (const plugin::Entry* e = m_host->catalog().find(id); e && e->manifest)
            return e->manifest->name.pick(m_locale);
    }
    if (const PluginListModel::Card* c = m_list.card(id))
        return c->name;
    return id;
}

QVariantList PluginSettings::logPlugins() const
{
    QVariantList out;
    for (const QString& id : m_log.plugins())
        out.push_back(QVariantMap {{u"id"_s, id}, {u"name"_s, pluginName(id)}});
    return out;
}

void PluginSettings::setLogFilter(const QString& id)
{
    if (m_log.filter() == id)
        return;
    m_log.setFilter(id);
    emit logFilterChanged();
}

bool PluginSettings::developerMode() const
{
    return m_editor->m_settings.pluginDeveloperMode;
}

void PluginSettings::setDeveloperMode(bool on)
{
    if (m_editor->m_settings.pluginDeveloperMode == on)
        return;
    m_editor->m_settings.pluginDeveloperMode = on;
    m_editor->commit();
    emit developerModeChanged();
}

void PluginSettings::setActive(bool on)
{
    if (m_active == on)
        return;
    m_active = on;
    if (on) {
        readMemory();
        m_memoryTimer.start();
    } else
        m_memoryTimer.stop();
    emit activeChanged();
}

void PluginSettings::setRecordingHotkey(bool on)
{
    if (m_recordingHotkey == on)
        return;
    m_recordingHotkey = on;
    emit recordingHotkeyChanged();
}

QString PluginSettings::templateAuthor() const
{
    QString author = sanitizedName(qEnvironmentVariable("USERNAME"));
    if (author.isEmpty())
        author = u"me"_s;
    return author;
}

void PluginSettings::setMessage(const QString& message)
{
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

void PluginSettings::clearMessage()
{
    setMessage({});
}

QString PluginSettings::permissionsNote() const
{
    return tr("这些由 Windows 强制：不是管理员、内存上限、子进程、剪贴板、窗口。这些只是插件自己的说明，Win顺 挡不住：联网、开端口、读哪些文件、写哪些文件（严格沙箱的插件除外）。只安装你信得过来源的插件。");
}

// What each permission means and whether Windows holds the plugin to it
// (the plugin design, §3.3). The first row is what every plugin gets.
QVariantList PluginSettings::permissionRows(const QString& id) const
{
    QVariantList rows;
    const auto row = [&](const QString& name, const QString& text, bool enforced, const QString& note = {}) {
        rows.push_back(QVariantMap {{u"name"_s, name}, {u"text"_s, text}, {u"enforced"_s, enforced}, {u"note"_s, note}});
    };
    const plugin::Entry* e = m_host ? m_host->catalog().find(id) : nullptr;
    if (!e || !e->manifest)
        return rows;
    const plugin::Manifest& m = *e->manifest;
    const bool strict = m.runtime && m.runtime->sandbox == plugin::SandboxLevel::Strict;
    // What every plugin gets, as it is: a zero-code one runs no program of
    // its own (Win顺 does what its manifest says, as the user); a strict
    // one runs at low integrity (runner: restrictedToken(LOW)), which
    // cannot write the user's files, only its own data folder.
    if (!m.runtime)
        row(QString(), tr("不运行自己的程序：Win顺 按它的清单打开网址、程序或文件夹，用你的普通权限，不是管理员"), true);
    else if (strict)
        row(QString(), tr("在严格沙箱里运行：低完整性，不是管理员，写不了你的文件（只能写它自己的数据文件夹）；内存上限 %1 MB").arg(m.runtime->memoryMB), true);
    else
        row(QString(), tr("以普通用户权限运行，不是管理员；内存上限 %1 MB").arg(m.runtime->memoryMB), true);
    using P = plugin::Permission;
    if (m.permissions.has(P::Processes))
        row(u"processes"_s, permissionText(P::Processes), true);
    if (m.permissions.has(P::ClipboardRead))
        row(u"clipboard:read"_s, permissionText(P::ClipboardRead), true);
    if (m.permissions.has(P::ClipboardWrite))
        row(u"clipboard:write"_s, permissionText(P::ClipboardWrite), true);
    if (m.permissions.has(P::Ui))
        row(u"ui"_s, permissionText(P::Ui), true, tr("部分"));
    if (m.permissions.has(P::WinshunSearch))
        row(u"winshun:search"_s, permissionText(P::WinshunSearch), true);
    if (m.permissions.has(P::WinshunOpen))
        row(u"winshun:open"_s, permissionText(P::WinshunOpen), true);
    if (m.permissions.has(P::WinshunHistory))
        row(u"winshun:history"_s, permissionText(P::WinshunHistory), true);
    if (m.permissions.has(P::WinshunContent))
        row(u"winshun:content"_s, permissionText(P::WinshunContent), true);
    if (m.permissions.has(P::FilesWrite))
        row(u"files:write"_s, permissionText(P::FilesWrite), strict, strict ? tr("严格沙箱：只能写它自己的数据文件夹") : tr("和你自己一样能写"));
    if (m.permissions.has(P::FilesRead))
        row(u"files:read"_s, m.permissions.readPaths.isEmpty() ? permissionText(P::FilesRead) : tr("读这些文件：%1").arg(m.permissions.readPaths.join(u"、"_s)), false);
    if (m.permissions.has(P::Network))
        row(u"network"_s, permissionText(P::Network), false, tr("Win顺 不能阻止它联网"));
    if (m.permissions.has(P::NetworkListen))
        row(u"network:listen"_s, m.permissions.listens.isEmpty() ? permissionText(P::NetworkListen)
            : tr("在局域网上开端口，让别的设备连进来：%1").arg(m.permissions.listens.join(u"、"_s)), false,
            tr("Win顺 不能阻止它开端口"));
    if (m.permissions.has(P::Notifications))
        row(u"notifications"_s, permissionText(P::Notifications), true, tr("经 Win顺 弹出，每 2 秒最多一条"));
    return rows;
}

QString PluginSettings::permissionText(plugin::Permission p)
{
    using P = plugin::Permission;
    switch (p) {
    case P::Processes: return tr("启动程序（起子进程，或打开程序、非网页链接）");
    case P::ClipboardRead: return tr("读剪贴板");
    case P::ClipboardWrite: return tr("写剪贴板");
    case P::Ui: return tr("打开自己的窗口、切换前台");
    case P::WinshunSearch: return tr("用 Win顺 搜索文件名和内容");
    case P::WinshunOpen: return tr("让 Win顺 打开文件和网址（以普通权限）");
    case P::WinshunHistory: return tr("读最近打开的项目");
    case P::WinshunContent: return tr("读 Win顺 从文档和文本文件里取出的文字");
    case P::FilesWrite: return tr("写文件");
    case P::FilesRead: return tr("读你的文件");
    case P::Network: return tr("联网");
    case P::NetworkListen: return tr("在局域网上开端口，让别的设备连进来");
    case P::Notifications: return tr("弹出系统通知");
    }
    return plugin::permissionName(p);
}

PluginListModel::Card PluginSettings::cardFor(const QString& id) const
{
    PluginListModel::Card c;
    c.id = id;
    const plugin::Entry* e = m_host->catalog().find(id);
    if (!e)
        return c;
    const plugin::Catalog& catalog = m_host->catalog();
    const Settings& settings = m_editor->m_settings;
    const PluginConfig* config = settings.plugin(id);
    const plugin::Host::State state = m_host->state(id);
    const bool dark = darkMode();
    c.bundled = e->source == plugin::Entry::Source::Bundled;
    c.hasReadme = QFileInfo::exists(e->dir + u"\\README.md"_s) || QFileInfo::exists(e->dir + u"\\README.txt"_s);
    c.hasData = QFileInfo::exists(QDir(m_dataRoot).filePath(id + u".data"_s));
    c.enabled = config && config->enabled;
    if (!e->manifest) {
        c.name = id;
        c.usable = false;
        c.error = e->error;
        c.status = tr("无法使用：%1").arg(e->error);
        c.tone = u"error"_s;
        c.icon = FileIconProvider::pluginIconUrl(id, u"glyph:U+EA86"_s, dark);
        return c;
    }
    const plugin::Manifest& m = *e->manifest;
    // A newer version asking for more than the user said yes to waits for
    // their OK (Catalog::setConfirmed): its switch shows off, so that one
    // click asks with the new ones marked, and the state line says what.
    const QStringList awaiting = catalog.awaitingConsent(id);
    if (!awaiting.isEmpty())
        c.enabled = false;
    c.name = m.name.pick(m_locale);
    c.version = m.version;
    c.author = m.author;
    c.homepage = m.homepage;
    // The AI: what the manifest offers, the card's switch, and what the AI
    // actually gets (the master switch on the AI page gates it all).
    c.aiExpose = m.aiExpose;
    c.aiEnabled = !config || config->ai;
    if (c.aiExpose) {
        const QStringList tools = plugin::AiBridge::toolNamesOf(m, settings.mcpPluginActions);
        if (!settings.mcp)
            c.aiNote = tr("AI 助手页的“允许 AI 连接”关着");
        else if (!settings.mcpPlugins)
            c.aiNote = tr("AI 助手页的“AI 可以使用插件”关着，打开后才成为工具");
        else if (!c.aiEnabled)
            c.aiNote = tr("关着：AI 看不到这个插件的命令");
        else if (!c.enabled)
            c.aiNote = awaiting.isEmpty() ? tr("插件停用时 AI 调用会得到“已停用”的提示") : tr("同意了新权限，AI 才能用它");
        else
            c.aiNote = tr("工具：%1").arg(tools.join(u"、"_s));
    }
    c.description = m.description.pick(m_locale);
    c.resident = m.resident();
    // The manifest's icon, else its first command's (as the rows and the
    // chip in the scope tabs fall back), else the puzzle piece.
    QString icon = m.icon;
    if (icon.isEmpty() && !m.commands.isEmpty())
        icon = m.commands.first().icon;
    c.icon = FileIconProvider::pluginIconUrl(id, icon.isEmpty() ? u"glyph:U+EA86"_s : icon, dark);

    using Status = plugin::Host::State::Status;
    QString memory;
    switch (state.status) {
    case Status::Off:
        if (!awaiting.isEmpty()) { // what it asks for, as the permission rows say it
            QStringList asked;
            for (const QString& name : awaiting) {
                const std::optional<plugin::Permission> p = plugin::permissionFromName(name);
                asked.push_back(u"“"_s + (p ? permissionText(*p) : name) + u"”"_s);
            }
            const bool never = !config || config->confirmed.isEmpty(); // a hand turned it on: no new version to speak of
            c.status = (never ? tr("还没同意它要的权限：%1。打开开关、同意后才能用") : tr("新版本要更多权限：%1。打开开关、同意后才能用"))
                           .arg(asked.join(tr("、")));
            c.tone = u"warn"_s;
            break;
        }
        c.status = tr("已停用");
        c.tone = u"idle"_s;
        break;
    case Status::Unusable:
        c.usable = false;
        c.error = state.error;
        c.status = tr("无法使用：%1").arg(state.error);
        c.tone = u"error"_s;
        break;
    case Status::Ready:
        if (!state.error.isEmpty()) { // the program is missing, or it would not start
            c.error = state.error;
            c.status = state.error;
            c.tone = u"warn"_s;
        } else {
            c.status = c.resident ? tr("就绪，用到时启动") : tr("就绪");
            c.tone = u"ok"_s;
        }
        break;
    case Status::Starting:
        c.status = tr("正在启动…");
        c.tone = u"busy"_s;
        break;
    case Status::Running:
        memory = memoryText(state.pid);
        c.status = memory.isEmpty() ? tr("运行中") : tr("运行中 · %1").arg(memory);
        if (state.timeouts > 0)
            c.status += tr(" · 超时 %n 次", "", state.timeouts);
        c.tone = u"ok"_s;
        break;
    case Status::Backoff:
        c.error = state.error;
        c.status = tr("崩溃，稍后重启：%1").arg(state.error);
        c.tone = u"warn"_s;
        break;
    case Status::Crashed:
        c.error = state.error;
        c.status = tr("连续崩溃，已停用：%1").arg(state.error);
        c.tone = u"error"_s;
        break;
    }

    // Keywords: each command's, as in use, the user's over the manifest's,
    // and who has one the plugin cannot use.
    for (const plugin::Command& command : m.commands) {
        if (command.keywords.isEmpty() && !(config && std::any_of(config->keywords.cbegin(), config->keywords.cend(),
                                                [&](const auto& k) { return k.first == command.id; })))
            continue; // triggered otherwise (regex, push): no keyword to show
        QString user;
        bool overridden = false;
        if (config) {
            for (const auto& [cmd, keyword] : config->keywords) {
                if (cmd == command.id) {
                    user = keyword;
                    overridden = true;
                }
            }
        }
        QString conflict;
        for (const plugin::KeywordConflict& k : catalog.conflicts()) {
            if (k.pluginId != id || k.commandId != command.id)
                continue;
            QString who;
            if (k.takenBy.startsWith(u"builtin:"))
                who = tr("Win顺 的“%1”").arg(k.takenBy.mid(8));
            else if (k.takenBy.startsWith(u"web:"))
                who = tr("网页搜索“%1”").arg(k.takenBy.mid(4));
            else if (k.takenBy.startsWith(u"fav:"))
                who = tr("收藏“%1”").arg(k.takenBy.mid(4));
            else {
                const QString other = k.takenBy.mid(7, k.takenBy.indexOf(u'/') - 7);
                who = tr("插件“%1”").arg(pluginName(other));
            }
            conflict = tr("关键字 %1 已被%2占用，改一个").arg(k.keyword, who);
            break;
        }
        const QString title = command.title.pick(m_locale).isEmpty() ? c.name : command.title.pick(m_locale);
        c.keywords.push_back(QVariantMap {{u"command"_s, command.id}, {u"title"_s, title},
            {u"keyword"_s, overridden ? user : command.keywords.join(u' ')}, {u"defaults"_s, command.keywords.join(u' ')},
            {u"inUse"_s, catalog.keywordsOf(id, command.id).join(u' ')}, {u"conflict"_s, conflict}, {u"user"_s, overridden}});
    }
    c.permissions = permissionRows(id);

    // The declared settings and their values (a password's: whether one is kept).
    const QJsonObject values = m_host->pluginSettings(id);
    for (const plugin::Setting& s : m.settings) {
        QVariantMap row {{u"key"_s, s.key}, {u"type"_s, s.type}, {u"title"_s, s.title.pick(m_locale).isEmpty() ? s.key : s.title.pick(m_locale)},
            {u"hint"_s, s.hint.pick(m_locale)}};
        const QJsonValue value = values.value(s.key);
        if (s.type == u"password") {
            row.insert(u"value"_s, QString());
            row.insert(u"set"_s, !value.toString().isEmpty());
        } else if (s.type == u"bool")
            row.insert(u"value"_s, value.toBool());
        else if (s.type == u"choice") {
            QStringList options;
            QStringList titles;
            choiceOptions(s, m_locale, &options, &titles);
            const QString current = value.toVariant().toString();
            row.insert(u"options"_s, titles);
            row.insert(u"optionValues"_s, options);
            row.insert(u"optionIndex"_s, std::max<qsizetype>(0, options.indexOf(current)));
            row.insert(u"value"_s, current);
        } else
            row.insert(u"value"_s, value.toVariant().toString());
        c.settings.push_back(row);
    }
    return c;
}

void PluginSettings::rebuild()
{
    if (m_preview) {
        if (m_list.setCards(m_preview->cards))
            emit listChanged();
        return;
    }
    if (!m_host) {
        if (m_list.setCards({}))
            emit listChanged();
        m_gallery.rebuild();
        return;
    }
    std::vector<PluginListModel::Card> cards;
    for (const plugin::Entry& e : m_host->catalog().entries())
        cards.push_back(cardFor(e.id));
    // The user's own first, then the bundled ones, each by name.
    std::stable_sort(cards.begin(), cards.end(), [](const PluginListModel::Card& a, const PluginListModel::Card& b) {
        if (a.bundled != b.bundled)
            return !a.bundled;
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    const bool rowsChanged = m_list.setCards(std::move(cards));
    QString note;
    if (m_host->tokenSource() == plugin::Runner::TokenSource::Restricted)
        note = tr("插件看不到你在资源管理器里登录的网络位置：资源管理器以管理员身份运行或没有运行，插件只能用 Win顺 自己降权后的身份，它和资源管理器不在同一个登录会话里。");
    if (note != m_restrictedNote) {
        m_restrictedNote = note;
        emit stateChanged();
    }
    if (rowsChanged)
        emit listChanged();
    m_gallery.rebuild(); // what is installed now, the cards' icons, the language
}

void PluginSettings::readMemory()
{
    if (!m_host || m_preview)
        return;
    bool running = false;
    for (const plugin::Entry& e : m_host->catalog().entries())
        running = running || m_host->state(e.id).status == plugin::Host::State::Status::Running;
    if (running)
        rebuild();
}

// ---- the switch, keywords, settings ----------------------------------------------

void PluginSettings::setEnabled(const QString& id, bool on)
{
    if (!m_host)
        return;
    const plugin::Entry* e = m_host->catalog().find(id);
    if (!e || !e->manifest)
        return;
    Settings& settings = m_editor->m_settings;
    if (!on) {
        PluginConfig& config = settings.pluginConfig(id);
        if (!config.enabled)
            return;
        config.enabled = false;
        m_editor->commit(); // App::applyPlugins turns it off in the host
        return;
    }
    // On: the first time, and whenever a newer version asks for more, the
    // user sees what it asks for first.
    const PluginConfig* config = settings.plugin(id);
    const QStringList confirmed = config ? config->confirmed : QStringList();
    const QStringList more = plugin::install::unconfirmed(*e->manifest, confirmed);
    if (confirmed.isEmpty() || !more.isEmpty()) {
        QVariantMap info {{u"id"_s, id}, {u"name"_s, e->manifest->name.pick(m_locale)}, {u"version"_s, e->manifest->version},
            {u"author"_s, e->manifest->author}, {u"homepage"_s, e->manifest->homepage},
            {u"source"_s, e->source == plugin::Entry::Source::Bundled ? tr("随 Win顺 安装") : tr("你的插件文件夹")},
            {u"permissions"_s, permissionRows(id)}, {u"more"_s, !confirmed.isEmpty()}, {u"newNames"_s, more}};
        emit confirmationNeeded(info);
        return;
    }
    confirmEnable(id);
}

void PluginSettings::declineEnable(const QString& id)
{
    // The dialog's no (or Esc). A plugin switched on that waits for the OK
    // goes off for real: a normal off card, asking again next time it is
    // turned on. Others have nothing to undo.
    if (!m_host || m_host->catalog().awaitingConsent(id).isEmpty())
        return;
    const PluginConfig* config = m_editor->m_settings.plugin(id);
    if (!config || !config->enabled)
        return;
    m_editor->m_settings.pluginConfig(id).enabled = false;
    m_editor->commit();
}

void PluginSettings::confirmEnable(const QString& id)
{
    if (!m_host)
        return;
    const plugin::Entry* e = m_host->catalog().find(id);
    if (!e || !e->manifest)
        return;
    PluginConfig& config = m_editor->m_settings.pluginConfig(id);
    config.enabled = true;
    config.confirmed = e->manifest->permissions.names();
    if (config.confirmed.isEmpty())
        config.confirmed = {QString(kConfirmedNone)};
    m_editor->commit();
}

void PluginSettings::setAiEnabled(const QString& id, bool on)
{
    PluginConfig& config = m_editor->m_settings.pluginConfig(id);
    if (config.ai == on)
        return;
    config.ai = on;
    m_editor->commit(); // App::applyPlugins tells the bridge
}

void PluginSettings::setKeyword(const QString& id, const QString& command, const QString& keyword)
{
    const QString word = keyword.trimmed();
    if (word.contains(u' ') || word.contains(u'"'))
        return;
    PluginConfig& config = m_editor->m_settings.pluginConfig(id);
    for (auto& [cmd, k] : config.keywords) {
        if (cmd == command) {
            if (k == word)
                return;
            k = word;
            m_editor->commit();
            return;
        }
    }
    config.keywords.append({command, word});
    m_editor->commit();
}

void PluginSettings::resetKeyword(const QString& id, const QString& command)
{
    PluginConfig& config = m_editor->m_settings.pluginConfig(id);
    const qsizetype had = config.keywords.size();
    config.keywords.removeIf([&](const std::pair<QString, QString>& k) { return k.first == command; });
    if (config.keywords.size() != had)
        m_editor->commit();
}

void PluginSettings::applyPluginSettings(const QString& id)
{
    if (!m_host)
        return;
    const plugin::Entry* e = m_host->catalog().find(id);
    const PluginConfig* config = m_editor->m_settings.plugin(id);
    if (e && e->manifest)
        m_host->setPluginSettings(id, plugin::secrets::settingsOf(id, *e->manifest, config ? config->settings : QList<std::pair<QString, QString>>()));
}

void PluginSettings::setSettingValue(const QString& id, const QString& key, const QVariant& value)
{
    if (!m_host)
        return;
    const plugin::Entry* e = m_host->catalog().find(id);
    if (!e || !e->manifest)
        return;
    QString text;
    if (value.typeId() == QMetaType::Bool)
        text = value.toBool() ? u"true"_s : u"false"_s;
    else
        text = value.toString();
    PluginConfig& config = m_editor->m_settings.pluginConfig(id);
    const bool iniChanged = plugin::secrets::store(id, *e->manifest, key, text, config.settings);
    if (iniChanged)
        m_editor->commit(); // App::applyPlugins tells the host
    else
        applyPluginSettings(id); // a password: the host hears it from here
    rebuild();
}

void PluginSettings::pickFolderSetting(const QString& id, const QString& key)
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择文件夹"));
    if (!folder.isEmpty())
        setSettingValue(id, key, QDir::toNativeSeparators(folder));
}

void PluginSettings::pickFileSetting(const QString& id, const QString& key)
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString file = shell::pickFile(owner, tr("选择文件"), {});
    if (!file.isEmpty())
        setSettingValue(id, key, QDir::toNativeSeparators(file));
}

// ---- installing and removing -----------------------------------------------------

void PluginSettings::installFromFolder()
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择插件文件夹（里面有 plugin.json）"));
    if (!folder.isEmpty())
        install(folder);
}

void PluginSettings::installFromArchive()
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString file = shell::pickFile(owner, tr("选择插件压缩包"), {{tr("插件压缩包"), u"*.zip;*.wsplugin"_s}, {tr("所有文件"), u"*.*"_s}});
    if (!file.isEmpty())
        install(file);
}

void PluginSettings::install(const QString& given)
{
    installFrom(given, {}, {});
}

void PluginSettings::installFrom(const QString& given, const QString& expectedId, const QString& name)
{
    if (!m_host)
        return;
    QString path = given;
    if (path.startsWith(u"file:", Qt::CaseInsensitive))
        path = QUrl(path).toLocalFile();
    cancelInstall();
    std::optional<plugin::install::Candidate> candidate = plugin::install::inspect(path);
    if (!candidate) {
        setMessage(tr("找不到 %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (!candidate->manifest) {
        setMessage(tr("不能安装 %1：%2").arg(name.isEmpty() ? QFileInfo(path).fileName() : name, candidate->error));
        return;
    }
    const QString id = candidate->manifest->id;
    if (!expectedId.isEmpty() && id != expectedId) {
        plugin::install::discard(*candidate);
        setMessage(tr("没有安装 %1：下载来的是另一个插件（%2）").arg(name.isEmpty() ? expectedId : name, id));
        return;
    }
    if (const plugin::Entry* e = m_host->catalog().find(id); e && e->source == plugin::Entry::Source::User && e->manifest) {
        const int compared = plugin::compareVersions(candidate->manifest->version, e->manifest->version);
        QVariantMap info {{u"id"_s, id}, {u"name"_s, candidate->manifest->name.pick(m_locale)}, {u"installedVersion"_s, e->manifest->version},
            {u"newVersion"_s, candidate->manifest->version}, {u"downgrade"_s, compared < 0}, {u"same"_s, compared == 0}};
        m_pendingInstall = std::move(candidate);
        emit installQuestion(info);
        return;
    }
    finishInstall(std::move(*candidate));
}

void PluginSettings::confirmInstall()
{
    if (!m_pendingInstall)
        return;
    plugin::install::Candidate candidate = std::move(*m_pendingInstall);
    m_pendingInstall.reset();
    finishInstall(std::move(candidate));
}

void PluginSettings::cancelInstall()
{
    if (!m_pendingInstall)
        return;
    plugin::install::discard(*m_pendingInstall);
    m_pendingInstall.reset();
}

void PluginSettings::finishInstall(plugin::install::Candidate candidate)
{
    if (!m_host)
        return;
    const QString id = candidate.manifest->id;
    const QString name = candidate.manifest->name.pick(m_locale);
    m_host->stop(id); // its program, if running, lets go of its folder
    // A moment for the process to leave (shutdown is asked for, then the job ends it).
    const QDeadlineTimer deadline(2500);
    while (!deadline.hasExpired()) {
        const plugin::Host::State::Status s = m_host->state(id).status;
        if (s != plugin::Host::State::Status::Running && s != plugin::Host::State::Status::Starting)
            break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QString error;
    const QString placed = plugin::install::place(candidate, m_userDir, &error);
    plugin::install::discard(candidate);
    if (placed.isEmpty()) {
        setMessage(tr("没有安装 %1：%2").arg(name, error));
        return;
    }
    m_host->catalog().scan();
    // Installed, and off until the user turns it on (the permissions are
    // shown then); a version put over one that is on and asking for more
    // waits for the user's OK (Catalog::setConfirmed), its switch asking.
    const PluginConfig* config = m_editor->m_settings.plugin(id);
    const QString version = candidate.manifest->version;
    if (!m_host->catalog().awaitingConsent(id).isEmpty())
        setMessage(tr("已安装 %1 %2。新版本要更多权限，先停着；打开它的开关、同意后才能用").arg(name, version));
    else
        setMessage(config && config->enabled ? tr("已安装 %1 %2").arg(name, version) : tr("已安装 %1 %2，打开它的开关就能用").arg(name, version));
    rebuild();
}

void PluginSettings::uninstall(const QString& id)
{
    if (!m_host)
        return;
    const plugin::Entry* e = m_host->catalog().find(id);
    if (!e || e->source == plugin::Entry::Source::Bundled)
        return;
    emit uninstallQuestion(QVariantMap {{u"id"_s, id}, {u"name"_s, pluginName(id)},
        {u"hasData"_s, QFileInfo::exists(QDir(m_dataRoot).filePath(id + u".data"_s))}});
}

void PluginSettings::confirmUninstall(const QString& id, bool withData)
{
    if (!m_host)
        return;
    const plugin::Entry* e = m_host->catalog().find(id);
    if (!e || e->source == plugin::Entry::Source::Bundled)
        return;
    const QString name = pluginName(id);
    m_host->stop(id);
    const QDeadlineTimer deadline(2500);
    while (!deadline.hasExpired()) {
        const plugin::Host::State::Status s = m_host->state(id).status;
        if (s != plugin::Host::State::Status::Running && s != plugin::Host::State::Status::Starting)
            break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    const QString error = plugin::install::remove(m_userDir, m_dataRoot, id, withData);
    // Its section and its passwords go with it, whatever became of the folder.
    plugin::secrets::removeAll(id);
    Settings& settings = m_editor->m_settings;
    const qsizetype had = settings.plugins.size();
    settings.plugins.removeIf([&](const PluginConfig& c) { return c.id == id; });
    m_host->catalog().scan();
    if (settings.plugins.size() != had)
        m_editor->commit();
    setMessage(error.isEmpty() ? tr("已卸载 %1").arg(name) : tr("卸载 %1：%2").arg(name, error));
    rebuild();
}

void PluginSettings::reload(const QString& id)
{
    if (m_host)
        m_host->reload(id);
}

void PluginSettings::openFolder(const QString& id)
{
    if (!m_host)
        return;
    if (const plugin::Entry* e = m_host->catalog().find(id))
        shell::open(e->dir);
}

void PluginSettings::openReadme(const QString& id)
{
    if (!m_host)
        return;
    const plugin::Entry* e = m_host->catalog().find(id);
    if (!e)
        return;
    for (const QString& name : {u"README.md"_s, u"README.txt"_s}) {
        if (QFileInfo::exists(e->dir + u'\\' + name)) {
            shell::open(e->dir + u'\\' + name);
            return;
        }
    }
    shell::open(e->dir);
}

void PluginSettings::openHomepage(const QString& id)
{
    if (const PluginListModel::Card* c = m_list.card(id); c && !c->homepage.isEmpty())
        shell::openUrl(c->homepage);
}

void PluginSettings::openLog()
{
    if (!m_logPath.isEmpty() && QFileInfo::exists(m_logPath))
        shell::open(m_logPath);
    else
        shell::open(Settings::dataDir());
}

void PluginSettings::openUserDir()
{
    if (!m_userDir.isEmpty()) {
        QDir().mkpath(m_userDir);
        shell::open(m_userDir);
    }
}

void PluginSettings::clearLog()
{
    m_log.clear();
    emit logPluginsChanged();
}

// ---- templates --------------------------------------------------------------------

namespace {

// The files of each template (the plugin design, §5.5; the protocol and
// the SDKs in docs/plugins.md). Every template is written on an SDK file
// (src/plugins-sdk, built in as ":/sdk/…"), copied into the plugin's folder
// so the plugin stays self-contained. PowerShell scripts carry a BOM:
// Windows PowerShell 5.1 reads a .ps1 without one in the ANSI code page.
struct TemplateFile {
    QString name;
    QString text;
    bool bom = false;
};

QString manifestText(const QString& id, const QString& name, const QString& description, const QString& runtime, const QString& extra = {})
{
    return u"{\n  \"manifest\": 1,\n  \"id\": \"%1\",\n  \"name\": {\"zh\": \"%2\", \"en\": \"%2\"},\n  \"version\": \"0.1.0\",\n"
           "  \"description\": \"%3\",\n"
           "%4"
           "  \"permissions\": [],\n"
           "  \"commands\": [{\n    \"id\": \"main\",\n    \"keyword\": \"%2\",\n    \"title\": {\"zh\": \"%2\", \"en\": \"%2\"},\n"
           "    \"hint\": {\"zh\": \"随便输入\", \"en\": \"Type anything\"},\n"
           "    \"actions\": [{\"id\": \"default\", \"title\": {\"zh\": \"复制\", \"en\": \"Copy\"}, \"key\": \"Enter\"}]\n  }],\n"
           "  \"settings\": [{\"key\": \"greeting\", \"type\": \"text\", \"title\": {\"zh\": \"问候语\", \"en\": \"Greeting\"}, \"default\": \"你好\"}]%5\n}\n"_s
        .arg(id, name, description, runtime.isEmpty() ? QString() : u"  \"runtime\": %1,\n"_s.arg(runtime), extra);
}

std::vector<TemplateFile> templateFiles(const QString& kind, const QString& id, const QString& name)
{
    std::vector<TemplateFile> files;
    const QString readmeTail = u"\n## 协议速查\n\n"
                               "- 一次性插件：查询在环境变量 `WS_QUERY`（关键字之后的文字）、`WS_TEXT`、`WS_COMMAND`、`WS_SETTINGS`（JSON）里；stdout 打印一个 JSON 对象 `{\"items\": [...]}`、一个数组，或纯文本行（每行一条）。\n"
                               "- 常驻插件：stdio 上 JSON-RPC 2.0，一行一条：`initialize`、`query`（回 `{\"items\": [...]}`）、`action`（回 `{\"effects\": [...]}`）、`settingsChanged`、`shutdown`；可以发 `winshun/tool`、`winshun/log`、`winshun/notify`、`winshun/progress`。SDK 把这些都包好了。\n"
                               "- 一条结果：`{\"id\", \"title\", \"subtitle\", \"icon\", \"score\", \"autocomplete\", \"open\": {\"path\" | \"url\"}, \"copy\", \"actions\", \"valid\", \"data\"}`。\n"
                               "- 图标：`icon.png`（请用 PNG，不支持 SVG）、`ext:pdf`、`file:C:\\\\...`、`glyph:U+E8E5`、`color:#ff8800`。\n"
                               "- 改了脚本，开发者模式下会自动重新加载；设置 → 插件 → 开发者 → 消息记录里能看到每一条消息。完整文档：docs/plugins.md。\n"_s;
    if (kind == u"powershell") {
        files.push_back({u"plugin.json"_s, manifestText(id, name, u"用 PowerShell 写的常驻插件"_s, u"{\"mode\": \"resident\", \"program\": \"powershell\", \"script\": \"main.ps1\"}"_s)});
        files.push_back({u"main.ps1"_s,
            u"# Win顺 常驻插件（Windows PowerShell），用 WinShun.psm1 SDK：stdio 上一行一条 JSON-RPC，SDK 包好了。\n"
            "# 这个文件要保存为带 BOM 的 UTF-8（Windows PowerShell 5.1 才能读对中文）。\n"
            "Import-Module \"$PSScriptRoot\\WinShun.psm1\"\n\n"
            "Start-WinShunPlugin -Name '%1' -OnQuery {\n"
            "    param($q, $plugin)\n"
            "    $greeting = if ($plugin.settings.greeting) { $plugin.settings.greeting } else { '你好' }\n"
            "    @(\n"
            "        (New-WinShunItem -Id 'echo' -Title \"$greeting，$($q.text)\" -Subtitle '按 Enter 复制' -Copy $q.text -Score 90),\n"
            "        (New-WinShunItem -Id 'upper' -Title $q.text.ToUpperInvariant() -Subtitle '大写' -Copy $q.text.ToUpperInvariant())\n"
            "    )\n"
            "}\n"
            "# Enter 默认复制 -Copy 的文字；要做别的，加 -OnAction { param($a, $plugin) ... }，\n"
            "# 返回 New-WinShunEffect -Notify '...'、-OpenPath、-OpenUrl、-SetQuery、-Hide 等。\n"_s.arg(id),
            true});
        files.push_back({u"README.md"_s,
            u"# %1\n\n用 Windows PowerShell 写的 Win顺 常驻插件：输入 `%1 文字` 看效果。\n\n- `plugin.json`：清单（关键字、权限、设置项）。\n- `main.ps1`：脚本，带 BOM 的 UTF-8。\n- `WinShun.psm1`：SDK（随 Win顺 提供，新版本可以直接覆盖）。\n"_s.arg(name) + readmeTail});
    } else if (kind == u"python") {
        files.push_back({u"plugin.json"_s, manifestText(id, name, u"用 Python 写的一次性插件：每次查询起一个进程"_s, u"{\"mode\": \"once\", \"program\": \"python\", \"script\": \"main.py\"}"_s)});
        files.push_back({u"main.py"_s,
            u"# Win顺 一次性插件（Python），用 winshun.py SDK：查询在环境变量里，结果打印成一个 JSON 对象。\n"
            "import winshun\n\n"
            "q = winshun.query()\n"
            "settings = winshun.settings()\n"
            "greeting = settings.get(\"greeting\") or \"你好\"\n"
            "winshun.reply([\n"
            "    winshun.item(\"echo\", f\"{greeting}，{q.text}\", \"按 Enter 复制\", copy=q.text, score=90),\n"
            "    winshun.item(\"upper\", q.text.upper(), \"大写\", copy=q.text.upper()),\n"
            "])\n"_s});
        files.push_back({u"README.md"_s,
            u"# %1\n\n用 Python 写的 Win顺 一次性插件：输入 `%1 文字` 看效果。需要 Python 3。\n\n- `plugin.json`：清单。\n- `main.py`：脚本。\n- `winshun.py`：SDK（随 Win顺 提供，新版本可以直接覆盖）。\n"_s.arg(name) + readmeTail});
    } else if (kind == u"python-resident") {
        files.push_back({u"plugin.json"_s, manifestText(id, name, u"用 Python 写的常驻插件"_s, u"{\"mode\": \"resident\", \"program\": \"python\", \"script\": \"main.py\"}"_s)});
        files.push_back({u"main.py"_s,
            u"# Win顺 常驻插件（Python），用 winshun.py SDK：一个进程一直开着，stdio 上一行一条 JSON-RPC，SDK 包好了。\n"
            "import winshun\n\n\n"
            "def on_query(q, plugin):\n"
            "    greeting = (plugin.settings or {}).get(\"greeting\") or \"你好\"\n"
            "    return [\n"
            "        winshun.item(\"echo\", f\"{greeting}，{q.text}\", \"按 Enter 复制\", copy=q.text, score=90),\n"
            "        winshun.item(\"upper\", q.text.upper(), \"大写\", copy=q.text.upper()),\n"
            "    ]\n\n\n"
            "# Enter 默认复制 copy 的文字；要做别的，传 on_action=lambda a, plugin: [winshun.notify(\"…\"), winshun.hide()]。\n"
            "winshun.serve(on_query, name=\"%1\")\n"_s.arg(id)});
        files.push_back({u"README.md"_s,
            u"# %1\n\n用 Python 写的 Win顺 常驻插件：输入 `%1 文字` 看效果。需要 Python 3。\n\n- `plugin.json`：清单。\n- `main.py`：脚本。\n- `winshun.py`：SDK（随 Win顺 提供，新版本可以直接覆盖）。\n"_s.arg(name) + readmeTail});
    } else if (kind == u"cpp") {
        files.push_back({u"plugin.json"_s, manifestText(id, name, u"用 C++ 写的常驻插件（winshun.h 单头文件 SDK）"_s, u"{\"mode\": \"resident\", \"program\": \"%1.exe\", \"sandbox\": \"strict\"}"_s.arg(name))});
        files.push_back({u"main.cpp"_s,
            u"// Win顺 常驻插件（C++），用 winshun.h 单头文件 SDK：无 Qt，只要 Win32 和标准库。\n"
            "#include \"winshun.h\"\n\n"
            "struct Plugin : winshun::Plugin {\n"
            "    std::string name() const override { return \"%1\"; }\n\n"
            "    winshun::Items query(const winshun::Query& q) override\n"
            "    {\n"
            "        const std::string greeting = settings()[\"greeting\"].isString() ? settings()[\"greeting\"].toString() : \"你好\";\n"
            "        std::string upper = q.text;\n"
            "        for (char& c : upper)\n"
            "            c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);\n"
            "        return {\n"
            "            winshun::Item(\"echo\", greeting + \"，\" + q.text, \"按 Enter 复制\").withCopy(q.text).withScore(90),\n"
            "            winshun::Item(\"upper\", upper, \"大写\").withCopy(upper),\n"
            "        };\n"
            "    }\n"
            "    // Enter 默认复制 copy 的文字；要做别的，重写 action() 返回 winshun::effect::notify(...)、openPath、hide 等。\n"
            "};\n\n"
            "int main()\n"
            "{\n"
            "    return Plugin().serve();\n"
            "}\n"_s.arg(id)});
        files.push_back({u"CMakeLists.txt"_s,
            u"cmake_minimum_required(VERSION 3.21)\n"
            "project(%1 LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\n"
            "add_executable(%1 main.cpp)\n"
            "target_compile_definitions(%1 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)\n"
            "target_compile_options(%1 PRIVATE /utf-8)\n"
            "# The C runtime linked in: the exe needs no DLL, and the strict sandbox (low integrity) loads nothing else.\n"
            "set_target_properties(%1 PROPERTIES MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>\")\n"
            "# Built straight into the plugin folder, where plugin.json names it.\n"
            "set_target_properties(%1 PROPERTIES RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_SOURCE_DIR}\" RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_SOURCE_DIR}\")\n"_s.arg(name)});
        files.push_back({u"README.md"_s,
            u"# %1\n\n用 C++ 写的 Win顺 常驻插件：无 Qt，只要 Win32 和标准库，`winshun.h` 一个头文件就是 SDK。\n\n"
            "编译（Visual Studio 的开发者命令行，或装了 CMake 和 MSVC 的终端）：\n\n"
            "```\ncmake -B build\ncmake --build build --config Release\n```\n\n"
            "`%1.exe` 会直接生成在这个文件夹里（`plugin.json` 的 `runtime.program` 就是它），然后打开插件的开关、输入 `%1 文字`。\n"
            "它跑在严格沙箱里（`sandbox: strict`：低完整性、不能起子进程、不碰剪贴板），要写文件只能写 `dataDir()`；需要更多就把 `sandbox` 去掉。\n\n"
            "- `plugin.json`：清单。\n- `main.cpp`：代码。\n- `winshun.h`：SDK（随 Win顺 提供，新版本可以直接覆盖）。\n- `CMakeLists.txt`：编译脚本。\n"_s.arg(name) + readmeTail});
    } else {
        files.push_back({u"plugin.json"_s,
            u"{\n  \"manifest\": 1,\n  \"id\": \"%1\",\n  \"name\": {\"zh\": \"%2\", \"en\": \"%2\"},\n  \"version\": \"0.1.0\",\n"
            "  \"description\": \"零代码插件：关键字开网址、跳文件夹、保存的搜索\",\n"
            "  \"commands\": [\n"
            "    {\"id\": \"web\", \"keyword\": \"%2\", \"title\": {\"zh\": \"搜 Bing\", \"en\": \"Search Bing\"}, \"run\": {\"url\": \"https://www.bing.com/search?q=%s\"}},\n"
            "    {\"id\": \"dl\", \"keyword\": \"%2dl\", \"title\": {\"zh\": \"下载文件夹\", \"en\": \"Downloads\"}, \"run\": {\"folder\": \"%USERPROFILE%\\\\Downloads\"}},\n"
            "    {\"id\": \"pdf\", \"keyword\": \"%2pdf\", \"title\": {\"zh\": \"找 PDF\", \"en\": \"Find PDFs\"}, \"icon\": \"ext:pdf\", \"run\": {\"search\": {\"query\": \"ext:pdf %s\", \"scope\": \"files\"}}}\n"
            "  ]\n}\n"_s.arg(id, name)});
        files.push_back({u"README.md"_s,
            u"# %1\n\n零代码 Win顺 插件：只有 `plugin.json`，没有脚本。`run` 可以是 `url`（%s 是要搜的文字）、`program`、`folder`、`search` 或 `items`。\n"_s.arg(name) + readmeTail});
    }
    return files;
}

// The SDK file a template is written on: ":/sdk/<name>", copied beside it.
QString sdkFileFor(const QString& kind)
{
    if (kind == u"powershell")
        return u"WinShun.psm1"_s;
    if (kind == u"python" || kind == u"python-resident")
        return u"winshun.py"_s;
    if (kind == u"cpp")
        return u"winshun.h"_s;
    return {};
}

} // namespace

void PluginSettings::copyDevelopmentPrompt()
{
    QFile guide(u":/sdk/DEVELOPMENT.md"_s);
    if (!guide.open(QIODevice::ReadOnly)) {
        setMessage(tr("读不了插件开发指南"));
        return;
    }
    QGuiApplication::clipboard()->setText(QString::fromUtf8(guide.readAll()));
    setMessage(tr("已复制开发提示；把它和创建好的模板交给 AI，再补上你想做的功能"));
}

QString PluginSettings::createFromTemplate(const QString& kind, const QString& givenName)
{
    if (!templateKinds().contains(kind))
        return tr("没有这种模板");
    const QString name = sanitizedName(givenName);
    if (name.isEmpty())
        return tr("名字只能用小写字母、数字和连字符");
    const QString id = templateAuthor() + u'.' + name;
    if (!plugin::validPluginId(id))
        return tr("名字不合规则");
    if (m_userDir.isEmpty())
        return tr("插件文件夹未知");
    const QString folder = QDir(m_userDir).filePath(id);
    if (QFileInfo::exists(folder))
        return tr("已经有一个叫 %1 的插件了").arg(id);
    if (!QDir().mkpath(folder))
        return tr("建不了 %1").arg(QDir::toNativeSeparators(folder));
    for (const TemplateFile& f : templateFiles(kind, id, name)) {
        QFile file(folder + u'/' + f.name);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return tr("写不了 %1").arg(QDir::toNativeSeparators(file.fileName()));
        if (f.bom)
            file.write("\xEF\xBB\xBF");
        file.write(f.text.toUtf8());
    }
    QStringList supportFiles{u"DEVELOPMENT.md"_s};
    if (const QString sdk = sdkFileFor(kind); !sdk.isEmpty())
        supportFiles.append(sdk);
    for (const QString& sdk : supportFiles) {
        // Byte for byte (the .psm1 keeps its BOM); a copy from the resources is read-only, so not that.
        QFile from(u":/sdk/"_s + sdk);
        QFile to(folder + u'/' + sdk);
        if (!from.open(QIODevice::ReadOnly) || !to.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return tr("写不了 %1").arg(QDir::toNativeSeparators(to.fileName()));
        to.write(from.readAll());
    }
    if (m_host)
        m_host->catalog().scan();
    setMessage(tr("已建好 %1，打开它的开关、输入 %2 试试；改了脚本，开发者模式下会自动重新加载").arg(id, name));
    shell::open(QDir::toNativeSeparators(folder));
    rebuild();
    return {};
}

// ---- the preview (no host) ---------------------------------------------------------

void PluginSettings::loadPreview()
{
    m_preview = std::make_unique<Preview>();
    m_userDir = u"C:\\Users\\me\\AppData\\Local\\WinShun\\plugins"_s;
    const bool dark = darkMode();
    const auto glyph = [&](const QString& code) { return FileIconProvider::pluginIconUrl(u"preview"_s, u"glyph:"_s + code, dark); };
    const auto permission = [](const QString& name, const QString& text, bool enforced, const QString& note = {}) {
        return QVariantMap {{u"name"_s, name}, {u"text"_s, text}, {u"enforced"_s, enforced}, {u"note"_s, note}};
    };
    const QVariantMap admin = permission({}, tr("以普通用户权限运行，不是管理员；内存上限 %1 MB").arg(256), true);
    const QVariantMap zeroCode = permission({}, tr("不运行自己的程序：Win顺 按它的清单打开网址、程序或文件夹，用你的普通权限，不是管理员"), true);
    const QVariantMap strict = permission({}, tr("在严格沙箱里运行：低完整性，不是管理员，写不了你的文件（只能写它自己的数据文件夹）；内存上限 %1 MB").arg(64), true);
    const auto keyword = [](const QString& command, const QString& title, const QString& kw, const QString& defaults, const QString& conflict, bool user) {
        return QVariantMap {{u"command"_s, command}, {u"title"_s, title}, {u"keyword"_s, kw}, {u"defaults"_s, defaults}, {u"inUse"_s, conflict.isEmpty() ? kw : QString()},
            {u"conflict"_s, conflict}, {u"user"_s, user}};
    };
    std::vector<PluginListModel::Card> cards;
    {
        PluginListModel::Card c;
        c.id = u"lingcore.kill"_s;
        c.name = tr("结束进程");
        c.version = u"1.2.0"_s;
        c.author = u"LingCore"_s;
        c.homepage = u"https://github.com/LingCore/WinShun"_s;
        c.description = tr("kill <进程名或 PID> 列出进程，Enter 结束，Ctrl+Enter 连同子进程");
        c.icon = glyph(u"U+E711"_s);
        c.enabled = true;
        c.resident = true;
        c.status = tr("运行中 · %1").arg(u"38 MB"_s) + tr(" · 超时 %n 次", "", 2);
        c.tone = u"ok"_s;
        c.keywords = {keyword(u"kill"_s, tr("结束进程"), u"kill jc"_s, u"kill jc"_s, {}, false)};
        c.permissions = {admin, permission(u"processes"_s, tr("启动程序（起子进程，或打开程序、非网页链接）"), true)};
        c.settings = {QVariantMap {{u"key"_s, u"showSystem"_s}, {u"type"_s, u"bool"_s}, {u"title"_s, tr("也列出系统进程")}, {u"hint"_s, QString()}, {u"value"_s, true}}};
        c.hasReadme = true;
        c.hasData = true;
        c.aiExpose = true;
        c.aiEnabled = true;
        c.aiNote = tr("工具：%1").arg(u"plugin_lingcore_kill_kill"_s);
        cards.push_back(c);
    }
    {
        PluginListModel::Card c;
        c.id = u"lingcore.translate"_s;
        c.name = tr("翻译");
        c.version = u"0.9.1"_s;
        c.author = u"LingCore"_s;
        c.description = tr("tr <句子> 翻译；整句中文或英文时推送一行");
        c.icon = glyph(u"U+E8C1"_s);
        c.enabled = true;
        c.resident = true;
        c.status = tr("就绪，用到时启动");
        c.tone = u"ok"_s;
        c.keywords = {keyword(u"tr"_s, tr("翻译"), u"tr"_s, u"tr"_s, {}, false)};
        c.permissions = {admin, permission(u"network"_s, tr("联网"), false, tr("Win顺 不能阻止它联网")),
            permission(u"winshun:search"_s, tr("用 Win顺 搜索文件名和内容"), true)};
        c.settings = {QVariantMap {{u"key"_s, u"apiKey"_s}, {u"type"_s, u"password"_s}, {u"title"_s, tr("API 密钥")}, {u"hint"_s, tr("存在 Windows 凭据管理器里，不写进设置文件")}, {u"value"_s, QString()}, {u"set"_s, true}},
            QVariantMap {{u"key"_s, u"engine"_s}, {u"type"_s, u"choice"_s}, {u"title"_s, tr("用哪家")}, {u"hint"_s, QString()}, {u"value"_s, u"deepl"_s},
                {u"options"_s, QStringList {u"DeepL"_s, u"Google"_s, u"百度"_s}}, {u"optionValues"_s, QStringList {u"deepl"_s, u"google"_s, u"baidu"_s}}, {u"optionIndex"_s, 0}},
            // Too long to stand side by side: one button and a menu (PluginCard's choiceControl).
            QVariantMap {{u"key"_s, u"style"_s}, {u"type"_s, u"choice"_s}, {u"title"_s, u"译文风格"_s}, {u"hint"_s, QString()}, {u"value"_s, u"free"_s},
                {u"options"_s, QStringList {u"直译（逐句对照，保留原文的句子结构和专有名词）"_s, u"意译（读起来像母语写的；长句会拆开重排）"_s, u"术语表优先（按你的术语表翻，其余意译）"_s}},
                {u"optionValues"_s, QStringList {u"literal"_s, u"free"_s, u"glossary"_s}}, {u"optionIndex"_s, 1}},
            QVariantMap {{u"key"_s, u"target"_s}, {u"type"_s, u"text"_s}, {u"title"_s, tr("目标语言")}, {u"hint"_s, tr("例如 zh、en、ja")}, {u"value"_s, u"zh"_s}},
            QVariantMap {{u"key"_s, u"cache"_s}, {u"type"_s, u"folder"_s}, {u"title"_s, tr("缓存文件夹")}, {u"hint"_s, QString()}, {u"value"_s, u"D:\\翻译缓存"_s}}};
        c.hasReadme = true;
        cards.push_back(c);
    }
    {
        PluginListModel::Card c;
        c.id = u"someone.web"_s;
        c.name = tr("网页快捷方式");
        c.version = u"2.0.0"_s;
        c.author = u"someone"_s;
        c.description = tr("零代码：gh 搜 GitHub，bd 搜百度");
        c.icon = glyph(u"U+E774"_s);
        c.enabled = true;
        c.status = tr("就绪");
        c.tone = u"ok"_s;
        c.keywords = {keyword(u"gh"_s, u"GitHub"_s, u"gh"_s, u"gh"_s, tr("关键字 %1 已被%2占用，改一个").arg(u"gh"_s, tr("网页搜索“%1”").arg(u"GitHub"_s)), false),
            keyword(u"bd"_s, tr("百度"), u"baidu"_s, u"bd"_s, {}, true)};
        c.permissions = {zeroCode};
        cards.push_back(c);
    }
    {
        PluginListModel::Card c;
        c.id = u"someone.bookmarks"_s;
        c.name = tr("浏览器书签");
        c.version = u"1.0.0"_s;
        c.author = u"someone"_s;
        c.description = tr("bm <词> 搜 Chrome 和 Edge 的书签");
        c.icon = glyph(u"U+E8A4"_s);
        c.enabled = true;
        c.error = tr("需要 Python 3：没有找到 python.exe");
        c.status = c.error;
        c.tone = u"warn"_s;
        c.keywords = {keyword(u"bm"_s, tr("书签"), u"bm"_s, u"bm"_s, {}, false)};
        c.permissions = {admin, permission(u"files:read"_s, tr("读这些文件：%1").arg(u"%LOCALAPPDATA%\\Google\\Chrome"_s), false)};
        cards.push_back(c);
    }
    {
        PluginListModel::Card c;
        c.id = u"winshun.tools"_s;
        c.name = tr("小工具");
        c.version = u"1.0.0"_s;
        c.author = u"LingCore"_s;
        c.bundled = true;
        c.description = tr("= 计算，# 颜色，ts 时间戳");
        c.icon = glyph(u"U+E8EF"_s);
        c.enabled = true;
        c.resident = true;
        c.error = tr("崩溃（退出码 %1）").arg(3);
        c.status = tr("连续崩溃，已停用：%1").arg(c.error);
        c.tone = u"error"_s;
        c.keywords = {keyword(u"calc"_s, tr("计算"), u"="_s, u"="_s, {}, false), keyword(u"ts"_s, tr("时间戳"), u"ts"_s, u"ts"_s, {}, false)};
        c.permissions = {strict, permission(u"clipboard:read"_s, tr("读剪贴板"), true)};
        c.hasReadme = true;
        c.aiExpose = true;
        c.aiEnabled = false;
        c.aiNote = tr("关着：AI 看不到这个插件的命令");
        cards.push_back(c);
    }
    {
        PluginListModel::Card c;
        c.id = u"broken.plugin"_s;
        c.name = u"broken.plugin"_s;
        c.usable = false;
        c.error = u"commands[0].keyword 含空格"_s;
        c.status = tr("无法使用：%1").arg(c.error);
        c.tone = u"error"_s;
        c.icon = glyph(u"U+EA86"_s);
        cards.push_back(c);
    }
    m_preview->cards = std::move(cards);
    m_editor->m_settings.pluginDeveloperMode = true; // the log pane and the templates on show
    emit developerModeChanged();
    m_restrictedNote = tr("插件看不到你在资源管理器里登录的网络位置：资源管理器以管理员身份运行或没有运行，插件只能用 Win顺 自己降权后的身份，它和资源管理器不在同一个登录会话里。");
    for (int i = 0; i < 6; ++i) {
        m_log.append({u"14:0%1:12.%2"_s.arg(i).arg(100 + i * 37), i % 2 ? u"lingcore.translate"_s : u"lingcore.kill"_s, i % 3 == 0 ? u"→"_s : i % 3 == 1 ? u"←"_s : u"stderr"_s,
            i % 3 == 0 ? u"query #%1"_s.arg(i) : i % 3 == 1 ? u"result #%1"_s.arg(i - 1) : u"Warning: slow"_s,
            i % 3 == 0 ? u"{\"jsonrpc\":\"2.0\",\"id\":%1,\"method\":\"query\",\"params\":{\"queryId\":17,\"command\":\"kill\",\"text\":\"note\"}}"_s.arg(i)
                       : u"{\"jsonrpc\":\"2.0\",\"id\":%1,\"result\":{\"items\":[{\"title\":\"notepad.exe\"}]}}"_s.arg(i - 1),
            i % 3 == 1 ? 12 + i : -1});
    }
    emit stateChanged();
    emit logPluginsChanged();
    rebuild();
    m_gallery.loadPreview();
}

} // namespace ws
