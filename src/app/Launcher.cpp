#include "Launcher.h"

#include "AppCatalog.h"
#include "History.h"
#include "IndexService.h"
#include "Query.h"
#include "SearchEngine.h"
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QPointer>
#include <QScreen>
#include <QStyleHints>

#include <windows.h>

#include <algorithm>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace qf {

namespace {

QString number(qint64 n)
{
    return QLocale().toString(n);
}

} // namespace

Launcher::Launcher(IndexService* index, AppCatalog* apps, SearchEngine* engine, History* history, QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_apps(apps)
    , m_engine(engine)
    , m_history(history)
{
    m_contentDebounce.setSingleShot(true);
    m_contentDebounce.setInterval(300ms); // content search reads files: wait for a pause in typing
    connect(&m_contentDebounce, &QTimer::timeout, this, &Launcher::startContentSearch);

    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(2s);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        m_flash.clear();
        emit statusChanged();
    });

    m_statusPoll.setInterval(500ms); // live item count while the index is being built
    connect(&m_statusPoll, &QTimer::timeout, this, [this] {
        if (m_index->state() != IndexService::State::Ready || m_index->isRefreshing())
            refreshStatus();
    });

    connect(m_engine, &SearchEngine::resultsReady, this, &Launcher::onResults);
    connect(m_engine, &SearchEngine::contentResults, this, &Launcher::onContentResults);
    connect(m_engine, &SearchEngine::contentProgress, this, &Launcher::onContentProgress);
    connect(m_index, &IndexService::stateChanged, this, [this] {
        // A finished index can only improve the current results.
        if (m_index->state() == IndexService::State::Ready && m_window && m_window->isVisible() && m_scope != Content)
            search();
        refreshStatus();
    });
    connect(m_apps, &AppCatalog::changed, this, [this] {
        // Apps installed or removed (or the first list arrived): 全部 and 应用 show them.
        if (m_window && m_window->isVisible() && (m_scope == All || m_scope == Apps))
            search();
        refreshStatus();
    });
    refreshStatus();
}

void Launcher::setQuery(const QString& query)
{
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    search();
}

void Launcher::setScope(Scope scope)
{
    if (scope == m_scope)
        return;
    m_scope = scope;
    emit scopeChanged();
    // Keep the old rows until the new results replace them: clearing first
    // collapses the window and then grows it back (flicker).
    search();
}

bool Launcher::busy() const
{
    const auto state = m_index->state();
    return state == IndexService::State::Loading || state == IndexService::State::Building
        || (m_contentRunning && !m_quiet);
}

QString Launcher::contentFilesLabel() const
{
    constexpr qsizetype kShown = 3; // the full default list would overflow the search box
    QStringList parts;
    for (const QString& ext : m_contentExtensions.first(std::min(kShown, m_contentExtensions.size())))
        parts.append(u'.' + ext);
    return parts.join(u" / "_s) + (m_contentExtensions.size() > kShown ? u" 等文件"_s : u" 文件"_s);
}

QString Launcher::placeholder() const
{
    switch (m_scope) {
    case Apps:
        return u"搜索已安装的应用"_s;
    case Files:
        return u"搜索文件"_s;
    case Folders:
        return u"搜索文件夹"_s;
    case Content:
        return u"搜索 %1中的文字"_s.arg(contentFilesLabel());
    default:
        return u"搜索应用、文件和文件内容"_s;
    }
}

void Launcher::setContentOptions(QStringList extensions, qint64 maxFileBytes)
{
    for (QString& ext : extensions) {
        ext = ext.trimmed();
        while (ext.startsWith(u'.') || ext.startsWith(u'*'))
            ext.remove(0, 1);
    }
    extensions.removeAll(QString());
    if (extensions.isEmpty())
        extensions = {u"txt"_s};
    m_contentExtensions = extensions;
    m_maxContentBytes = maxFileBytes;
    emit scopeChanged(); // placeholder text mentions the extensions
}

void Launcher::handleShown()
{
    m_statusPoll.start();
    m_apps->refresh(); // reads the list again only if apps were installed or removed
    search(); // refresh: files may have changed, and "recent" certainly has
    emit shown();
}

void Launcher::handleHidden()
{
    m_statusPoll.stop();
    m_contentDebounce.stop();
    if (m_contentRunning || m_pending) {
        m_engine->cancel();
        m_requestId = 0;
        m_pending = false;
        m_contentRunning = false;
    }
}

