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
#include <QSpan>
#include <QPointer>
#include <QScreen>
#include <QStyleHints>

#include <windows.h>

#include <algorithm>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

// QML sees Launcher::Scope; the engine takes ws::Scope. One cast between them.
static_assert(Launcher::All == static_cast<int>(ws::Scope::All) && Launcher::Files == static_cast<int>(ws::Scope::Files)
    && Launcher::Content == static_cast<int>(ws::Scope::Content));

namespace {

// Files 内容 reads at once. Opening a file waits for the antivirus to scan
// it, so throughput grows well past the processor count: about 2x at 16
// threads over 6, and little more beyond 32 (measured with Defender).
constexpr int kContentThreads = 16;

constexpr qsizetype kShownExtensions = 3; // named in the search box and the status line

QString number(qint64 n)
{
    return QLocale().toString(n);
}

// A count rounded to what a glance takes in: 323 万, 3.2M. `unit`: whether
// it ends in one (no space before the noun then, in Chinese).
QString roughNumber(qint64 n, bool* unit)
{
    *unit = false;
    const QLocale locale;
    struct Unit {
        qint64 size;
        QStringView name;
    };
    static constexpr Unit chinese[] {{100'000'000, u" 亿"}, {10'000, u" 万"}};
    static constexpr Unit english[] {{1'000'000'000, u"B"}, {1'000'000, u"M"}, {1'000, u"K"}};
    for (const Unit& u : locale.language() == QLocale::Chinese ? QSpan<const Unit>(chinese) : QSpan<const Unit>(english)) {
        if (n < u.size)
            continue;
        const double value = double(n) / u.size;
        QString text = locale.toString(value, 'f', value < 100 ? 1 : 0);
        const QString zero = locale.decimalPoint() + locale.zeroDigit();
        if (text.endsWith(zero))
            text.chop(zero.size());
        *unit = true;
        return text + u.name;
    }
    return number(n);
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

    connect(&m_results, &ResultModel::selectionChanged, this, &Launcher::refreshStatus);
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
        // Apps installed or removed (or the first list arrived): 全部 shows them.
        if (m_window && m_window->isVisible() && m_scope == All)
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
    m_results.setSelection({});
    search();
}

void Launcher::setScope(Scope scope)
{
    if (scope == m_scope)
        return;
    m_scope = scope;
    emit scopeChanged();
    m_results.setSelection({});
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

// The first few content extensions, ".txt / .md / .log": the full default
// list would overflow the search box.
QString Launcher::shownExtensions() const
{
    QStringList parts;
    for (const QString& ext : m_contentExtensions.first(std::min(kShownExtensions, m_contentExtensions.size())))
        parts.append(u'.' + ext);
    return parts.join(u" / "_s);
}

QString Launcher::contentFilesLabel() const
{
    return m_contentExtensions.size() > kShownExtensions ? tr("%1 等文件").arg(shownExtensions())
                                                         : tr("%1 文件").arg(shownExtensions());
}

QString Launcher::placeholder() const
{
    switch (m_scope) {
    case Files:
        return tr("搜索文件和文件夹");
    case Content:
        return m_contentExtensions.size() > kShownExtensions ? tr("搜索 %1 等文件中的文字").arg(shownExtensions())
                                                             : tr("搜索 %1 文件中的文字").arg(shownExtensions());
    default:
        return tr("搜索应用、文件和文件内容");
    }
}

void Launcher::setContentOptions(QStringList extensions, qint64 maxFileBytes, bool inLowPriority)
{
    m_contentInLowPriority = inLowPriority;
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

void Launcher::retranslate()
{
    m_results.retranslate();
    emit scopeChanged(); // placeholder, contentFilesLabel
    refreshStatus();
}

void Launcher::handleHidden()
{
    m_results.setSelection({});
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
    request.scope = static_cast<ws::Scope>(m_scope);
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
    const ParsedQuery query = ws::parseQuery(m_query);
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
    request.scope = ws::Scope::Content;
    request.contentExtensions = m_contentExtensions;
    request.maxContentFileBytes = m_maxContentBytes;
    if (m_scope == Content) {
        // Asked for: system and program folders too if so set, and many
        // files at once (each open mostly waits for the antivirus).
        request.contentInLowPriority = m_contentInLowPriority;
        request.contentThreads = kContentThreads;
    } else {
        // 全部 looks in your own files on its own, gently.
        request.contentThreads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 2, 6);
    }
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
    showRows(std::move(rows), ws::parseQuery(m_query).highlights);
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
    const int items = static_cast<int>(m_index->itemCount());
    // Waiting for a pause in typing counts too: "0 个结果 · 9 毫秒" would read as final.
    const bool contentPending = !m_quiet && (m_contentRunning || m_contentDebounce.isActive());
    QString s;
    if (m_query.trimmed().isEmpty() || (!m_haveResults && !m_contentRunning)) {
        switch (state) {
        case IndexService::State::Idle:
        case IndexService::State::Loading:
            s = tr("正在加载索引…");
            break;
        case IndexService::State::Building:
            s = tr("正在建立索引… 已收录 %Ln 项", nullptr, items);
            break;
        case IndexService::State::Ready: {
            bool unit = false;
            const QString count = roughNumber(items, &unit);
            s = unit ? tr("已索引 %1项", nullptr, items).arg(count) : tr("已索引 %1 项", nullptr, items).arg(count);
            if (m_index->isRefreshing())
                s += tr(" · 后台同步中");
            break;
        }
        }
    } else if (m_scope == Content) {
        if (contentPending)
            s = m_contentTotal > 0 ? tr("正在搜索 %1内容… %2 / %3")
                                         .arg(contentFilesLabel(), number(m_contentScanned), number(m_contentTotal))
                                   : tr("正在搜索 %1内容…").arg(contentFilesLabel());
        else if (m_contentScanned < m_contentTotal && m_results.count() > 0)
            s = tr("已显示前 %Ln 个包含该文字的文件（共 %1 个 %2）", nullptr, m_results.count())
                    .arg(number(m_contentTotal), contentFilesLabel());
        else
            s = tr("%Ln 个文件包含该文字 · 共查找 %1 个 %2", nullptr, m_results.count())
                    .arg(number(m_contentTotal), contentFilesLabel());
    } else {
        const qint64 ms = m_elapsedUs / 1000;
        const int results = static_cast<int>(m_totalMatches + m_contentHits);
        if (contentPending)
            s = m_contentTotal > 0 ? tr("%Ln 个结果 · 正在搜索内容… %1 / %2", nullptr, results)
                                         .arg(number(m_contentScanned), number(m_contentTotal))
                                   : tr("%Ln 个结果 · 正在搜索内容…", nullptr, results);
        else if (m_contentHits > 0)
            s = tr("%Ln 个结果 · 其中 %1 个是文件内容匹配", nullptr, results).arg(number(m_contentHits));
        else
            s = tr("%Ln 个结果 · %1 毫秒", nullptr, results).arg(ms < 1 ? u"<1"_s : number(ms));
        if (state == IndexService::State::Building)
            s += tr(" · 索引尚未建完");
    }
    if (const int selected = m_results.selectedCount(); selected > 0)
        s = tr("已选择 %Ln 项", nullptr, selected);
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
    if (m_results.isSelected(row) && m_results.selectedCount() > 1) {
        performMany(m_results.selection(), static_cast<Action>(action));
        return;
    }
    if (const SearchResult* r = m_results.at(row))
        perform(*r, static_cast<Action>(action));
}

void Launcher::triggerSelection(int action)
{
    const SearchResults items = m_results.selection();
    if (items.size() == 1)
        perform(items.first(), static_cast<Action>(action));
    else if (!items.isEmpty())
        performMany(items, static_cast<Action>(action));
}

void Launcher::toggleSelected(int row)
{
    const SearchResult* r = m_results.at(row);
    if (!r)
        return;
    QSet<QString> paths = m_results.selectedPaths();
    if (!paths.remove(r->path))
        paths.insert(r->path);
    m_results.setSelection(std::move(paths));
}

void Launcher::selectRange(int from, int to, bool add)
{
    QSet<QString> paths = add ? m_results.selectedPaths() : QSet<QString>();
    for (int row = std::max(0, std::min(from, to)); row <= std::max(from, to); ++row) {
        if (const SearchResult* r = m_results.at(row))
            paths.insert(r->path);
    }
    m_results.setSelection(std::move(paths));
}

void Launcher::clearSelection()
{
    m_results.setSelection({});
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
        flash(tr("“%1” 已不存在").arg(r.name));
        return;
    }
    switch (action) {
    case Open:
    case RunAsAdmin:
        m_history->record(r.path);
        shell::open(r.path, action == RunAsAdmin, reportFailure(r.name));
        emit dismissRequested();
        break;
    case Reveal:
        m_history->record(r.path);
        shell::reveal(r.path);
        emit dismissRequested();
        break;
    case CopyPath:
        shell::copyText(r.path);
        flash(tr("已复制路径"));
        break;
    case CopyName:
        shell::copyText(r.name);
        flash(tr("已复制名称"));
        break;
    case CopyItem:
        shell::copyFiles({r.path});
        flash(tr("已复制，可在资源管理器中粘贴"));
        break;
    case Recycle: {
        const auto owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
        // Back on the GUI thread, where `self` can be checked safely.
        shell::recycle({r.path}, owner, [self = QPointer(this), r](bool ok) {
            QMetaObject::invokeMethod(qApp, [self, r, ok] {
                if (self)
                    self->onRecycled(r, ok);
            }, Qt::QueuedConnection);
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
            flash(tr("“%1”不能以管理员身份运行").arg(app.name));
            return;
        }
        m_history->record(app.path);
        shell::launchApp(app.path, action == RunAsAdmin, reportFailure(app.name));
        emit dismissRequested();
        break;
    case Reveal:
        if (app.target.isEmpty() || !QFileInfo::exists(app.target)) {
            flash(tr("“%1”没有可以打开的位置").arg(app.name));
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
            flash(tr("已复制，可在资源管理器中粘贴"));
            break;
        }
        [[fallthrough]]; // nothing to paste: copy where it is instead
    case CopyPath:
        shell::copyText(app.target.isEmpty() ? app.path : app.target);
        flash(tr("已复制路径"));
        break;
    case CopyName:
        shell::copyText(app.name);
        flash(tr("已复制名称"));
        break;
    case Recycle:
        break; // apps are uninstalled in Windows Settings
    }
}

// Several selected results at once. Those an action does not apply to
// (running a folder as administrator, recycling an app) are left out.
void Launcher::performMany(const SearchResults& items, Action action)
{
    const auto exists = [](const QString& path) { return QFileInfo::exists(path); };
    switch (action) {
    case Open:
    case RunAsAdmin: {
        const bool admin = action == RunAsAdmin;
        int started = 0;
        for (const SearchResult& r : items) {
            if (r.isApp()) {
                if (admin && !r.elevatable)
                    continue;
                shell::launchApp(r.path, admin, reportFailure(r.name));
            } else {
                if (!exists(r.path) || (admin && (r.isDir || !shell::canRunAsAdministrator(r.path))))
                    continue;
                shell::open(r.path, admin, reportFailure(r.name));
            }
            m_history->record(r.path);
            ++started;
        }
        if (started == 0) {
            flash(admin ? tr("选中的项目都不能以管理员身份运行") : tr("选中的项目都已不存在"));
            return;
        }
        emit dismissRequested();
        break;
    }
    case Reveal: {
        QStringList paths;
        int shown = 0;
        for (const SearchResult& r : items) {
            const QString& place = r.isApp() ? r.target : r.path;
            if (place.isEmpty() || !exists(place))
                continue;
            m_history->record(r.path);
            if (r.isPackagedApp())
                shell::open(place); // its install folder; the folder above it is not readable
            else
                paths.append(place);
            ++shown;
        }
        if (shown == 0) {
            flash(tr("选中的项目没有可以打开的位置"));
            return;
        }
        shell::reveal(paths);
        emit dismissRequested();
        break;
    }
    case CopyItem: {
        QStringList files;
        for (const SearchResult& r : items) {
            const QString file = r.isApp() ? (r.hasCopyableTarget() ? r.target : QString()) : r.path;
            if (!file.isEmpty() && exists(file))
                files.append(file);
        }
        if (files.isEmpty()) {
            flash(tr("选中的项目没有可以复制的文件"));
            return;
        }
        shell::copyFiles(files);
        flash(tr("已复制 %Ln 项，可在资源管理器中粘贴", nullptr, static_cast<int>(files.size())));
        break;
    }
    case CopyPath: {
        QStringList paths;
        for (const SearchResult& r : items)
            paths.append(r.isApp() && !r.target.isEmpty() ? r.target : r.path);
        shell::copyText(paths.join(u'\n'));
        flash(tr("已复制 %Ln 个路径", nullptr, static_cast<int>(paths.size())));
        break;
    }
    case CopyName: {
        QStringList names;
        for (const SearchResult& r : items)
            names.append(r.name);
        shell::copyText(names.join(u'\n'));
        flash(tr("已复制 %Ln 个名称", nullptr, static_cast<int>(names.size())));
        break;
    }
    case Recycle: {
        SearchResults files;
        QStringList paths;
        for (const SearchResult& r : items) {
            if (r.isApp()) // apps are uninstalled in Windows Settings
                continue;
            if (!exists(r.path)) {
                forgetRecycled(r);
                continue;
            }
            files.append(r);
            paths.append(r.path);
        }
        if (files.isEmpty()) {
            refreshStatus();
            flash(tr("选中的项目不能删除"));
            return;
        }
        const auto owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
        shell::recycle(paths, owner, [self = QPointer(this), files](bool ok) {
            QMetaObject::invokeMethod(qApp, [self, files, ok] {
                if (self)
                    self->onRecycledMany(files, ok);
            }, Qt::QueuedConnection);
        });
        break;
    }
    }
}

// For shell::open() and launchApp(), which call it on a worker thread.
std::function<void(bool)> Launcher::reportFailure(const QString& name)
{
    return [self = QPointer(this), name](bool ok) {
        if (ok)
            return;
        QMetaObject::invokeMethod(qApp, [self, name] {
            if (self)
                emit self->openFailed(name);
        }, Qt::QueuedConnection);
    };
}

void Launcher::onRecycled(const SearchResult& result, bool ok)
{
    if (!forgetRecycled(result)) { // cancelled, in use, access denied...
        flash(tr("没有删除“%1”").arg(result.name));
        return;
    }
    refreshStatus();
    flash((ok ? tr("已将“%1”移到回收站") : tr("已删除“%1”")).arg(result.name));
}

void Launcher::onRecycledMany(const SearchResults& items, bool ok)
{
    int gone = 0;
    for (const SearchResult& r : items) {
        if (forgetRecycled(r))
            ++gone;
    }
    refreshStatus();
    const int kept = static_cast<int>(items.size()) - gone;
    if (gone == 0)
        flash(tr("没有删除选中的项目"));
    else if (kept > 0)
        flash(tr("已将 %Ln 项移到回收站，%1 项没有删除", nullptr, gone).arg(kept));
    else if (ok)
        flash(tr("已将 %Ln 项移到回收站", nullptr, gone));
    else
        flash(tr("已删除 %Ln 项", nullptr, gone));
}

bool Launcher::forgetRecycled(const SearchResult& result)
{
    if (QFileInfo::exists(result.path))
        return false;
    m_history->remove(result.path);
    const SearchResult* row = m_results.at(m_results.indexOf(result.path));
    const bool contentRow = row && row->line > 0;
    if (m_results.remove(result.path) && m_scope != Content) {
        if (contentRow)
            --m_contentHits;
        else if (m_totalMatches > 0)
            --m_totalMatches;
    }
    return true;
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

    if (m_results.isSelected(row) && m_results.selectedCount() > 1) { // for the whole selection
        const SearchResults selection = m_results.selection();
        const auto any = [&](auto test) { return std::any_of(selection.cbegin(), selection.cend(), test); };
        QVariantList items;
        items.append(entry(Open, tr("打开 %Ln 项", nullptr, static_cast<int>(selection.size())), u"Enter"_s, u""_s));
        if (any([](const SearchResult& r) { return !r.isApp() || !r.target.isEmpty(); }))
            items.append(entry(Reveal, tr("打开所在位置"), u"Ctrl+Enter"_s, u""_s));
        items.append(separator);
        if (any([](const SearchResult& r) { return !r.isApp() || r.hasCopyableTarget(); }))
            items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u""_s));
        items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u""_s));
        return items;
    }

    // Glyphs: Segoe Fluent Icons / MDL2 Assets, or the name of an icon
    // Glyph.qml draws.
    QVariantList items;
    if (r->isApp()) {
        items.append(entry(Open, tr("打开"), u"Enter"_s, u"\uE8A7"_s)); // OpenInNewWindow
        if (r->elevatable)
            items.append(entry(RunAsAdmin, tr("以管理员身份运行"), u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
        if (!r->target.isEmpty())
            items.append(entry(Reveal, r->isPackagedApp() ? tr("打开安装文件夹") : tr("打开所在位置"), u"Ctrl+Enter"_s,
                u"\uE8DA"_s));
        items.append(separator);
        if (r->hasCopyableTarget())
            items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u""_s));
        if (!r->target.isEmpty())
            items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        return items;
    }
    items.append(entry(Open, r->isDir ? tr("打开文件夹") : tr("打开"), u"Enter"_s, r->isDir ? u"\uE838"_s : u"\uE8E5"_s));
    items.append(entry(Reveal, tr("打开所在位置"), u"Ctrl+Enter"_s, u"\uE8DA"_s));
    if (!r->isDir && shell::canRunAsAdministrator(r->path))
        items.append(entry(RunAsAdmin, tr("以管理员身份运行"), u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
    items.append(separator);
    items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
    items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
    items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
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

} // namespace ws
