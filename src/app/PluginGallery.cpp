#include "PluginGallery.h"

#include "Catalog.h"
#include "FileIconProvider.h"
#include "Host.h"
#include "PluginSettings.h"
#include "Programs.h"
#include "Settings.h"
#include "platform/Http.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QPointer>
#include <QSaveFile>
#include <QStyleHints>
#include <QThreadPool>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace gallery = plugin::gallery;

namespace {

constexpr qint64 kStaleMs = 10 * 60 * 1000; // the tab opened again after this long: the list fetched again
constexpr int kListTimeoutMs = 15000; // each step's (see http::get)
constexpr int kPackageTimeoutMs = 30000;

// WinHTTP, as the update check has it: the system's proxy settings.
gallery::Fetcher webFetcher(int timeoutMs)
{
    return [timeoutMs](const QString& url, qint64 maxBytes, const gallery::Progress& progress) {
        http::Response r = http::get(url, {}, timeoutMs, maxBytes, http::Session::Own, progress);
        return gallery::Fetched {r.status, std::move(r.body), r.error};
    };
}

bool darkMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

// What a plugin may do that matters most: go online, take connections from
// the network, write files, start programs.
bool risky(plugin::Permission p)
{
    return p == plugin::Permission::Network || p == plugin::Permission::NetworkListen || p == plugin::Permission::FilesWrite
        || p == plugin::Permission::Processes;
}

// Every word typed is somewhere in the row.
bool fits(const QString& words, const QString& filter)
{
    const QStringList typed = filter.toLower().split(u' ', Qt::SkipEmptyParts);
    return std::ranges::all_of(typed, [&](const QString& word) { return words.contains(word); });
}

} // namespace

// ---- the rows -------------------------------------------------------------------

int PluginGalleryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant PluginGalleryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
        return {};
    const Row& r = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole: return r.id;
    case NameRole: return r.name;
    case DescriptionRole: return r.description;
    case AuthorRole: return r.author;
    case VersionRole: return r.version;
    case IconRole: return r.icon;
    case KeywordsRole: return r.keywords;
    case PermissionsRole: return r.permissions;
    case NeedsRole: return r.needs;
    case NeedsMissingRole: return r.needsMissing;
    case StageRole: return r.stage;
    case InstalledVersionRole: return r.installedVersion;
    case MinWinShunRole: return r.minWinShun;
    case BusyRole: return r.busy;
    case ProgressRole: return r.progress;
    case ProblemRole: return r.problem;
    case ShownRole: return r.shown;
    }
    return {};
}

QHash<int, QByteArray> PluginGalleryModel::roleNames() const
{
    // Not "state" (an Item's own) nor "enabled": the delegates' required properties take these names.
    return {{IdRole, "pluginId"}, {NameRole, "name"}, {DescriptionRole, "description"}, {AuthorRole, "author"}, {VersionRole, "version"},
        {IconRole, "icon"}, {KeywordsRole, "keywords"}, {PermissionsRole, "permissions"}, {NeedsRole, "needs"}, {NeedsMissingRole, "needsMissing"},
        {StageRole, "stage"}, {InstalledVersionRole, "installedVersion"}, {MinWinShunRole, "minWinShun"}, {BusyRole, "busy"},
        {ProgressRole, "progress"}, {ProblemRole, "problem"}, {ShownRole, "shown"}};
}