// Nothing is cleared up front: the rows on screen stay until results replace
// them. Searching what is already shown again (the window reopens, the index
// changes) is a refresh: the rows are updated in place, and content matches
// from the last scan stay until the new scan has had a chance to find them.
void Launcher::search()
{
    m_flash.clear();
    m_contentDebounce.stop();
    const QString text = m_query.trimmed();
    m_refresh = m_haveResults && !m_replacePending && text == m_shownText && m_scope == m_shownScope;
    m_replacePending = !m_refresh;
    m_withContent = m_scope == All && !contentNeedle().isEmpty();
    m_unconfirmed.clear();
    // Quietly, unless the rows came from a content scan that was cut short
    // (window hidden mid-scan): that one still shows as the search it is.
    const bool scans = m_scope == Content || m_withContent;
    m_quiet = m_refresh && (!scans || m_scanComplete);
    if (!m_refresh)
        m_scanComplete = false;
    if (!m_quiet) { // a quiet refresh keeps showing the last scan's figures
        m_contentScanned = 0;
        m_contentTotal = 0;
    }

    if (m_scope == Content) {
        m_engine->cancel();
        m_requestId = 0;
        m_pending = false;
        m_contentRunning = false;
        if (text.isEmpty()) {
            m_replacePending = false;
            m_results.clear();
            m_haveResults = false;
            emit resultsReplaced();
        } else if (m_refresh) {
            for (int row = 0; row < m_results.count(); ++row)
                m_unconfirmed.insert(m_results.at(row)->path);
            startContentSearch(); // at once: nobody is typing
        } else {
            m_contentDebounce.start();
        }
        refreshStatus();
        return;
    }

    SearchEngine::Request request;
    request.text = m_query;
    request.scope = static_cast<qf::Scope>(m_scope);
    if (m_scope == Apps)
        request.limit = 5000; // with nothing typed, 应用 lists every app
    request.history = m_history->items();
    m_requestId = m_engine->submit(std::move(request)); // also stops a running content scan
    m_pending = true;
    m_contentRunning = false;
    refreshStatus();
}

// Rows for the current query and scope. Replacing another query's rows
// moves the selection back to the first one; a refresh keeps it.
void Launcher::showRows(SearchResults rows, QStringList highlights)
{
    m_results.assign(std::move(rows), std::move(highlights));
    m_shownText = m_query.trimmed();
    m_shownScope = m_scope;
    m_haveResults = true;
    if (m_replacePending) {
        m_replacePending = false;
        emit resultsReplaced();
    }
}


// 全部 looks for the typed text inside files too, as one phrase (quotes
// dropped). Not for name syntax: wildcards, ext: and !exclusions only make
// sense for names.
QString Launcher::contentNeedle() const
{
    const ParsedQuery query = qf::parseQuery(m_query);
    if (query.terms.empty() || !query.extensions.empty())
        return {};
    if (std::any_of(query.terms.cbegin(), query.terms.cend(),
            [](const QueryTerm& t) { return t.negated || t.wildcard; }))
        return {};
    return QString(m_query).remove(u'"').trimmed();
}

void Launcher::startContentSearch()
{
    SearchEngine::Request request;
    request.text = m_scope == Content ? m_query.trimmed() : contentNeedle();
    request.scope = qf::Scope::Content;
    request.contentExtensions = m_contentExtensions;
    request.maxContentFileBytes = m_maxContentBytes;
    m_requestId = m_engine->submit(std::move(request));
    m_pending = true;
    m_contentRunning = true;
    refreshStatus();
}

void Launcher::onResults(quint64 id, const SearchResults& results, qint64 total, qint64 elapsedUs)
{
    if (id != m_requestId)
        return; // superseded while in flight
    m_pending = false;
    if (m_contentRunning) {
        refreshStatus();
        return; // the (empty) start of a content scan: rows come with contentResults
    }

    SearchResults rows = results;
    if (m_refresh && m_withContent) {
        // Keep last scan's content matches below the names until the new
        // scan finds them again (or finishes without them), rather than
        // dropping them only to add them back a moment later.
        QSet<QString> names;
        for (const SearchResult& r : rows)
            names.insert(r.path);
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r->line > 0 && !names.contains(r->path)) {
                rows.append(*r);
                m_unconfirmed.insert(r->path);
            }
        }
    }
    m_contentHits = static_cast<int>(m_unconfirmed.size());
    m_totalMatches = total;
    if (!m_refresh)
        m_elapsedUs = elapsedUs; // a refresh keeps the figure the user has seen
    // Content search starts only now: a new request cancels the running one,
    // and the name search must finish first. Before the rows change, so the
    // view knows more are on the way and does not shrink in between.
    if (m_withContent) {
        if (m_refresh)
            startContentSearch();
        else
            m_contentDebounce.start();
    }
    showRows(std::move(rows), qf::parseQuery(m_query).highlights);
    refreshStatus();
    emit namesShown();
}