void PluginGalleryModel::setRows(std::vector<Row> rows)
{
    const bool sameIds = rows.size() == m_rows.size()
        && std::equal(rows.begin(), rows.end(), m_rows.begin(), [](const Row& a, const Row& b) { return a.id == b.id; });
    if (!sameIds) {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
        return;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        Row& old = m_rows[i];
        Row& now = rows[i];
        if (old == now)
            continue;
        QList<int> roles;
        const auto differs = [&](bool changed, Role role) {
            if (changed)
                roles.push_back(role);
        };
        differs(old.name != now.name, NameRole);
        differs(old.description != now.description, DescriptionRole);
        differs(old.author != now.author, AuthorRole);
        differs(old.version != now.version, VersionRole);
        differs(old.icon != now.icon, IconRole);
        differs(old.keywords != now.keywords, KeywordsRole);
        differs(old.permissions != now.permissions, PermissionsRole);
        differs(old.needs != now.needs, NeedsRole);
        differs(old.needsMissing != now.needsMissing, NeedsMissingRole);
        differs(old.stage != now.stage, StageRole);
        differs(old.installedVersion != now.installedVersion, InstalledVersionRole);
        differs(old.minWinShun != now.minWinShun, MinWinShunRole);
        differs(old.busy != now.busy, BusyRole);
        differs(old.progress != now.progress, ProgressRole);
        differs(old.problem != now.problem, ProblemRole);
        differs(old.shown != now.shown, ShownRole);
        old = std::move(now);
        const QModelIndex at = index(static_cast<int>(i));
        if (!roles.isEmpty())
            emit dataChanged(at, at, roles);
    }
}

void PluginGalleryModel::setProgress(const QString& id, double progress)
{
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id != id || m_rows[i].progress == progress)
            continue;
        m_rows[i].progress = progress;
        const QModelIndex at = index(static_cast<int>(i));
        emit dataChanged(at, at, {ProgressRole});
    }
}

// ---- the tab's view-model ---------------------------------------------------------

struct PluginGallery::Download {
    int status = 0;
    unsigned long error = 0;
    gallery::Problem check = gallery::Problem::None;
    QString path; // the package, checked, in a temporary file
    QString saveError;
};

PluginGallery::PluginGallery(PluginSettings* settings)
    : QObject(settings)
    , m_settings(settings)
{
}

PluginGallery::~PluginGallery()
{
    if (m_cancel)
        m_cancel->store(true); // its result, should it still come, is let go of (its file removed)
}

QString PluginGallery::source() const
{
    const QString given = qEnvironmentVariable("WINSHUN_PLUGIN_INDEX");
    return given.isEmpty() ? QString(gallery::kIndexUrl) : given;
}

bool PluginGallery::keeping() const
{
    return !m_preview && source() == gallery::kIndexUrl;
}

QString PluginGallery::cachePath() const
{
    return Settings::dataDir() + u"\\plugin-index.json"_s;
}

int PluginGallery::shownCount() const
{
    return static_cast<int>(std::ranges::count_if(m_list.rows(), [](const PluginGalleryModel::Row& r) { return r.shown; }));
}

void PluginGallery::setFilter(const QString& filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    emit filterChanged();
    rebuild();
}

void PluginGallery::setActive(bool on)
{
    if (m_active == on)
        return;
    m_active = on;
    emit activeChanged();
    if (!on || m_preview)
        return;
    if (!m_cacheRead)
        readCache();
    if (!m_fetchedThisRun.isValid() || m_fetchedThisRun.hasExpired(kStaleMs))
        refresh();
}

QString PluginGallery::status() const
{
    const int n = count();
    if (m_fetching)
        return n > 0 ? tr("%n 个插件；正在获取最新的列表…", "", n) : tr("正在获取插件列表…");
    if (!m_hasIndex)
        return {}; // `problem` says why
    if (m_fetchedThisRun.isValid() || !m_listTime.isValid())
        return tr("%n 个插件", "", n);
    const QLocale locale(m_settings->m_locale.startsWith(u"en"_s) ? QLocale::English : QLocale::Chinese);
    return tr("%n 个插件；列表是 %1 获取的", "", n).arg(locale.toString(m_listTime, QLocale::ShortFormat));
}