void Launcher::onContentResults(quint64 id, const SearchResults& results)
{
    if (id != m_requestId)
        return;
    if (m_replacePending) { // 内容, new query: its first matches replace the previous query's rows
        showRows(results, {});
        refreshStatus();
        return;
    }
    SearchResults fresh;
    for (const SearchResult& r : results) {
        const int row = m_results.indexOf(r.path);
        if (row < 0)
            fresh.append(r);
        else if (m_unconfirmed.remove(r.path))
            m_results.update(row, r); // kept from the last scan and found again; the line may have moved
        // else a file whose name matched already has a row
    }
    if (m_scope != Content)
        m_contentHits += static_cast<int>(fresh.size());
    m_results.append(fresh);
}

void Launcher::onContentProgress(quint64 id, int scanned, int total, bool finished)
{
    if (id != m_requestId)
        return;
    m_contentRunning = !finished;
    if (!finished && m_quiet)
        return; // the status keeps the last scan's figures
    m_contentScanned = scanned;
    m_contentTotal = total;
    if (finished) {
        m_scanComplete = true;
        if (m_replacePending)
            showRows({}, {}); // 内容, new query: nothing found
        if (!m_unconfirmed.isEmpty()) { // kept from the last scan, but gone now
            const int removed = m_results.removeAll(m_unconfirmed);
            if (m_scope != Content)
                m_contentHits -= removed;
            m_unconfirmed.clear();
        }
    }
    refreshStatus();
}

void Launcher::refreshStatus()
{
    const auto state = m_index->state();
    const QString items = number(static_cast<qint64>(m_index->itemCount()));
    // Waiting for a pause in typing counts too: "0 个结果 · 9 毫秒" would read as final.
    const bool contentPending = !m_quiet && (m_contentRunning || m_contentDebounce.isActive());
    QString s;
    if (m_scope == Apps) {
        const qint64 ms = m_elapsedUs / 1000;
        if (!m_apps->isLoaded())
            s = u"正在读取已安装的应用…"_s;
        else if (m_query.trimmed().isEmpty() || !m_haveResults)
            s = u"已安装 %1 个应用"_s.arg(number(static_cast<qint64>(m_apps->apps()->size())));
        else
            s = u"%1 个应用 · %2 毫秒"_s.arg(number(m_totalMatches), ms < 1 ? u"<1"_s : number(ms));
    } else if (m_query.trimmed().isEmpty() || (!m_haveResults && !m_contentRunning)) {
        switch (state) {
        case IndexService::State::Idle:
        case IndexService::State::Loading:
            s = u"正在加载索引…"_s;
            break;
        case IndexService::State::Building:
            s = u"正在建立索引… 已收录 %1 项"_s.arg(items);
            break;
        case IndexService::State::Ready:
            s = u"已索引 %1 个文件和文件夹"_s.arg(items);
            if (m_apps->isLoaded())
                s += u" · %1 个应用"_s.arg(number(static_cast<qint64>(m_apps->apps()->size())));
            if (m_index->isRefreshing())
                s += u" · 后台同步中"_s;
            break;
        }
    } else if (m_scope == Content) {
        if (contentPending)
            s = m_contentTotal > 0 ? u"正在搜索 %1内容… %2 / %3"_s.arg(
                    contentFilesLabel(), number(m_contentScanned), number(m_contentTotal))
                                   : u"正在搜索 %1内容…"_s.arg(contentFilesLabel());
        else if (m_contentScanned < m_contentTotal && m_results.count() > 0)
            s = u"已显示前 %1 个包含该文字的文件（共 %2 个 %3）"_s.arg(
                number(m_results.count()), number(m_contentTotal), contentFilesLabel());
        else
            s = u"%1 个文件包含该文字 · 共查找 %2 个 %3"_s.arg(
                number(m_results.count()), number(m_contentTotal), contentFilesLabel());
    } else {
        const qint64 ms = m_elapsedUs / 1000;
        const QString results = number(m_totalMatches + m_contentHits);
        if (contentPending)
            s = m_contentTotal > 0
                ? u"%1 个结果 · 正在搜索内容… %2 / %3"_s.arg(results, number(m_contentScanned), number(m_contentTotal))
                : u"%1 个结果 · 正在搜索内容…"_s.arg(results);
        else if (m_contentHits > 0)
            s = u"%1 个结果 · 其中 %2 个是文件内容匹配"_s.arg(results, number(m_contentHits));
        else
            s = u"%1 个结果 · %2 毫秒"_s.arg(results, ms < 1 ? u"<1"_s : number(ms));
        if (state == IndexService::State::Building)
            s += u" · 索引尚未建完"_s;
    }
    if (s != m_status) {
        m_status = s;
        emit statusChanged();
    } else {
        emit statusChanged(); // busy/searching may have changed
    }
}