void PluginGallery::readCache()
{
    m_cacheRead = true;
    if (!keeping() || m_hasIndex)
        return;
    QFile file(cachePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    std::optional<gallery::Index> index = gallery::parse(file.read(gallery::kMaxIndexBytes), source(), nullptr);
    if (!index)
        return;
    m_index = std::move(*index);
    m_hasIndex = true;
    m_listTime = QFileInfo(file).lastModified();
    rebuild();
    emit stateChanged();
}

void PluginGallery::refresh()
{
    if (m_preview || m_fetching)
        return;
    m_fetching = true;
    m_problem.clear();
    m_problemIsConnection = false;
    emit stateChanged();
    const QString from = source();
    QThreadPool::globalInstance()->start([self = QPointer(this), from] {
        gallery::Fetched fetched = gallery::fetch(from, gallery::kMaxIndexBytes + 1, webFetcher(kListTimeoutMs));
        QMetaObject::invokeMethod(qApp, [self, from, fetched = std::move(fetched)] {
            if (self)
                self->finishFetch(from, fetched);
        }, Qt::QueuedConnection);
    });
}

void PluginGallery::finishFetch(const QString& from, const gallery::Fetched& fetched)
{
    m_fetching = false;
    QString error;
    std::optional<gallery::Index> index;
    if (fetched.status == 200 && fetched.body.size() <= gallery::kMaxIndexBytes)
        index = gallery::parse(fetched.body, from, &error);
    if (!index) {
        m_problem = fetched.status == 200 ? tr("没看懂插件列表，过一会儿再试。") : networkProblem(fetched);
        m_problemIsConnection = connectionFailed(fetched);
        qWarning().noquote() << "Plugin library: no list from" << from << fetched.status << fetched.error << error;
        emit stateChanged();
        return;
    }
    m_fetchedThisRun.start();
    m_problem.clear();
    m_problemIsConnection = false;
    m_index = std::move(*index);
    m_hasIndex = true;
    m_listTime = QDateTime::currentDateTime();
    if (keeping()) { // shown at once next time, and when GitHub cannot be reached
        QDir().mkpath(Settings::dataDir());
        QSaveFile file(cachePath());
        if (!file.open(QIODevice::WriteOnly) || file.write(fetched.body) != fetched.body.size() || !file.commit())
            qWarning().noquote() << "Plugin library: the list could not be kept:" << file.errorString();
    }
    rebuild();
    emit stateChanged();
}

void PluginGallery::rebuild()
{
    std::vector<PluginGalleryModel::Row> rows;
    if (m_preview) {
        rows = m_previewRows;
    } else {
        plugin::Host* host = m_settings->m_host;
        const QString& locale = m_settings->m_locale;
        const QString bundledDir = host ? host->options().bundledPluginDir : QString();
        const QString appVersion = QStringLiteral(WINSHUN_VERSION);
        const bool dark = darkMode();
        for (const gallery::Item& item : m_index.items) {
            PluginGalleryModel::Row r;
            r.id = item.id;
            r.name = item.name.pick(locale);
            r.description = item.description.pick(locale);
            r.author = item.author;
            r.version = item.version;
            r.keywords = item.keywords;
            r.minWinShun = item.minWinShun;
            const gallery::Local local = host ? gallery::localOf(host->catalog(), bundledDir, item.id) : gallery::Local {};
            r.installedVersion = local.userVersion;
            switch (gallery::stateOf(item, local, appVersion)) {
            case gallery::State::Install: r.stage = u"install"_s; break;
            case gallery::State::Update: r.stage = u"update"_s; break;
            case gallery::State::Installed: r.stage = u"installed"_s; break;
            case gallery::State::Bundled: r.stage = u"bundled"_s; break;
            case gallery::State::NeedsNewerApp: r.stage = u"app"_s; break;
            }
            // The icon of the copy here, if any (the list has none).
            const PluginListModel::Card* card = m_settings->m_list.card(item.id);
            r.icon = card && !card->icon.isEmpty() ? card->icon : FileIconProvider::pluginIconUrl(item.id, u"glyph:U+EA86"_s, dark);
            // Named as the cards and the permission dialog name them.
            for (const QString& name : item.permissions) {
                const std::optional<plugin::Permission> p = plugin::permissionFromName(name);
                r.permissions.push_back(QVariantMap {{u"text"_s, p ? PluginSettings::permissionText(*p) : name}, {u"risky"_s, p && risky(*p)}});
            }
            if (item.program) {
                switch (*item.program) {
                case plugin::Program::Python: r.needs = tr("需要 Python 3"); break;
                case plugin::Program::Pwsh: r.needs = tr("需要 PowerShell 7"); break;
                case plugin::Program::Node: r.needs = tr("需要 Node.js"); break;
                case plugin::Program::PowerShell: // comes with Windows
                case plugin::Program::Exe: break; // its own program, in the package
                }
                r.needsMissing = !r.needs.isEmpty() && plugin::programs::find(*item.program).isEmpty();
            }
            r.busy = item.id == m_downloadId;
            r.progress = r.busy ? m_progress : 0;
            r.problem = m_problems.value(item.id);
            r.words = QStringList {item.id, item.name.zh, item.name.en, item.description.zh, item.description.en, item.keywords.join(u' ')}
                          .join(u'\n')
                          .toLower();
            rows.push_back(std::move(r));
        }
    }
    for (PluginGalleryModel::Row& r : rows)
        r.shown = fits(r.words, m_filter);
    m_list.setRows(std::move(rows));
    emit listChanged();
}

void PluginGallery::setRowProgress(const QString& id, double progress)
{
    if (id != m_downloadId || progress <= m_progress)
        return;
    m_progress = progress;
    m_list.setProgress(id, progress); // that row alone, not the whole list again
}

void PluginGallery::install(const QString& id)
{
    if (m_preview || !m_downloadId.isEmpty() || !m_settings->m_host)
        return;
    const gallery::Item* found = m_index.find(id);
    if (!found)
        return;
    const auto& rows = m_list.rows();
    const auto row = std::ranges::find_if(rows, [&](const PluginGalleryModel::Row& r) { return r.id == id; });
    if (row == rows.end() || (row->stage != u"install" && row->stage != u"update"))
        return;
    m_problems.remove(id);
    if (found->size > gallery::kMaxPackageBytes) {
        m_problems.insert(id, tr("这个插件包太大（%1 MB），没有下载。").arg(found->size >> 20));
        rebuild();
        return;
    }
    m_downloadId = id;
    m_progress = 0;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    const quint64 generation = ++m_generation;
    rebuild();
    emit stateChanged();
    QThreadPool::globalInstance()->start([self = QPointer(this), item = *found, cancel, generation] {
        // The progress told now and then: a piece of the body each time would flood the page.
        QElapsedTimer sinceTold;
        sinceTold.start();
        const gallery::Progress progress = [&](qint64 received) {
            if (cancel->load())
                return false;
            if (sinceTold.elapsed() >= 50 || received >= item.size) {
                sinceTold.restart();
                const double part = std::min(1.0, static_cast<double>(received) / static_cast<double>(item.size));
                QMetaObject::invokeMethod(qApp, [self, id = item.id, generation, part] {
                    if (self && self->m_generation == generation)
                        self->setRowProgress(id, part);
                }, Qt::QueuedConnection);
            }
            return true;
        };
        // One byte more than the list says: a bigger package shows as one.
        gallery::Fetched fetched = gallery::fetch(item.url, item.size + 1, webFetcher(kPackageTimeoutMs), progress);
        Download d;
        d.status = fetched.status;
        d.error = fetched.error;
        if (fetched.status == 200 && !cancel->load()) {
            d.check = gallery::check(item, fetched.body);
            if (d.check == gallery::Problem::None)
                d.path = gallery::saveTemporary(item, fetched.body, &d.saveError);
        }
        QMetaObject::invokeMethod(qApp, [self, d, id = item.id, generation, cancel] {
            if (!self || cancel->load() || self->m_generation != generation) {
                if (!d.path.isEmpty())
                    QFile::remove(d.path);
                return;
            }
            self->finishDownload(id, d);
        }, Qt::QueuedConnection);
    });
}

void PluginGallery::cancel(const QString& id)
{
    if (id.isEmpty() || id != m_downloadId)
        return;
    if (m_cancel)
        m_cancel->store(true);
    m_cancel.reset();
    m_downloadId.clear();
    m_progress = 0;
    ++m_generation;
    rebuild();
    emit stateChanged();
}

void PluginGallery::finishDownload(const QString& id, const Download& d)
{
    m_downloadId.clear();
    m_cancel.reset();
    m_progress = 0;
    if (d.path.isEmpty()) {
        QString problem;
        if (d.status != 200)
            problem = networkProblem(gallery::Fetched {d.status, {}, d.error});
        else if (d.check != gallery::Problem::None)
            problem = tr("下载的文件校验不对，没有安装。再试一次；还不行就过一会儿再试。");
        else
            problem = tr("下载的文件存不下：%1").arg(d.saveError);
        qWarning().noquote() << "Plugin library: not installed:" << id << d.status << d.error << static_cast<int>(d.check) << d.saveError;
        m_problems.insert(id, problem);
        rebuild();
        emit stateChanged();
        return;
    }
    rebuild();
    emit stateChanged();
    // The page's own install: its questions, what it says when done, and
    // the plugin off until it is turned on. It has read the package by the
    // time it returns (an archive is unpacked into a folder of its own,
    // waiting there for a question's answer), so the file can go. It may
    // run events meanwhile (a plugin's program let go of): nothing of the
    // rows is held across it.
    const gallery::Item* item = m_index.find(id);
    m_settings->installFrom(d.path, id, item ? item->name.pick(m_settings->m_locale) : id);
    QFile::remove(d.path);
}

QString PluginGallery::networkProblem(const gallery::Fetched& fetched) const
{
    if (fetched.status == 403 || fetched.status == 429)
        return tr("GitHub 暂时不让下载（访问太频繁），过一会儿再试。");
    if (fetched.status == 404)
        return tr("GitHub 上没有这个文件了，可能刚更新过：点“刷新”再试。");
    if (fetched.status != 0)
        return tr("GitHub 返回了错误（%1）。").arg(fetched.status);
    // The proxy that was set by hand, when the way to it is what failed (GitHub #84).
    if (const QString proxy = http::proxyServer(); !proxy.isEmpty() && connectionFailed(fetched))
        return tr("连不上你设置的代理 %1。看看代理软件开着没有，或者到“网络代理”里改一下。").arg(proxy);
    switch (fetched.error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return tr("找不到 GitHub。网络不通，或者要设置网络代理才能访问。");
    case ERROR_WINHTTP_TIMEOUT:
        return tr("连不上 GitHub，等了太久也没有回应。网络没问题的话，要设置网络代理才能访问。");
    case ERROR_FILE_NOT_FOUND:
    case ERROR_ACCESS_DENIED:
    case ERROR_READ_FAULT: // a list or package of a file (WINSHUN_PLUGIN_INDEX)
        return tr("读不了这个文件（错误 %1）。").arg(fetched.error);
    default:
        return tr("连不上 GitHub。网络没问题的话，要设置网络代理才能访问。");
    }
}

bool PluginGallery::connectionFailed(const gallery::Fetched& fetched)
{
    // No answer at all, and not for want of the file (a list or package of a file).
    return fetched.status == 0 && fetched.error != ERROR_FILE_NOT_FOUND && fetched.error != ERROR_ACCESS_DENIED
        && fetched.error != ERROR_READ_FAULT && fetched.error != ERROR_CANCELLED;
}

void PluginGallery::openRepository()
{
    shell::openUrl(QString(gallery::kRepositoryUrl));
}

void PluginGallery::loadPreview()
{
    m_preview = true;
    m_hasIndex = true;
    m_fetchedThisRun.start();
    const bool dark = darkMode();
    const auto glyph = [&](const QString& code) { return FileIconProvider::pluginIconUrl(u"preview"_s, u"glyph:"_s + code, dark); };
    const auto permission = [](plugin::Permission p) { return QVariantMap {{u"text"_s, PluginSettings::permissionText(p)}, {u"risky"_s, risky(p)}}; };
    const auto row = [&](const QString& id, const QString& name, const QString& description, const QString& version, const QString& stage) {
        PluginGalleryModel::Row r;
        r.id = id;
        r.name = name;
        r.description = description;
        r.author = u"LingCore"_s;
        r.version = version;
        r.stage = stage;
        r.icon = glyph(u"U+EA86"_s);
        r.words = (id + u'\n' + name + u'\n' + description).toLower();
        return r;
    };
    std::vector<PluginGalleryModel::Row> rows;
    {
        PluginGalleryModel::Row r = row(u"lingcore.translate"_s, tr("翻译"), tr("fy 词：中英互译，Enter 复制译文"), u"1.0.0"_s, u"installed"_s);
        r.icon = glyph(u"U+E8C1"_s);
        r.installedVersion = u"1.0.0"_s;
        r.keywords = {u"fy"_s};
        r.permissions = {permission(plugin::Permission::Network)};
        r.needs = tr("需要 Python 3");
        rows.push_back(r);
    }
    {
        PluginGalleryModel::Row r = row(u"lingcore.kill"_s, tr("结束进程"), tr("kill <进程名或 PID> 列出进程，Enter 结束，Ctrl+Enter 连同子进程"), u"1.2.0"_s, u"update"_s);
        r.icon = glyph(u"U+E711"_s);
        r.installedVersion = u"1.1.0"_s;
        r.keywords = {u"kill"_s, u"jc"_s};
        r.permissions = {permission(plugin::Permission::Processes)};
        rows.push_back(r);
    }
    {
        PluginGalleryModel::Row r = row(u"winshun.bookmarks"_s, tr("浏览器书签"), tr("bm 词：搜 Chrome 和 Edge 的书签，Enter 打开"), u"1.1.0"_s, u"bundled"_s);
        r.icon = glyph(u"U+E728"_s);
        r.keywords = {u"bm"_s, u"sq"_s};
        r.permissions = {permission(plugin::Permission::FilesRead)};
        r.needs = tr("需要 Python 3");
        rows.push_back(r);
    }
    {
        PluginGalleryModel::Row r = row(u"lingcore.clips"_s, tr("剪贴板存档"), tr("cb 词：把剪贴板记录存成每天一个文本文件"), u"0.3.0"_s, u"install"_s);
        r.keywords = {u"cb"_s};
        r.permissions = {permission(plugin::Permission::ClipboardRead), permission(plugin::Permission::FilesWrite)};
        r.needs = tr("需要 Node.js");
        r.needsMissing = true;
        rows.push_back(r);
    }
    {
        PluginGalleryModel::Row r = row(u"lingcore.weather"_s, tr("天气"), tr("tq 词：今天和明天的天气"), u"1.0.2"_s, u"install"_s);
        r.icon = glyph(u"U+E753"_s);
        r.keywords = {u"tq"_s};
        r.permissions = {permission(plugin::Permission::Network)};
        r.needs = tr("需要 Python 3");
        r.busy = true;
        r.progress = 0.42;
        rows.push_back(r);
    }
    {
        PluginGalleryModel::Row r = row(u"lingcore.timer"_s, tr("倒计时"), tr("ds 5m：五分钟后提醒"), u"2.0.0"_s, u"install"_s);
        r.icon = glyph(u"U+E916"_s);
        r.keywords = {u"ds"_s};
        r.problem = tr("下载的文件校验不对，没有安装。再试一次；还不行就过一会儿再试。");
        rows.push_back(r);
    }
    {
        PluginGalleryModel::Row r = row(u"lingcore.notes"_s, tr("便签"), tr("bq 词：记一句话，再搜出来"), u"1.0.0"_s, u"app"_s);
        r.icon = glyph(u"U+E70B"_s);
        r.keywords = {u"bq"_s};
        r.minWinShun = u"0.14.0"_s;
        rows.push_back(r);
    }
    m_previewRows = std::move(rows);
    rebuild();
    emit stateChanged();
}

} // namespace ws