void Launcher::flash(const QString& message)
{
    m_flash = message;
    m_flashTimer.start();
    emit statusChanged();
}

void Launcher::cycleScope(int delta)
{
    constexpr int kScopes = Content + 1;
    setScope(static_cast<Scope>(((m_scope + delta) % kScopes + kScopes) % kScopes));
}

void Launcher::dismiss()
{
    emit dismissRequested();
}

void Launcher::trigger(int row, int action)
{
    if (const SearchResult* r = m_results.at(row))
        perform(*r, static_cast<Action>(action));
}

void Launcher::perform(const SearchResult& result, Action action)
{
    const SearchResult r = result; // the model may change underneath us
    if (r.isApp()) {
        performApp(r, action);
        return;
    }
    const bool needsFile = action == Open || action == Reveal || action == RunAsAdmin || action == Recycle;
    if (needsFile && !QFileInfo::exists(r.path)) {
        if (action == Recycle)
            m_results.remove(r.path);
        m_history->remove(r.path);
        flash(u"“%1” 已不存在"_s.arg(r.name));
        return;
    }
    switch (action) {
    case Open:
    case RunAsAdmin:
        m_history->record(r.path);
        shell::open(r.path, action == RunAsAdmin);
        emit dismissRequested();
        break;
    case Reveal:
        m_history->record(r.path);
        shell::reveal(r.path);
        emit dismissRequested();
        break;
    case CopyPath:
        shell::copyText(r.path);
        flash(u"已复制路径"_s);
        break;
    case CopyName:
        shell::copyText(r.name);
        flash(u"已复制名称"_s);
        break;
    case CopyItem:
        shell::copyFiles({r.path});
        flash(u"已复制，可在资源管理器中粘贴"_s);
        break;
    case Recycle: {
        const auto owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
        shell::recycle(r.path, owner, [self = QPointer(this), r](bool ok) {
            if (self)
                QMetaObject::invokeMethod(self, [self, r, ok] { self->onRecycled(r, ok); }, Qt::QueuedConnection);
        });
        break;
    }
    }
}

// An installed app opens through the shell (shell:AppsFolder\<id>), as the
// Start menu opens it; its "location" is its program or install folder.
void Launcher::performApp(const SearchResult& app, Action action)
{
    switch (action) {
    case Open:
    case RunAsAdmin:
        if (action == RunAsAdmin && !app.elevatable) {
            flash(u"“%1”不能以管理员身份运行"_s.arg(app.name));
            return;
        }
        m_history->record(app.path);
        shell::launchApp(app.path, action == RunAsAdmin);
        emit dismissRequested();
        break;
    case Reveal:
        if (app.target.isEmpty() || !QFileInfo::exists(app.target)) {
            flash(u"“%1”没有可以打开的位置"_s.arg(app.name));
            return;
        }
        m_history->record(app.path);
        if (app.isPackagedApp())
            shell::open(app.target); // its install folder; the folder above it is not readable
        else
            shell::reveal(app.target);
        emit dismissRequested();
        break;
    case CopyItem:
        if (app.hasCopyableTarget() && QFileInfo::exists(app.target)) {
            shell::copyFiles({app.target});
            flash(u"已复制，可在资源管理器中粘贴"_s);
            break;
        }
        [[fallthrough]]; // nothing to paste: copy where it is instead
    case CopyPath:
        shell::copyText(app.target.isEmpty() ? app.path : app.target);
        flash(u"已复制路径"_s);
        break;
    case CopyName:
        shell::copyText(app.name);
        flash(u"已复制名称"_s);
        break;
    case Recycle:
        break; // apps are uninstalled in Windows Settings
    }
}

void Launcher::onRecycled(const SearchResult& result, bool ok)
{
    if (QFileInfo::exists(result.path)) { // cancelled, in use, access denied...
        flash(u"没有删除“%1”"_s.arg(result.name));
        return;
    }
    m_history->remove(result.path);
    const SearchResult* row = m_results.at(m_results.indexOf(result.path));
    const bool contentRow = row && row->line > 0;
    if (m_results.remove(result.path) && m_scope != Content) {
        if (contentRow)
            --m_contentHits;
        else if (m_totalMatches > 0)
            --m_totalMatches;
    }
    refreshStatus();
    flash(ok ? u"已将“%1”移到回收站"_s.arg(result.name) : u"已删除“%1”"_s.arg(result.name));
}

QVariantList Launcher::menuItems(int row) const
{
    const SearchResult* r = m_results.at(row);
    if (!r)
        return {};
    const auto entry = [](Action action, const QString& text, const QString& shortcut, const QString& glyph) {
        return QVariantMap {
            {u"action"_s, static_cast<int>(action)},
            {u"text"_s, text},
            {u"shortcut"_s, shortcut},
            {u"glyph"_s, glyph},
        };
    };
    const QVariantMap separator {{u"separator"_s, true}};

    // Glyphs: Segoe Fluent Icons / MDL2 Assets, or the name of an icon
    // Glyph.qml draws.
    QVariantList items;
    if (r->isApp()) {
        items.append(entry(Open, u"打开"_s, u"Enter"_s, u"\uE8A7"_s)); // OpenInNewWindow
        if (r->elevatable)
            items.append(entry(RunAsAdmin, u"以管理员身份运行"_s, u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
        if (!r->target.isEmpty())
            items.append(entry(Reveal, r->isPackagedApp() ? u"打开安装文件夹"_s : u"打开所在位置"_s, u"Ctrl+Enter"_s,
                u"\uE8DA"_s));
        items.append(separator);
        if (r->hasCopyableTarget())
            items.append(entry(CopyItem, u"复制"_s, u"Ctrl+C"_s, u""_s));
        if (!r->target.isEmpty())
            items.append(entry(CopyPath, u"复制完整路径"_s, u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, u"复制名称"_s, QString(), u"\uE8AC"_s));
        return items;
    }
    items.append(entry(Open, r->isDir ? u"打开文件夹"_s : u"打开"_s, u"Enter"_s, r->isDir ? u"\uE838"_s : u"\uE8E5"_s));
    items.append(entry(Reveal, u"打开所在位置"_s, u"Ctrl+Enter"_s, u"\uE8DA"_s));
    if (!r->isDir && shell::canRunAsAdministrator(r->path))
        items.append(entry(RunAsAdmin, u"以管理员身份运行"_s, u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
    items.append(separator);
    items.append(entry(CopyItem, u"复制"_s, u"Ctrl+C"_s, u"\uE8C8"_s));
    items.append(entry(CopyPath, u"复制完整路径"_s, u"Ctrl+Shift+C"_s, u"copyPath"_s));
    items.append(entry(CopyName, u"复制名称"_s, QString(), u"\uE8AC"_s));
    return items;
}

QRectF Launcher::screenArea(QPointF globalPos) const
{
    QScreen* screen = QGuiApplication::screenAt(globalPos.toPoint());
    if (!screen && m_window)
        screen = m_window->screen();
    return screen ? QRectF(screen->availableGeometry()) : QRectF();
}

void Launcher::prepareMenuWindow(QWindow* menu) const
{
    if (!menu)
        return;
    // Showing it must not take the focus from the launcher: losing it
    // dismisses the launcher, and the search box keeps handling the keys.
    menu->setProperty("_q_showWithoutActivating", true);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    win::styleFramelessWindow(menu, dark, dark ? QColor(0x40, 0x40, 0x40) : QColor(0xD4, 0xD4, 0xD4));
}

} // namespace qf
