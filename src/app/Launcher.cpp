#include "Launcher.h"

#include "AppCatalog.h"
#include "FileIconProvider.h"
#include "FilePreview.h"
#include "History.h"
#include "Host.h"
#include "IndexService.h"
#include "Merge.h"
#include "PathText.h"
#include "Query.h"
#include "SearchEngine.h"
#include "ShareLister.h"
#include "SwitchBackend.h"
#include "Switches.h"
#include "SystemCatalog.h"
#include "Unc.h"
#include "platform/DragOut.h"
#include "platform/Shell.h"
#include "platform/ShellContextMenu.h"
#include "platform/WindowEffects.h"

#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStyleHints>

#include <windows.h>

#include <algorithm>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

// QML sees Launcher::Scope; the engine takes ws::Scope. One cast between them.
static_assert(Launcher::All == static_cast<int>(ws::Scope::All) && Launcher::Files == static_cast<int>(ws::Scope::Files)
    && Launcher::Content == static_cast<int>(ws::Scope::Content)
    && Launcher::Folders == static_cast<int>(ws::Scope::Folders));
static_assert(Launcher::BestMatch == static_cast<int>(RankBy::Match)
    && Launcher::NewestFirst == static_cast<int>(RankBy::Newest)
    && Launcher::OldestFirst == static_cast<int>(RankBy::Oldest));
static_assert(Launcher::RecentlyOpened == static_cast<int>(ws::StartList::Opened)
    && Launcher::RecentlyModified == static_cast<int>(ws::StartList::Modified));

namespace {

// Files 内容 reads at once. Opening a file waits for the antivirus to scan
// it, so throughput grows well past the processor count: about 2x at 16
// threads over 6, and little more beyond 32 (measured with Defender).
constexpr int kContentThreads = 16;

constexpr qsizetype kShownExtensions = 3; // named in the search box and the status line

// The most a content size limit can be set to (Settings: roundedSizeMB), the
// limit for the files asked for after being left out (searchLeftOut "large").
constexpr std::int64_t kLargestFile = 4096ll << 20;

// The hints' kinds (leftOutHints), and a drive's root from its kind.
constexpr QLatin1StringView kLargeHint("large");
constexpr QLatin1StringView kSystemHint("system");
constexpr QLatin1StringView kTypesHint("types");
constexpr QLatin1StringView kDriveHint("drive:"); // + "E:"
// With only the chosen folders indexed (GitHub #52): a folder not among them
// walked for the query (+ its path), the same folder added to them (+ its
// path), the settings' page of them, and the general note closed for good.
constexpr QLatin1StringView kFolderHint("folder:");
constexpr QLatin1StringView kAddFolderHint("addfolder:");
constexpr QLatin1StringView kFoldersHint("folders");
constexpr QLatin1StringView kFoldersDismissHint("folders-dismiss");

// What a hint that walks the disk walks: a drive's root, or a folder.
QString walkedPath(const QString& kind)
{
    if (kind.startsWith(kDriveHint))
        return kind.mid(kDriveHint.size());
    if (kind.startsWith(kFolderHint))
        return kind.mid(kFolderHint.size());
    return {};
}

// Rows asked for at a time: names, content matches, files written lately.
// Scrolled to the end, a page more (loadMore), up to kMaxPages.
constexpr int kPage = 256;
constexpr int kFewRows = 8; // fewer found than the window shows: the content scan's left-out hints
constexpr int kMaxPages = 16;

// Ranked by time: where a content match goes among `count` rows (`at(i)`
// reads one). Among the files, name and content matches alike, in their
// order; not among apps, places, web shortcuts, plugins' rows or folders,
// and never above `floor`. With no file at or below it: before the folders
// at the end (files first), else at the end.
template <typename At>
int timePlace(int count, At at, const SearchResult& r, int floor, RankBy by, KindOrder kindOrder)
{
    int lastFile = -1;
    for (int row = floor; row < count; ++row) {
        const SearchResult& x = at(row);
        if (x.isApp() || x.isDir)
            continue;
        if (rankedBefore(r, x, by))
            return row;
        lastFile = row;
    }
    if (lastFile >= 0)
        return lastFile + 1;
    int row = count;
    while (kindOrder == KindOrder::FilesFirst && row > floor && at(row - 1).isDir)
        --row;
    return row;
}

// Whether the server of a network path answers (share::reachable: a few
// milliseconds, at most 1.5 seconds); true for a local path. Asked first and
// afresh, as asking Windows about a file on a server that went away (even
// seconds ago) holds the launcher up for half a minute. `fresh` false takes
// an answer of the last seconds: for many paths on one server.
bool serverAnswers(const QString& path, bool fresh = true)
{
    if (!path.startsWith(u"\\\\"))
        return true;
    const std::wstring p = path.toStdWString();
    return !unc::isUnc(p) || share::reachable(unc::host(p), std::chrono::milliseconds(1500), fresh);
}

// The drives being read from disk now, with how far: "D: 45%、E:".
QString readingDrives(const IndexService& index)
{
    QStringList drives;
    for (const IndexService::Reading& r : index.readingVolumes()) {
        const QString root = QString::fromStdWString(r.root);
        drives.append(r.percent >= 0 ? u"%1 %2%"_s.arg(root).arg(r.percent) : root);
    }
    return drives.join(Launcher::tr("、"));
}

// The folder a file is in: "D:\工作文档", "C:\" for one at a drive's root;
// empty for a path with no folder.
QString folderOf(const QString& file)
{
    const QString path = QDir::toNativeSeparators(file);
    const qsizetype slash = path.lastIndexOf(u'\\');
    if (slash <= 0)
        return {};
    return slash == 2 && path[1] == u':' ? path.left(3) : path.left(slash);
}

// The interface's language, as the plugins' texts are picked (Text::pick).
QString uiLocale()
{
    return QLocale().language() == QLocale::English ? u"en"_s : u"zh"_s;
}

} // namespace

// The screenshot history's rows are a plugin's in all but where they come
// from: these ids, never a plugin's (a plugin's id has a dot after its
// author's name too, but "winshun." is Win顺's own, not given out).
constexpr QStringView kShotPlugin = u"winshun.capture";
constexpr QStringView kShotCommand = u"history";
constexpr QStringView kShotIcon = u"glyph:U+E722"; // Camera
constexpr int kShotRows = 500;

// 贴到屏幕 on a file's row: the pictures Windows Imaging Component reads
// (WebP with Windows' codec; a GIF's first frame).
static bool pinnablePicture(const QString& path)
{
    static const QStringList kinds {u"png"_s, u"jpg"_s, u"jpeg"_s, u"bmp"_s, u"gif"_s, u"webp"_s};
    return kinds.contains(QFileInfo(path).suffix().toLower());
}

// The result model as plugin::Merge writes it: a plugin's whole list goes
// through showRows (keyword mode); pushed rows go in and out one by one.
class Launcher::ModelSink : public plugin::RowSink {
public:
    explicit ModelSink(Launcher& launcher)
        : m_launcher(launcher)
    {
    }
    int count() const override { return m_launcher.m_results.count(); }
    const SearchResult& row(int index) const override { return *m_launcher.m_results.at(index); }
    int currentRow() const override { return m_launcher.m_currentRow; }
    void assign(SearchResults rows) override { m_launcher.showPluginRows(std::move(rows)); }
    void append(const SearchResults& rows) override { m_launcher.m_results.append(rows); }
    void insert(int index, const SearchResult& row) override { m_launcher.m_results.insert(index, row); }
    void update(int index, const SearchResult& row) override { m_launcher.m_results.update(index, row); }
    void remove(int index) override
    {
        if (const SearchResult* r = m_launcher.m_results.at(index))
            m_launcher.m_results.remove(r->path);
    }

private:
    Launcher& m_launcher;
};

Launcher::Launcher(IndexService* index, AppCatalog* apps, SystemCatalog* places, SearchEngine* engine, History* history,
    QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_apps(apps)
    , m_places(places)
    , m_engine(engine)
    , m_history(history)
    , m_sink(std::make_unique<ModelSink>(*this))
{
    m_pluginWait.setSingleShot(true);
    m_pluginWait.setInterval(150ms); // a plugin that answers within it shows nothing of the wait
    connect(&m_pluginWait, &QTimer::timeout, this, [this] {
        m_pluginSlow = true;
        refreshStatus();
    });
    m_pluginErrorTimer.setSingleShot(true);
    m_pluginErrorTimer.setInterval(3s);
    connect(&m_pluginErrorTimer, &QTimer::timeout, this, [this] {
        m_pluginError.clear();
        refreshStatus();
    });

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

    m_awakeTick.setSingleShot(true);
    connect(&m_awakeTick, &QTimer::timeout, this, [this] {
        m_results.switchChanged(u"keep-awake"_s); // its second line counts the minutes
        scheduleAwakeTick();
    });

    m_slowSearch.setSingleShot(true);
    m_slowSearch.setInterval(1s);
    connect(&m_slowSearch, &QTimer::timeout, this, [this] {
        m_searchSlow = true;
        refreshStatus();
    });

    connect(&m_results, &ResultModel::selectionChanged, this, &Launcher::refreshStatus);
    connect(&m_suggester, &WebSuggester::answered, this, &Launcher::onSuggestions);
    connect(m_engine, &SearchEngine::resultsReady, this, &Launcher::onResults);
    connect(m_engine, &SearchEngine::contentResults, this, &Launcher::onContentResults);
    connect(m_engine, &SearchEngine::contentProgress, this, &Launcher::onContentProgress);
    connect(m_engine, &SearchEngine::contentLeftOut, this, &Launcher::onContentLeftOut);
    connect(m_engine, &SearchEngine::contentScope, this, &Launcher::onContentScope);
    connect(this, &Launcher::folderChipsChanged, this, &Launcher::refreshKeyHints); // Tab: 换个文件夹, or none to go to
    connect(this, &Launcher::queryChanged, this, &Launcher::folderChipsChanged); // a folder typed, or no longer
    connect(this, &Launcher::scopeChanged, this, &Launcher::folderChipsChanged);
    connect(m_engine, &SearchEngine::removableDrives, this, &Launcher::onRemovableDrives);
    connect(m_engine, &SearchEngine::driveResults, this, &Launcher::onDriveResults);
    connect(m_engine, &SearchEngine::driveProgress, this, &Launcher::onDriveProgress);
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
    connect(m_places, &SystemCatalog::changed, this, [this] {
        if (m_window && m_window->isVisible() && m_scope == All)
            search();
    });
    refreshStatus();
}

Launcher::~Launcher() = default;

void Launcher::setPluginHost(plugin::Host* host)
{
    m_plugins = host;
    m_merge.reset();
    if (!host)
        return;
    m_merge = std::make_unique<plugin::Merge>(*m_sink, host->catalog());
    m_merge->setLocale(uiLocale());
    m_merge->setMoreText(tr("（%1 查看全部）"));
    m_merge->setHintText(tr("按空格进入；Enter 或 Tab 也行"));
    m_merge->setPrefixHintText(tr("按 Tab 补全；Enter 直接进入"));
    connect(host, &plugin::Host::queryTaken, this, &Launcher::onPluginTaken);
    connect(host, &plugin::Host::results, this, &Launcher::onPluginResults);
    connect(host, &plugin::Host::queryFinished, this, &Launcher::onPluginFinished);
    connect(host, &plugin::Host::queryFailed, this, &Launcher::onPluginFailed);
    connect(host, &plugin::Host::progress, this, &Launcher::onPluginProgress);
    connect(host, &plugin::Host::notice, this, [this](const QString&, const QString& text) { flash(text); });
    connect(host, &plugin::Host::actionDone, this, &Launcher::onPluginActionDone);
}

void Launcher::setQuery(const QString& query)
{
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    m_results.setSelection({});
    m_held.reset(); // typed on: a key held for the rows before is not for these
    search();
}

void Launcher::setScope(Scope scope)
{
    if (scope == Content && !m_contentEnabled)
        scope = All; // a plugin's row may still ask for it
    // Back in 文件: as it was left (文件夹, or the kind chosen kept meanwhile).
    if (scope == Files && m_scope != Folders && m_foldersChosen)
        scope = Folders;
    if (scope == m_scope)
        return;
    m_scope = scope;
    if ((scope == Files || scope == Folders) && m_foldersChosen != (scope == Folders)) {
        m_foldersChosen = scope == Folders;
        emit fileChoiceChanged();
    }
    emit scopeChanged();
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints(); // Tab: through the kinds in 文件
    // Keep the old rows until the new results replace them: clearing first
    // collapses the window and then grows it back (flicker).
    search();
}

bool Launcher::busy() const
{
    const auto state = m_index->state();
    return state == IndexService::State::Loading || state == IndexService::State::Building
        || (m_contentRunning && !m_quiet) || m_walkRunning;
}

qreal Launcher::progress() const
{
    // While the index is still being built, that is the work going on.
    if (!m_contentRunning || m_quiet || m_contentTotal <= 0 || m_index->state() != IndexService::State::Ready)
        return -1;
    return std::clamp(qreal(m_contentScanned) / m_contentTotal, 0.0, 1.0);
}

bool Launcher::moreAvailable() const
{
    // Not while a hint's one-off search runs: its rows reaching the end of
    // the list would ask for a page more, which would cut it short.
    return (m_moreNames || m_moreContent) && m_pages < kMaxPages && m_haveResults && !m_replacePending && !m_keyword
        && m_hintRunning.isEmpty();
}

void Launcher::loadMore()
{
    if (!moreAvailable() || searching() || !m_window || !m_window->isVisible())
        return;
    ++m_pages;
    search(true);
}

bool Launcher::hintsSettled() const
{
    if (!m_haveResults || m_replacePending || m_keyword || m_query.trimmed().isEmpty())
        return false;
    return m_quiet || (!m_pending && !m_contentRunning && !m_walkRunning && !m_contentDebounce.isActive());
}

void Launcher::setChosenFolders(bool only, const QStringList& folders, bool noteDismissed)
{
    const bool changed = only != m_foldersOnly || folders != m_chosenFolders || noteDismissed != m_folderNoteDismissed;
    m_foldersOnly = only;
    m_chosenFolders = folders;
    m_folderNoteDismissed = noteDismissed;
    m_outsideKnown = false;
    if (changed && m_window && m_window->isVisible())
        refreshStatus(); // a folder just added is indexed: its note goes
}

void Launcher::noteSearchHere(const QString& folder)
{
    m_hereFolder = folder;
    m_hereSeen = false;
}

QString Launcher::outsideFolder() const
{
    if (!m_foldersOnly || m_scope == Content || m_keyword)
        return {};
    if (m_outsideKnown && m_outsideQuery == m_query)
        return m_outside;
    m_outsideKnown = true;
    m_outsideQuery = m_query;
    m_outside.clear();
    const pathtext::TypedPath typed = pathtext::typedOutside(m_query, m_chosenFolders);
    // A fixed disk of this PC, and the folder there: a removable drive has its
    // own hint, a share its own list, and a folder that is not there holds nothing.
    if (!typed.folder.isEmpty()) {
        const std::wstring root = typed.folder.left(3).toStdWString(); // "D:\"
        if (::GetDriveTypeW(root.c_str()) == DRIVE_FIXED && QFileInfo(typed.folder).isDir())
            m_outside = typed.folder;
    }
    return m_outside;
}

// "Search here" named a folder that is not indexed: what is typed after it is
// looked for in it all the same, the user having picked that folder. Once the
// names are listed, and when there is something to look for.
void Launcher::walkHereSoon()
{
    if (m_hereFolder.isEmpty())
        return;
    const QString outside = outsideFolder();
    if (outside.isEmpty() || !pathtext::sameFolder(outside, m_hereFolder)) {
        if (m_hereSeen || (!m_query.trimmed().isEmpty() && !pathtext::sameFolder(pathtext::splitTyped(m_query).folder, m_hereFolder)))
            m_hereFolder.clear(); // typed on elsewhere
        return;
    }
    m_hereSeen = true;
    const QString kind = kFolderHint + outside;
    if (!m_hintRunning.isEmpty() || m_hintsTaken.contains(kind) || pathtext::splitTyped(m_query).name.trimmed().isEmpty()
        || !hintsSettled())
        return;
    QTimer::singleShot(0, this, [this, kind] { searchLeftOut(kind); }); // not from within the status it follows
}

// In the order they are offered: the content scan's parts (large files in
// any scope; system folders and other kinds of text in 内容, where contents
// are what was asked for: 全部 reads them as a by-product, and under its name
// rows the counts would read oddly), then the drives. The content parts once
// the scan has run to its end (stopped at its limit, more rows come first:
// loadMore), the drives once the names are all listed, for the same reason.
QVariantList Launcher::leftOutHints() const
{
    QVariantList hints;
    // `action2`: a second link on the row, its own kind (clicked, not Enter).
    const auto add = [&](const QString& kind, const QString& text, const QString& action, const QString& kind2 = {},
                         const QString& action2 = {}) {
        hints.append(QVariantMap {{u"kind"_s, kind}, {u"text"_s, text}, {u"action"_s, action}, {u"kind2"_s, kind2},
            {u"action2"_s, action2}, {u"running"_s, kind == m_hintRunning}});
    };
    if (!m_hintRunning.isEmpty()) {
        QString text;
        if (m_hintRunning == kLargeHint)
            text = tr("正在搜这些大文件…");
        else if (m_hintRunning == kSystemHint)
            text = tr("正在搜系统和程序文件夹里的文件…");
        else if (m_hintRunning == kTypesHint)
            text = tr("正在搜这些类型的文件…");
        else
            text = tr("正在搜 %1… 已看 %Ln 项", nullptr, m_walkSeen).arg(QDir::toNativeSeparators(walkedPath(m_hintRunning)));
        add(m_hintRunning, text, tr("停止"));
        return hints;
    }
    if (!hintsSettled())
        return hints;
    // The counts are of files not read, whatever was typed: offered under
    // every list they would be noise. Only when what was found does not fill
    // the window (Main.qml maxRows), where "is that all?" is the question.
    if (m_scanComplete && !m_moreContent && m_results.count() < kFewRows) {
        if (!m_leftOut.large.isEmpty()) {
            add(kLargeHint, tr("另有 %Ln 个大文件超过大小上限没搜", nullptr, static_cast<int>(m_leftOut.large.size())),
                tr("也搜这些"));
        }
        if (m_scope == Content && m_leftOut.lowPriority > 0)
            add(kSystemHint, tr("系统和程序文件夹里还有 %Ln 个文件没搜", nullptr, m_leftOut.lowPriority), tr("也搜这些"));
        if (m_scope == Content && m_leftOut.otherTypes > 0) {
            // The two most numerous kinds by name, "等" when there are more.
            QStringList kinds;
            for (const QString& ext : m_leftOut.otherExtensions.first(std::min<qsizetype>(2, m_leftOut.otherExtensions.size())))
                kinds.append(u'.' + ext);
            const QString named = kinds.join(u' ');
            add(kTypesHint,
                m_leftOut.otherExtensions.size() > 2
                    ? tr("还有 %Ln 个 %1 等文件不在内容搜索的类型里", nullptr, m_leftOut.otherTypes).arg(named)
                    : tr("还有 %Ln 个 %1 文件不在内容搜索的类型里", nullptr, m_leftOut.otherTypes).arg(named),
                tr("也搜这些"));
        }
    }
    if (m_scope != Content && !m_moreNames) {
        for (const RemovableDrive& drive : m_drives) {
            const QString kind = kDriveHint + drive.root;
            if (m_hintsTaken.contains(kind))
                continue;
            add(kind,
                drive.label.isEmpty() ? tr("U 盘 %1 没收录").arg(drive.root) : tr("U 盘 %1（%2）没收录").arg(drive.root, drive.label),
                tr("这次也搜它"));
        }
    }
    // Only the chosen folders are indexed (GitHub #52). The folder typed is
    // not among them: walked for the words typed after it, or added; once
    // walked, adding it is what is left to offer. Else, when few rows came,
    // a word that more may be elsewhere, once (closed for good).
    if (m_scope != Content && !m_moreNames) {
        const QString outside = outsideFolder();
        switch (pathtext::scopeNote(m_foldersOnly, !outside.isEmpty(), m_folderNoteDismissed, static_cast<int>(m_results.count()),
            kFewRows, m_query.trimmed().size())) {
        case pathtext::ScopeNote::Outside:
            if (!m_hintsTaken.contains(kFolderHint + outside))
                add(kFolderHint + outside, tr("这个文件夹不在索引范围内"), tr("这次也搜它"), kAddFolderHint + outside, tr("加入索引"));
            else if (!m_hintsTaken.contains(kAddFolderHint + outside))
                add(kAddFolderHint + outside, tr("这个文件夹不在索引范围内"), tr("加入索引"));
            break;
        case pathtext::ScopeNote::Chosen:
            add(kFoldersHint, tr("只索引了你选的文件夹，可能有别的地方没搜到"), tr("设置文件夹…"), kFoldersDismissHint, tr("不再提示"));
            break;
        case pathtext::ScopeNote::None:
            break;
        }
    }
    return hints;
}

QString Launcher::approximateHeading() const
{
    if (!m_approximate || !m_haveResults || m_shownText.isEmpty())
        return {};
    // What was typed, on one line (as Main.qml shows it when nothing is found).
    static const QRegularExpression kSpaces(u"\\s+"_s);
    return tr("没有叫“%1”的，按读音找到：").arg(QString(m_shownText).replace(kSpaces, u" "_s));
}

// A hint taken up: the same query over that part alone. The content parts
// are a content scan (SearchEngine::Request::contentSet) whose matches come
// in as content matches do: the files over the limit under the largest limit
// the settings allow, the others under the limits set. A drive is walked
// (Request::walkFolder); its rows go after the rows there are, as they come.
// Not while a search runs, but a quiet refresh (the window opened again) is
// stopped for it: its rows stay as they were, which is what a quiet refresh
// keeps anyway, and a scan that finished without finding them again would
// have taken them for gone.
void Launcher::searchLeftOut(const QString& kind)
{
    if (!m_window || !m_window->isVisible() || !m_hintRunning.isEmpty())
        return;
    const QVariantList offered = leftOutHints();
    if (std::none_of(offered.cbegin(), offered.cend(), [&](const QVariant& h) {
            const QVariantMap hint = h.toMap();
            return hint[u"kind"_s] == kind || hint[u"kind2"_s] == kind;
        }))
        return;
    // Not searches: the settings' page, the note closed, a folder added.
    if (kind == kFoldersHint || kind == kFoldersDismissHint) {
        m_folderNoteDismissed = true; // its button was taken either way: said once
        emit folderNoteDismissed();
        if (kind == kFoldersHint)
            emit folderSettingsRequested();
        refreshStatus();
        return;
    }
    if (kind.startsWith(kAddFolderHint)) {
        const QString path = kind.mid(kAddFolderHint.size());
        m_hintsTaken.insert(kind); // the app adds it and says so; the note goes meanwhile
        m_hintsTaken.insert(kFolderHint + path);
        emit folderAddRequested(path);
        refreshStatus();
        return;
    }
    if (m_pending || m_contentRunning) {
        m_engine->cancel();
        m_pending = false;
        m_contentRunning = false;
        m_loadingMore = false;
        m_unconfirmed.clear();
    }
    SearchEngine::Request request;
    request.rankBy = rankBy();
    request.preferredTypes = m_preferredTypes;
    request.excludedTypes = m_excludedTypes;
    if (kind.startsWith(kDriveHint) || kind.startsWith(kFolderHint)) {
        // A folder typed as a path: the words after it are what is looked for.
        request.text = kind.startsWith(kFolderHint) ? pathtext::splitTyped(m_query).name.trimmed() : m_query;
        request.scope = m_scope == Folders ? ws::Scope::Folders : ws::Scope::Files; // 全部: its files and folders
        request.extensions = fileExtensions();
        request.limit = kPage;
        request.walkFolder = walkedPath(kind);
        m_walkRunning = true;
        m_walkSeen = 0;
    } else {
        request.text = m_scope == Content ? m_query.trimmed() : contentNeedle();
        request.scope = ws::Scope::Content;
        request.contentExtensions = m_contentExtensions;
        request.contentSizeLimits = m_contentSizeLimits;
        request.contentDocuments = m_contentDocuments;
        request.contentInLowPriority = m_contentInLowPriority;
        request.contentThreads = kContentThreads; // asked for: many files at once (each open mostly waits for the antivirus)
        request.maxContentResults = kPage * m_pages;
        if (m_scope == Content)
            setContentPlace(request); // those parts where the scan looked
        if (kind == kLargeHint) {
            request.contentSet = SearchEngine::ContentSet::Given;
            request.contentOnly = m_leftOut.large;
            request.maxContentResults = static_cast<int>(m_leftOut.large.size()); // a match per file at most: all of them
            request.contentSizeLimits.bytes.fill(kLargestFile);
        } else {
            request.contentSet
                = kind == kSystemHint ? SearchEngine::ContentSet::LowPriority : SearchEngine::ContentSet::OtherTypes;
        }
        m_pending = true;
        m_contentRunning = true;
    }
    m_hintRunning = kind;
    m_hintsTaken.insert(kind);
    m_hintHits = 0;
    m_quiet = false; // the divider shows how far it is
    m_requestId = m_engine->submit(std::move(request));
    refreshStatus();
}

void Launcher::stopLeftOut()
{
    if (m_hintRunning.isEmpty())
        return;
    m_engine->cancel();
    m_requestId = 0;
    m_pending = false;
    m_contentRunning = false;
    m_walkRunning = false;
    m_contentScanned = m_contentTotal; // the scan behind the rows did run to its end (setSort reads the figures)
    finishLeftOut(true);
}

// The rows it brought went in where they rank (content matches), which may
// be out of sight: the footer says what came of it.
void Launcher::finishLeftOut(bool stopped)
{
    const QString kind = std::exchange(m_hintRunning, QString());
    const int hits = m_hintHits;
    if (kind == kLargeHint)
        m_leftOut.large.clear();
    else if (kind == kSystemHint)
        m_leftOut.lowPriority = 0;
    else if (kind == kTypesHint)
        m_leftOut.otherTypes = 0;
    QString said;
    if (stopped)
        said = hits > 0 ? tr("已停止，找到 %Ln 条", nullptr, hits) : tr("已停止");
    else if (kind == kLargeHint)
        said = hits > 0 ? tr("大文件里找到 %Ln 条", nullptr, hits) : tr("这些大文件里也没有");
    else if (kind == kSystemHint)
        said = hits > 0 ? tr("系统和程序文件夹里找到 %Ln 条", nullptr, hits) : tr("系统和程序文件夹里也没有");
    else if (kind == kTypesHint)
        said = hits > 0 ? tr("这些类型的文件里找到 %Ln 条", nullptr, hits) : tr("这些类型的文件里也没有");
    else {
        const QString root = QDir::toNativeSeparators(walkedPath(kind));
        said = hits > 0 ? tr("%1 里找到 %Ln 项", nullptr, hits).arg(root) : tr("%1 里也没有").arg(root);
    }
    flash(said);
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
    if (m_contentDocuments)
        return tr("文档和 %1 等文件").arg(u'.' + m_contentExtensions.first());
    return m_contentExtensions.size() > kShownExtensions ? tr("%1 等文件").arg(shownExtensions())
                                                         : tr("%1 文件").arg(shownExtensions());
}

QString Launcher::placeholder() const
{
    switch (m_scope) {
    case Files:
        if (m_fileType >= 0)
            return tr("搜索%1", "a kind of file, as 图片").arg(m_fileTypes[m_fileType].name);
        return tr("搜索文件和文件夹");
    case Folders:
        return tr("搜索文件夹");
    case Content:
        if (!m_contentFolder.isEmpty())
            return tr("搜索 %1 里的文字").arg(folderLabel(m_contentFolder));
        if (m_contentDocuments)
            return tr("搜索 Word、Excel、PDF 和 %1 等文件中的文字").arg(u'.' + m_contentExtensions.first());
        return m_contentExtensions.size() > kShownExtensions ? tr("搜索 %1 等文件中的文字").arg(shownExtensions())
                                                             : tr("搜索 %1 文件中的文字").arg(shownExtensions());
    default:
        return m_contentEnabled ? tr("搜索应用、文件和文件内容") : tr("搜索应用和文件");
    }
}

void Launcher::setContentIndex(bool on)
{
    if (std::exchange(m_contentIndex, on) != on)
        emit contentIndexChanged();
}

void Launcher::setContentOptions(
    bool enabled, QStringList extensions, const ContentSizeLimits& sizeLimits, bool inLowPriority, bool documents)
{
    const bool wasEnabled = std::exchange(m_contentEnabled, enabled);
    if (!enabled) {
        m_contentFolder.clear();
        m_contentPick = {};
    }
    m_contentInLowPriority = inLowPriority;
    m_contentDocuments = documents;
    for (QString& ext : extensions) {
        ext = ext.trimmed();
        while (ext.startsWith(u'.') || ext.startsWith(u'*'))
            ext.remove(0, 1);
    }
    extensions.removeAll(QString());
    if (extensions.isEmpty())
        extensions = {u"txt"_s};
    m_contentExtensions = extensions;
    m_contentSizeLimits = sizeLimits;
    if (!enabled && m_scope == Content) {
        setScope(All);
        return;
    }
    emit scopeChanged(); // placeholder text mentions the extensions
    if (enabled != wasEnabled && m_scope == All && m_window && m_window->isVisible())
        search(); // 全部: content matches below the names come or go
}

void Launcher::setWebShortcuts(const WebShortcuts& shortcuts)
{
    if (shortcuts == m_web)
        return;
    m_web = shortcuts;
    // A suggestion address edited or added counts from the next key typed:
    // nothing kept from before is shown again, and the rows on screen (whose
    // site may have changed its name or address) wait for the new answer.
    m_suggester.clearCache();
    dropSuggestions();
    if (m_window && m_window->isVisible() && m_scope == All && !m_query.trimmed().isEmpty())
        search();
}

void Launcher::setWebSuggestions(bool on)
{
    if (on == m_webSuggestions)
        return;
    m_webSuggestions = on;
    if (!on) {
        m_suggester.cancel();
        dropSuggestions();
    } else if (m_window && m_window->isVisible() && m_scope == All && !m_query.trimmed().isEmpty()) {
        search();
    }
}

void Launcher::setFavorites(Favorites* favorites)
{
    if (m_favorites)
        disconnect(m_favorites, nullptr, this, nullptr);
    m_favorites = favorites;
    if (favorites) {
        connect(favorites, &Favorites::changed, this, [this] {
            emit favoritesChanged();
            emit folderChipsChanged();
            if (m_window && m_window->isVisible() && !m_keyword && m_scope != Content)
                search(); // the rows refreshed in place: 收藏 comes or goes
        });
    }
    emit favoritesChanged();
}

bool Launcher::hasFavorites() const
{
    return m_favorites && !m_favorites->items().isEmpty();
}

void Launcher::setSwitches(Switches* switches, const QString& stateFile)
{
    if (m_switches)
        disconnect(m_switches, nullptr, this, nullptr);
    m_switches = switches;
    m_stateFile = stateFile;
    m_switchPageTold = !stateFile.isEmpty() && QSettings(stateFile, QSettings::IniFormat).value(u"Hints/SwitchPage"_s).toBool();
    m_previewWanted = !stateFile.isEmpty() && QSettings(stateFile, QSettings::IniFormat).value(u"Launcher/PreviewOpen"_s).toBool();
    m_results.setSwitches(switches);
    if (!switches)
        return;
    // What Enter does on the row the keyboard is on follows its switch, whoever changed it.
    connect(switches, &Switches::changed, this, [this](const QString& id) {
        const SearchResult* r = m_results.at(m_currentRow);
        if (r && r->switchId == id)
            refreshKeyHints();
        if (id == u"keep-awake" && m_window && m_window->isVisible())
            scheduleAwakeTick(); // turned on or off, or given another end time
    });
    // A toggle that had to wait for the state is done, or given up.
    connect(switches, &Switches::waitEnded, this, [this](const QString& id, Switches::Outcome outcome) {
        const auto waiting = m_switchWaiting.find(id);
        if (waiting == m_switchWaiting.end())
            return;
        const SearchResult row = waiting.value();
        m_switchWaiting.erase(waiting);
        if (m_window && m_window->isVisible()) // closed meanwhile: it is not opened or said after the fact
            switchOutcome(row, {}, static_cast<int>(outcome));
    });
}

void Launcher::setResultOptions(KindOrder kindOrder, bool showModified, bool thumbnails, const QString& numberKeys)
{
    m_results.setThumbnails(thumbnails);
    const QString keys = numberKeys == u"off" ? QString() : numberKeys;
    if (kindOrder == m_kindOrder && showModified == m_showModified && keys == m_numberKeys)
        return;
    const bool reorder = kindOrder != m_kindOrder;
    m_kindOrder = kindOrder;
    m_showModified = showModified;
    m_numberKeys = keys;
    emit resultOptionsChanged();
    if (reorder && m_window && m_window->isVisible() && m_scope != Content)
        search();
}

void Launcher::setDoubleClickToOpen(bool on)
{
    if (on == m_doubleClickToOpen)
        return;
    m_doubleClickToOpen = on;
    emit resultOptionsChanged();
}

void Launcher::setPreferredTypes(const QStringList& types)
{
    if (types == m_preferredTypes)
        return;
    m_preferredTypes = types;
    // Their order shows only ranked by match, among names (not 内容's rows).
    if (m_window && m_window->isVisible() && m_scope != Content && rankBy() == RankBy::Match)
        search();
}

void Launcher::setExcludedTypes(const QStringList& types)
{
    if (types == m_excludedTypes)
        return;
    m_excludedTypes = types;
    // Files come or go in whatever is listed now: asked again, in place.
    if (m_window && m_window->isVisible())
        search();
}

void Launcher::setNumberHints(const QString& hints)
{
    if (m_numberHints == hints)
        return;
    m_numberHints = hints;
    emit resultOptionsChanged();
}

void Launcher::setNumberKeysLeft(unsigned digits)
{
    if (digits == m_numberKeysLeft)
        return;
    m_numberKeysLeft = digits;
    emit resultOptionsChanged();
}

void Launcher::setSort(Sort sort)
{
    if (sort == m_sort)
        return;
    m_sort = sort;
    emit sortChanged();
    if (!m_window || !m_window->isVisible() || m_query.trimmed().isEmpty() || m_keyword)
        return; // hidden: handleShown() searches
    // 内容, every match on screen: put in the other order, not searched again
    // (that would read the files once more).
    if (m_scope == Content && m_shownScope == Content && m_haveResults && !m_replacePending && m_scanComplete
        && !m_contentRunning && m_contentScanned >= m_contentTotal && m_shownText == m_query.trimmed()) {
        SearchResults rows;
        rows.reserve(m_results.count());
        for (int row = 0; row < m_results.count(); ++row)
            rows.append(*m_results.at(row));
        const RankBy by = rankBy();
        std::ranges::stable_sort(rows, [by](const SearchResult& a, const SearchResult& b) { return rankedBefore(a, b, by); });
        m_replacePending = true; // new rows: the first is selected
        showRows(std::move(rows), {});
        refreshStatus();
        return;
    }
    search();
}

void Launcher::setStartList(StartList list)
{
    if (list == m_startList)
        return;
    m_startList = list;
    emit startListChanged();
    if (m_window && m_window->isVisible() && showsStartList())
        search();
}

RankBy Launcher::rankBy() const
{
    // The panel with nothing typed: ranked means nothing, the newest first.
    if (m_panel && m_panelSort.key == SortKey::Rank && m_query.trimmed().isEmpty())
        return RankBy::Newest;
    // The sorts by a column are the names' (nameOrder); content matches stay as ranked.
    return m_sort <= OldestFirst ? static_cast<RankBy>(m_sort) : RankBy::Match;
}

SortOrder Launcher::nameOrder() const
{
    if (m_panel)
        return m_panelSort; // its columns; the sorts below are the compact list's
    switch (m_sort) {
    case ByName:
        return {SortKey::Name, false};
    case ByFolder:
        return {SortKey::Path, false};
    case ByType:
        return {SortKey::Type, false};
    case LargestFirst:
        return {SortKey::Size, true};
    case SmallestFirst:
        return {SortKey::Size, false};
    case BestMatch:
    case NewestFirst:
    case OldestFirst:
        break;
    }
    return {};
}

bool Launcher::showsStartList() const
{
    return m_scope != Content && m_query.trimmed().isEmpty() && !m_panel; // the panel lists everything instead
}

void Launcher::switchOrder()
{
    // The panel: as ranked, newest first, oldest first, round (its columns' orders).
    if (m_panel) {
        if (m_panelSort.key == SortKey::Modified && m_panelSort.descending)
            sortBy(static_cast<int>(SortKey::Modified)); // the other way round
        else if (m_panelSort.key == SortKey::Modified)
            sortBy(static_cast<int>(SortKey::Rank));
        else
            sortBy(static_cast<int>(SortKey::Modified));
        return;
    }
    // The footer's switch shows what it is now; the status keeps its figures.
    if (showsStartList()) {
        if (m_startList != NoStartList) // set so in the settings: kept so
            setStartList(m_startList == RecentlyOpened ? RecentlyModified : RecentlyOpened);
    }
    else if (!m_keyword) // a command's list is in the plugin's order
        // Ctrl+S goes round the first three; the other sorts (#44) are chosen
        // from the menu, and from one of them it goes back to the best match.
        setSort(m_sort <= OldestFirst ? static_cast<Sort>((m_sort + 1) % 3) : BestMatch);
}

void Launcher::setInTaskbarBox(bool on)
{
    if (on == m_inTaskbarBox)
        return;
    m_inTaskbarBox = on;
    emit inTaskbarBoxChanged();
}

void Launcher::reportField(
    const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition)
{
    emit fieldReported(text, cursor, selectionStart, selectionEnd, composition);
}

void Launcher::pressNumber(int number)
{
    if (number < 1 || number > 9 || m_numberKeys.isEmpty() || holdUntilShown(Open, number))
        return;
    emit numberPressed(number);
}

bool Launcher::holdUntilShown(int action, int number)
{
    if (!m_replacePending || !searching())
        return false; // the rows on screen are the query's
    m_held = {action, number};
    return true;
}

void Launcher::startOver()
{
    if (!m_contentFolder.isEmpty() || !m_contentPick.folder.isEmpty()) {
        m_contentFolder.clear();
        m_contentPick = {};
        emit folderChipsChanged();
    }
    if (filtersActive()) { // the panel's, as at first too (its order stays)
        m_dateChoice = 0;
        m_dateFrom.clear();
        m_dateTo.clear();
        m_sizeChoice = 0;
        m_sizeMin.clear();
        m_sizeMax.clear();
        m_folderFilter.clear();
        emit filtersChanged();
    }
    if (m_scope != All) {
        m_scope = All;
        emit scopeChanged();
    }
    if (!m_query.isEmpty()) {
        m_query.clear();
        emit queryChanged();
    }
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints();
}

void Launcher::handleShown()
{
    m_index->contentIndexer()->setSearchBoxOpen(true); // the files it searches are read before the indexer's
    m_statusPoll.start();
    m_apps->refresh(); // reads the list again only if apps were installed or removed
    m_places->refresh(); // ... if Windows was updated or its language changed
    if (m_switches)
        m_switches->refresh(); // the switches' rows show what they are now
    scheduleAwakeTick();
    m_previewResume = m_previewWanted;
    search(); // refresh: files may have changed, and "recent" certainly has
    emit historyChanged(); // things were opened since
    refreshFrequentApps();
    emit shown();
}

void Launcher::refreshFrequentApps()
{
    constexpr std::size_t kFrequentApps = 6; // a row over the taskbar
    std::vector<FrequentApp> picked;
    const std::shared_ptr<const AppList> apps = m_apps->apps();
    if (m_recordHistory && apps) {
        if (!m_windowsUsesAge.isValid() || m_windowsUsesAge.hasExpired(60'000)) {
            m_windowsUses = windowsAppUses(); // some 500 registry values: a few ms
            m_windowsUsesAge.start();
        }
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        const auto indexes
            = ws::frequentApps(*apps, *m_history, m_windowsUses, QDateTime::currentMSecsSinceEpoch(), kFrequentApps);
        for (const std::size_t i : indexes) {
            const AppInfo& app = (*apps)[i];
            const bool packaged = app.kind == AppKind::Store || app.kind == AppKind::System || app.kind == AppKind::Package;
            picked.push_back({app.name, app.launchPath(), FileIconProvider::appIconUrl(app.id, packaged, dark)});
        }
    }
    if (picked == m_frequent)
        return;
    m_frequent = std::move(picked);
    emit frequentAppsChanged();
}

QVariantList Launcher::frequentApps() const
{
    QVariantList list;
    for (const FrequentApp& app : m_frequent)
        list.append(QVariantMap {{u"name"_s, app.name}, {u"icon"_s, app.icon}});
    return list;
}

void Launcher::openFrequentApp(int index)
{
    if (index < 0 || index >= static_cast<int>(m_frequent.size()))
        return;
    const FrequentApp app = m_frequent[static_cast<std::size_t>(index)];
    remember(app.path);
    shell::launchApp(app.path, false, reportFailure(app.name));
    emit dismissRequested();
}

void Launcher::retranslate()
{
    m_results.retranslate();
    if (m_merge) {
        m_merge->setLocale(uiLocale());
        m_merge->setMoreText(tr("（%1 查看全部）"));
        m_merge->setHintText(tr("按空格进入；Enter 或 Tab 也行"));
        m_merge->setPrefixHintText(tr("按 Tab 补全；Enter 直接进入"));
    }
    emit scopeChanged(); // placeholder, contentFilesLabel
    refreshStatus();
}

void Launcher::handleHidden()
{
    closePreview(); // the wish to have it stays (m_previewWanted)
    m_previewResume = false;
    m_index->contentIndexer()->setSearchBoxOpen(false);
    m_systemCommand = false;
    m_results.setSelection({});
    m_held.reset();
    m_statusPoll.stop();
    m_awakeTick.stop();
    m_contentDebounce.stop();
    m_slowSearch.stop();
    m_searchSlow = false;
    m_suggester.cancel(); // nobody is looking; shown again, the same words come from the cache
    if (m_contentRunning || m_pending || m_walkRunning) {
        m_engine->cancel();
        m_requestId = 0;
        m_pending = false;
        m_contentRunning = false;
        m_walkRunning = false;
    }
    m_hintRunning.clear(); // whatever was asked is over; the hint is not offered again (m_hintsTaken)
    m_hereFolder.clear();
    if (m_plugins)
        m_plugins->cancelAll(); // nobody is looking: whatever they were asked is moot
    m_pluginPending.clear();
    m_pluginWait.stop();
    m_pluginSlow = false;
    // Opened again, a page, as quick as ever; more as it is scrolled again.
    m_pages = 1;
    m_loadingMore = false;
}

// Nothing is cleared up front: the rows on screen stay until results replace
// them. Searching what is already shown again (the window reopens, the index
// changes) is a refresh: the rows are updated in place, and content matches
// from the last scan stay until the new scan has had a chance to find them.
void Launcher::search(bool more)
{
    m_flash.clear();
    m_contentDebounce.stop();
    m_slowSearch.stop();
    m_searchSlow = false;
    const QString text = m_query.trimmed();
    // Sorted another way, or the other start list, the rows are new ones
    // (the first is selected). So is a plugin command's list in place of
    // the names' (or back): "=" and "= " are the same text trimmed.
    std::optional<KeywordMode> keyword = matchKeyword(m_query);
    const QString keywordKey = keyword ? keyword->pluginId + u'/' + keyword->commandId : QString();
    m_refresh = m_haveResults && !m_replacePending && text == m_shownText && m_scope == m_shownScope
        && fileExtensions() == m_shownExtensions && rankBy() == m_shownRankBy && nameOrder() == m_shownOrder && m_kindOrder == m_shownKindOrder
        && m_preferredTypes == m_shownPreferredTypes && keywordKey == m_shownKeyword
        && (m_scope != Content || (m_contentFolder == m_shownContentFolder && pickKey(m_contentPick) == m_shownPick))
        && (!text.isEmpty() || m_startList == m_shownStartList) && panelKey() == m_shownPanelKey;
    m_replacePending = !m_refresh;
    // The text is the one shown (another scope, order, kind of file): nobody
    // is typing, so a content scan starts at once, without waiting for a pause.
    m_contentAtOnce = m_refresh || text == m_shownText;
    // A page more of what is listed, or (anything else asking again) the
    // pages there are; a new list starts with one.
    m_loadingMore = more && m_refresh;
    m_hintRunning.clear(); // superseded, as any search is by what follows
    m_walkRunning = false;
    if (!m_refresh) {
        m_pages = 1;
        m_moreNames = false;
        m_moreContent = false;
        m_leftOut = {};
        m_drives.clear();
        m_hintsTaken.clear();
        m_leftOutFound.clear();
        m_approximate = false;
    }
    // The plugins are asked first: a keyword makes the list theirs, and
    // then neither names nor contents are searched (nothing waits for them
    // either way: the window shows with whatever it has). As typed: the
    // space after a keyword is what opens its command (Catalog::match).
    startPluginQuery(m_query, std::move(keyword));
    suggest(); // after the plugins: a plugin's keyword takes the list, and no site is asked
    m_withContent = m_contentEnabled && m_scope == All && !contentNeedle().isEmpty();
    m_unconfirmed.clear();
    // Quietly, unless the rows came from a content scan that was cut short
    // (window hidden mid-scan): that one still shows as the search it is.
    const bool scans = m_scope == Content || m_withContent;
    m_quiet = m_refresh && (!scans || m_scanComplete) && !m_loadingMore;
    if (!m_refresh)
        m_scanComplete = false;
    if (!m_quiet) { // a quiet refresh keeps showing the last scan's figures
        m_contentScanned = 0;
        m_contentTotal = 0;
    }

    if (m_keyword) { // the command's list: its answer replaces the rows (onPluginResults)
        m_engine->cancel();
        m_requestId = 0;
        m_pending = false;
        m_contentRunning = false;
        refreshStatus();
        if (m_keyword->pluginId == kShotPlugin) // the screenshots: in memory, at once
            showPluginRows(screenshotRows(m_keyword->words));
        return;
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
            // Asked again, the rows not found again go (not those a one-off
            // brought: this scan does not read those files); for a page more,
            // what is listed stays (the new scan finds those first).
            if (!m_loadingMore) {
                for (int row = 0; row < m_results.count(); ++row) {
                    if (!m_leftOutFound.contains(m_results.at(row)->path))
                        m_unconfirmed.insert(m_results.at(row)->path);
                }
            }
            startContentSearch(); // at once: nobody is typing
        } else if (m_contentAtOnce) {
            startContentSearch(); // switched to 内容 with the text as it was
        } else {
            m_contentDebounce.start();
        }
        refreshStatus();
        return;
    }

    SearchEngine::Request request;
    request.text = m_query;
    request.scope = static_cast<ws::Scope>(m_scope);
    request.extensions = fileExtensions();
    request.kindOrder = m_kindOrder;
    request.rankBy = rankBy();
    request.preferredTypes = m_preferredTypes;
    request.excludedTypes = m_excludedTypes;
    request.startList = m_startList == RecentlyModified ? ws::StartList::Modified : ws::StartList::Opened;
    request.limit = kPage * m_pages;
    if (m_panel) { // its filters and order; nothing typed lists everything
        request.filter = panelFilter();
        if (!m_folderFilter.isEmpty())
            request.folders = {m_folderFilter};
        request.browse = true;
    }
    if (m_panel || !text.isEmpty()) // nothing typed lists the start list, in its own order
        request.order = nameOrder();
    // With no start list, nothing opened shows before something is typed.
    if (m_recordHistory && !(text.isEmpty() && m_startList == NoStartList))
        request.history = m_history->ranked(text, QDateTime::currentMSecsSinceEpoch());
    request.web = m_web;
    if (m_favorites)
        request.favorites = m_favorites->items();
    // Keep awake is Win顺's own state: while it is on it is listed first, to be seen and ended.
    if (m_switches) {
        const SwitchState awake = m_switches->state(u"keep-awake");
        if (awake.ready() && awake.on == SwitchState::Value::On)
            request.pinnedPlaces = {placePath(u"winshun/switch-keep-awake"_s)};
    }
    m_requestId = m_engine->submit(std::move(request)); // also stops a running content scan
    m_nameRequestId = m_requestId;
    m_pending = true;
    m_contentRunning = false;
    if (m_replacePending)
        m_slowSearch.start();
    refreshStatus();
}

// Rows for the current query and scope. Replacing another query's rows
// moves the selection back to the first one; a refresh keeps it.
void Launcher::showRows(SearchResults rows, QStringList highlights)
{
    // The site's suggestions under its search's row, as they were: a new
    // list for the next word typed keeps them until the site answers for it
    // (rows coming and going at every key would make the window jump).
    if (!m_suggestRows.isEmpty() && !m_keyword) {
        rows.removeIf([](const SearchResult& r) { return r.suggestion; });
        if (const std::optional<TypedKeyword> typed = typedKeyword(m_web, m_query)) {
            const QString path = webPath(m_web[typed->index].keyword, typed->words);
            for (qsizetype i = 0; i < rows.size(); ++i) {
                if (rows[i].path == path) {
                    for (qsizetype s = 0; s < m_suggestRows.size(); ++s)
                        rows.insert(i + 1 + s, m_suggestRows[s]);
                    break;
                }
            }
        }
    }
    m_results.assign(std::move(rows), std::move(highlights));
    m_shownText = m_query.trimmed();
    m_shownScope = m_scope;
    m_shownExtensions = fileExtensions();
    m_shownRankBy = rankBy();
    m_shownOrder = nameOrder();
    m_shownStartList = m_startList;
    m_shownKindOrder = m_kindOrder;
    m_shownPreferredTypes = m_preferredTypes;
    m_shownKeyword = m_keyword ? m_keyword->pluginId + u'/' + m_keyword->commandId : QString();
    m_shownContentFolder = m_contentFolder;
    m_shownPick = pickKey(m_contentPick);
    m_shownPanelKey = panelKey();
    m_haveResults = true;
    if (m_replacePending) {
        m_replacePending = false;
        m_slowSearch.stop();
        m_searchSlow = false;
        emit resultsReplaced();
        if (const auto held = std::exchange(m_held, std::nullopt))
            emit heldKey(held->first, held->second); // on the rows it was pressed for
    }
    // Plugins that answered before the names: their rows go in now, among
    // these (after the first row is current again, so they are placed by it).
    if (m_merge && !m_merge->keywordMode()) {
        m_merge->namesShown();
        if (m_merge->takeBetterFirstRow())
            emit betterFirstRow();
    }
    if (!m_keyword)
        placeScreenshotHint();
}

// Keyword mode: the command's answer is the list (ModelSink::assign).
void Launcher::showPluginRows(SearchResults rows)
{
    showRows(std::move(rows), m_keyword ? ws::parseQuery(m_keyword->words).highlights : QStringList());
    refreshStatus();
}

// Only for a site's keyword followed by words: nothing typed otherwise ever
// leaves the computer, and these words go to the site the user is about to
// search anyway.
void Launcher::suggest()
{
    std::optional<TypedKeyword> typed;
    if (m_webSuggestions && m_scope == All && !m_keyword && !m_panel)
        typed = typedKeyword(m_web, m_query);
    const WebShortcut* web = typed && !typed->words.isEmpty() ? &m_web[typed->index] : nullptr;
    if (!web || web->suggestUrl(typed->words).isEmpty()) {
        m_suggester.cancel();
        dropSuggestions();
        return;
    }
    if (!m_suggestRows.isEmpty() && m_suggestRows.first().keyword != web->keyword)
        dropSuggestions(); // another site's: not under this one's row
    // One of them typed out in full is the search's own row now.
    m_suggestRows.removeIf([&](const SearchResult& r) { return r.words.compare(typed->words, Qt::CaseInsensitive) == 0; });
    m_suggester.ask(web->suggest, typed->words);
}

void Launcher::onSuggestions(const QString& endpoint, const QString& words, const QStringList& suggestions)
{
    const std::optional<TypedKeyword> typed = m_scope == All && !m_keyword ? typedKeyword(m_web, m_query) : std::nullopt;
    if (!typed || typed->words != words || m_web[typed->index].suggest != endpoint)
        return; // typed on meanwhile: that is being asked
    const WebShortcut& web = m_web[typed->index];
    SearchResults rows;
    for (const QString& s : suggestions) {
        // A web search's row, as SearchEngine makes one (fromWeb): Enter
        // searches the site for it, as typed out.
        SearchResult r;
        r.name = web.name;
        r.path = webPath(web.keyword, s);
        r.app = AppKind::Web;
        r.target = web.searchUrl(s);
        r.keyword = web.keyword;
        r.words = s;
        r.icon = web.iconRef();
        r.suggestion = true;
        r.exact = true; // a plugin's row goes after them, as after the search's own (plugin::Merge::placeFor)
        r.score = 1000; // the search's own row's (SearchEngine's kTypedKeywordScore): above any plugin's 0–100
        rows.append(r);
    }
    m_suggestRows = rows;
    placeSuggestions();
}

// Under the search's row, in place of those there: rows the same in the
// same place are not touched, so the list does not flicker as they come.
void Launcher::placeSuggestions()
{
    const std::optional<TypedKeyword> typed = typedKeyword(m_web, m_query);
    const int at = typed ? m_results.indexOf(webPath(m_web[typed->index].keyword, typed->words)) : -1;
    if (at < 0)
        return; // the rows are still the last query's: showRows puts them in
    int had = 0;
    while (const SearchResult* r = m_results.at(at + 1 + had)) {
        if (!r->suggestion)
            break;
        ++had;
    }
    const int now = static_cast<int>(m_suggestRows.size());
    for (int i = 0; i < std::min(had, now); ++i) {
        if (*m_results.at(at + 1 + i) != m_suggestRows[i])
            m_results.update(at + 1 + i, m_suggestRows[i]);
    }
    for (int i = had; i < now; ++i)
        m_results.insert(at + 1 + i, m_suggestRows[i]);
    // By place: while being rewritten, two rows may have had one path.
    m_results.removeRange(at + 1 + now, had - now);
    refreshKeyHints();
    refreshStatus();
}

void Launcher::dropSuggestions()
{
    m_suggestRows.clear();
    bool removed = false;
    for (int row = m_results.count() - 1; row >= 0; --row) {
        if (m_results.at(row)->suggestion) {
            m_results.removeRange(row, 1); // by place, not by path: the search's own row may share one
            removed = true;
        }
    }
    if (removed) {
        refreshKeyHints();
        refreshStatus();
    }
}


// 全部 looks for the typed text inside files too, as the 内容 scope does
// (contentTerms: the words, "a phrase", or a line of code as it is). Not for
// name syntax (ext:, wildcards, !exclusions: "*.log", "report !draft"),
// unless the text reads as code anyway ("if (!ok)", "char* p = s;"). Nor
// after a web shortcut's keyword: that is a search on the web ("gh
// WinShun"), and reading files for it only costs time. Nor for a path
// ("C:\Users\me\x"): that is a place to go to. Nor for a favorite's keyword.
QString Launcher::contentNeedle() const
{
    const QString text = m_query.trimmed();
    const ParsedQuery query = ws::parseQuery(text);
    if (query.terms.empty() || m_keyword || typedKeyword(m_web, m_query) || !pathtext::splitTyped(text).folder.isEmpty())
        return {};
    if (m_favorites && (typedFavoriteKeyword(m_favorites->items(), text) >= 0
        || typedCommandKeyword(m_favorites->items(), text) >= 0))
        return {};
    const bool nameSyntax = !query.extensions.empty()
        || std::any_of(query.terms.cbegin(), query.terms.cend(), [](const QueryTerm& t) { return t.negated || t.wildcard; });
    if (nameSyntax && !ws::looksLikeCode(text))
        return {};
    return text;
}

void Launcher::startContentSearch()
{
    SearchEngine::Request request;
    request.text = m_scope == Content ? m_query.trimmed() : contentNeedle();
    request.scope = ws::Scope::Content;
    request.rankBy = rankBy();
    request.excludedTypes = m_excludedTypes;
    request.maxContentResults = kPage * m_pages;
    request.contentExtensions = m_contentExtensions;
    request.contentSizeLimits = m_contentSizeLimits;
    request.contentDocuments = m_contentDocuments;
    if (m_panel) {
        request.filter = panelFilter();
        if (!m_folderFilter.isEmpty())
            request.contentFolders = {m_folderFilter};
    }
    if (m_scope == Content) {
        // Asked for: system and program folders too if so set, and many
        // files at once (each open mostly waits for the antivirus).
        request.contentInLowPriority = m_contentInLowPriority;
        request.contentThreads = kContentThreads;
        setContentPlace(request);
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

    // More to list than came: names beyond the page, or the files written
    // lately filling it (the favorites before them not counted).
    if (m_query.trimmed().isEmpty()) {
        m_moreNames = m_startList == RecentlyModified
            && std::ranges::count_if(results, [](const SearchResult& r) { return !r.favorite; }) >= kPage * m_pages;
    } else {
        m_moreNames = total > results.size();
    }
    if (m_loadingMore) {
        // A page more: what is not listed yet goes after the rows, in its
        // order; nothing listed moves (it is where its user is looking).
        SearchResults fresh;
        for (const SearchResult& r : results) {
            if (!r.isPlugin() && m_results.indexOf(r.path) < 0)
                fresh.append(r);
        }
        m_results.append(fresh);
        if (m_withContent)
            startContentSearch(); // its page more too
        else
            m_loadingMore = false;
        refreshStatus();
        return;
    }

    SearchResults rows = results;
    QSet<QString> names;
    if (m_refresh) {
        for (const SearchResult& r : rows)
            names.insert(r.path);
    }
    if (m_refresh && m_withContent) {
        // Keep last scan's content matches below the names until the new
        // scan finds them again (or finishes without them), rather than
        // dropping them only to add them back a moment later.
        // By time, each among the files where it belongs.
        const RankBy by = rankBy();
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r->line > 0 && !names.contains(r->path)) {
                const int at = by == RankBy::Match
                    ? static_cast<int>(rows.size())
                    : timePlace(static_cast<int>(rows.size()), [&](int i) -> const SearchResult& { return rows.at(i); },
                          *r, 0, by, m_kindOrder);
                rows.insert(at, *r);
                if (!m_leftOutFound.contains(r->path)) // a one-off brought it: the scan will not read that file
                    m_unconfirmed.insert(r->path);
            }
        }
    }
    if (m_refresh) {
        // Likewise the rows a walk of a drive brought (searchLeftOut): the
        // index has nothing to find them by again. After the names, as they came.
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r->line == 0 && !r->isPlugin() && m_leftOutFound.contains(r->path) && !names.contains(r->path))
                rows.append(*r);
        }
    }
    if (m_refresh) {
        // Likewise the plugins' rows, where they are: their new answer
        // rewrites them in place (plugin::Merge) instead of taking them
        // out and putting them back.
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r->isPlugin())
                rows.insert(std::min<qsizetype>(row, rows.size()), *r);
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
        if (m_refresh || m_contentAtOnce)
            startContentSearch();
        else
            m_contentDebounce.start();
    }
    // Rows found by how their names read (nothing is named as typed): the
    // heading says so (approximateHeading), and the names are highlighted by
    // the pinyin, as the result list reads "bg" back into 报告.
    m_approximate = std::ranges::any_of(results, [](const SearchResult& r) { return r.approximate; });
    const ParsedQuery query = ws::parseQuery(m_query);
    QStringList highlights = query.highlights;
    if (m_approximate) {
        highlights.clear();
        for (const ParsedQuery& reading : ws::homophoneQueries(query)) {
            for (const QString& word : reading.highlights) {
                if (!highlights.contains(word))
                    highlights.append(word);
            }
        }
    }
    showRows(std::move(rows), std::move(highlights));
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
    if (!m_hintRunning.isEmpty()) {
        for (const SearchResult& r : fresh)
            m_leftOutFound.insert(r.path);
        m_hintHits += static_cast<int>(fresh.size());
    }
    if (m_loadingMore) { // a page more: after the rows, as they come (loadMore)
        m_results.append(fresh);
        return;
    }
    // Each among the content rows by how well it matches (they come best
    // first, a tie after those already there); by time, among the files
    // (timePlace). Never above the row the keyboard is on, nor the rows
    // above it: they stay where they are seen; on the first row, a better
    // match takes its place.
    const int current = m_currentRow;
    const int floor = current > 0 ? current + 1 : 0;
    const RankBy by = rankBy();
    bool aboveFirst = false;
    for (const SearchResult& r : fresh) {
        int row = m_results.count();
        if (by != RankBy::Match) {
            row = timePlace(
                row, [&](int i) -> const SearchResult& { return *m_results.at(i); }, r, floor, by, m_kindOrder);
        } else {
            while (row > floor) {
                const SearchResult* before = m_results.at(row - 1);
                if (before->line == 0 || before->relevance >= r.relevance)
                    break; // a name match (they come first), or as good
                --row;
            }
        }
        aboveFirst |= row == 0 && current == 0;
        m_results.insert(row, r);
    }
    if (aboveFirst)
        emit betterFirstRow();
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
    if (finished && !m_hintRunning.isEmpty()) {
        // A part read after all (searchLeftOut): the scan behind the rows
        // is as it was (this one may have stopped at its limit, which says
        // nothing about that one).
        finishLeftOut(false);
    } else if (finished) {
        m_scanComplete = true;
        m_loadingMore = false;
        // Stopped at its limit before every file was read: more to list.
        m_moreContent = scanned < total;
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

void Launcher::onContentLeftOut(quint64 id, const ContentLeftOut& leftOut)
{
    if (id != m_requestId || !m_hintRunning.isEmpty())
        return; // superseded
    // Shown once the finishing progress arrives (leftOutHints); the parts
    // read already for this query are not offered again.
    m_leftOut = leftOut;
    if (m_hintsTaken.contains(kLargeHint))
        m_leftOut.large.clear();
    if (m_hintsTaken.contains(kSystemHint))
        m_leftOut.lowPriority = 0;
    if (m_hintsTaken.contains(kTypesHint))
        m_leftOut.otherTypes = 0;
}

void Launcher::onRemovableDrives(quint64 id, const RemovableDrives& drives)
{
    if (id != m_nameRequestId || drives == m_drives)
        return;
    m_drives = drives;
    refreshStatus();
}

// A drive's rows, as the walk finds them: after the rows there are (the hint
// taken up stands there), each file once.
void Launcher::onDriveResults(quint64 id, const SearchResults& results)
{
    if (id != m_requestId)
        return;
    SearchResults fresh;
    for (const SearchResult& r : results) {
        if (m_results.indexOf(r.path) < 0) {
            fresh.append(r);
            m_leftOutFound.insert(r.path);
        }
    }
    m_hintHits += static_cast<int>(fresh.size());
    m_results.append(fresh);
}

void Launcher::onDriveProgress(quint64 id, int seen, bool finished)
{
    if (id != m_requestId)
        return;
    m_walkSeen = seen;
    if (finished) {
        m_walkRunning = false;
        if (!m_hintRunning.isEmpty())
            finishLeftOut(false);
    }
    refreshStatus();
}

// ---- the search panel -------------------------------------------------------------

void Launcher::setPanel(bool on)
{
    if (on == m_panel)
        return;
    m_panel = on;
    emit panelChanged();
    refreshStatus();
    if (m_window && m_window->isVisible() && !m_keyword)
        search(); // the rows as the panel (or the box) lists them; the same ones stay where they are
}

void Launcher::togglePanel()
{
    emit panelToggleRequested();
}

void Launcher::setPinned(bool on)
{
    if (on == m_pinned)
        return;
    m_pinned = on;
    emit pinnedChanged();
    // Said in the footer as well: Ctrl+T leaves no other trace near the keys (short,
    // so the key hints keep their room).
    if (m_window && m_window->isVisible())
        flash(on ? tr("已钉住") : tr("已取消钉住")); // what pinning means is in the pin button's tip
}

bool Launcher::filtersActive() const
{
    return m_dateChoice != 0 || m_sizeChoice != 0 || !m_folderFilter.isEmpty();
}

QVariantList Launcher::favoriteFolders() const
{
    QVariantList folders;
    if (!m_favorites)
        return folders;
    for (const Favorite& f : m_favorites->items()) {
        if (f.kind == Favorite::Kind::Folder)
            folders.append(QVariantMap {{u"name"_s, f.name}, {u"path"_s, f.path}});
    }
    return folders;
}

namespace {

QDate dateOf(const QString& text)
{
    return QDate::fromString(text.trimmed(), Qt::ISODate);
}

FileTime fileTimeOf(const QDateTime& when)
{
    const qint64 secs = when.toSecsSinceEpoch();
    return secs <= 0 ? 0 : secs >= 0xFFFF'FFFFll ? 0xFFFF'FFFFu : static_cast<FileTime>(secs);
}

std::optional<std::uint64_t> megabytesOf(const QString& text)
{
    bool ok = false;
    const double mb = text.trimmed().toDouble(&ok);
    if (!ok || mb < 0 || mb > 1e9)
        return std::nullopt;
    return static_cast<std::uint64_t>(mb * 1048576.0 + 0.5);
}

} // namespace

EntryFilter Launcher::panelFilter() const
{
    EntryFilter f;
    const QDateTime now = QDateTime::currentDateTime();
    const QDate today = now.date();
    switch (m_dateChoice) {
    case 1:
        f.after = fileTimeOf(today.startOfDay());
        break;
    case 2:
        f.after = fileTimeOf(now.addDays(-7));
        break;
    case 3:
        f.after = fileTimeOf(now.addDays(-30));
        break;
    case 4:
        f.after = fileTimeOf(QDate(today.year(), 1, 1).startOfDay());
        break;
    case 5: {
        const QDate from = dateOf(m_dateFrom);
        const QDate to = dateOf(m_dateTo);
        if (from.isValid())
            f.after = fileTimeOf(from.startOfDay());
        if (to.isValid())
            f.before = fileTimeOf(to.addDays(1).startOfDay());
        if (!from.isValid() && !to.isValid())
            f.after = 1; // a bound all the same: times not known stay out, as the choice says
        break;
    }
    default:
        break;
    }
    constexpr std::uint64_t mb = 1 << 20;
    switch (m_sizeChoice) {
    case 1:
        f.maxSize = packSize(mb - 1);
        break;
    case 2:
        f.minSize = packSize(mb);
        f.maxSize = packSize(100 * mb);
        break;
    case 3:
        f.minSize = packSize(100 * mb);
        f.maxSize = packSize(1024 * mb);
        break;
    case 4:
        f.minSize = packSize(1024 * mb + 1);
        break;
    case 5: {
        const auto min = megabytesOf(m_sizeMin);
        const auto max = megabytesOf(m_sizeMax);
        if (min)
            f.minSize = packSize(*min);
        if (max)
            f.maxSize = packSize(*max);
        if (!min && !max)
            f.minSize = 1; // a bound all the same (packSize(0) is 0: none)
        break;
    }
    default:
        break;
    }
    return f;
}

QString Launcher::panelKey() const
{
    if (!m_panel)
        return {};
    // Only what changes the rows: the box's list is the panel's when nothing
    // narrows or reorders it, so expanding keeps the rows (and the selection).
    QString key;
    if (filtersActive()) {
        key += u"f:%1,%2,%3,%4,%5,%6,%7;"_s.arg(m_dateChoice).arg(m_dateFrom, m_dateTo).arg(m_sizeChoice).arg(m_sizeMin, m_sizeMax, m_folderFilter);
    }
    if (m_panelSort.key != SortKey::Rank)
        key += u"o:%1,%2;"_s.arg(static_cast<int>(m_panelSort.key)).arg(m_panelSort.descending ? 1 : 0);
    if (m_query.trimmed().isEmpty())
        key += u"browse;"_s;
    return key;
}

void Launcher::setDateChoice(int choice)
{
    choice = std::clamp(choice, 0, 5);
    if (choice == m_dateChoice)
        return;
    m_dateChoice = choice;
    emit filtersChanged();
    if (m_panel)
        search();
}

void Launcher::setDateRange(const QString& from, const QString& to)
{
    if (from == m_dateFrom && to == m_dateTo && m_dateChoice == 5)
        return;
    m_dateFrom = from;
    m_dateTo = to;
    m_dateChoice = 5;
    emit filtersChanged();
    if (m_panel)
        search();
}

void Launcher::setSizeChoice(int choice)
{
    choice = std::clamp(choice, 0, 5);
    if (choice == m_sizeChoice)
        return;
    m_sizeChoice = choice;
    emit filtersChanged();
    if (m_panel)
        search();
}

void Launcher::setSizeRange(const QString& minMb, const QString& maxMb)
{
    if (minMb == m_sizeMin && maxMb == m_sizeMax && m_sizeChoice == 5)
        return;
    m_sizeMin = minMb;
    m_sizeMax = maxMb;
    m_sizeChoice = 5;
    emit filtersChanged();
    if (m_panel)
        search();
}

void Launcher::setFolderFilter(const QString& path)
{
    QString folder = QDir::toNativeSeparators(path.trimmed());
    while (folder.size() > 3 && folder.endsWith(u'\\'))
        folder.chop(1);
    if (folder == m_folderFilter)
        return;
    m_folderFilter = folder;
    emit filtersChanged();
    if (m_panel)
        search();
}

void Launcher::pickFolderFilter()
{
    // The dialog is a window of ours over the launcher: the launcher stays
    // (App's activeChanged lets a command of ours keep it, systemCommandRunning).
    const auto owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    m_systemCommand = true;
    const QString path = shell::pickFolder(owner, tr("选择要在其中搜索的文件夹"));
    m_systemCommand = false;
    if (m_window)
        m_window->requestActivate();
    if (!path.isEmpty())
        setFolderFilter(path);
}

void Launcher::clearFilters()
{
    if (!filtersActive())
        return;
    m_dateChoice = 0;
    m_dateFrom.clear();
    m_dateTo.clear();
    m_sizeChoice = 0;
    m_sizeMin.clear();
    m_sizeMax.clear();
    m_folderFilter.clear();
    emit filtersChanged();
    if (m_panel)
        search();
}

void Launcher::sortBy(int key)
{
    const auto sortKey = static_cast<SortKey>(std::clamp(key, 0, 4));
    SortOrder order;
    if (sortKey == m_panelSort.key && sortKey != SortKey::Rank) {
        order = {sortKey, !m_panelSort.descending};
    } else {
        // Names and paths up (a to z); sizes and times down (the largest, the newest first).
        order = {sortKey, sortKey == SortKey::Size || sortKey == SortKey::Modified};
    }
    if (order == m_panelSort)
        return;
    m_panelSort = order;
    emit panelSortChanged();
    if (m_panel && m_window && m_window->isVisible() && !m_keyword)
        search();
}

void Launcher::refreshStatus()
{
    // Only what is worth reading: the left of the footer is the order's
    // switch, and how far a content scan is shows on the divider above
    // (progress()). Not the figures of a search that went as it should.
    const auto state = m_index->state();
    const int items = static_cast<int>(m_index->itemCount());
    QString s;
    if (m_query.trimmed().isEmpty() || (!m_haveResults && !m_contentRunning)) {
        switch (state) {
        case IndexService::State::Idle:
        case IndexService::State::Loading:
            s = tr("正在加载索引…");
            break;
        case IndexService::State::Building:
            s = tr("正在建立索引… 已收录 %Ln 项", nullptr, items);
            if (const QString drives = readingDrives(*m_index); !drives.isEmpty())
                s += tr(" · 正在读 %1").arg(drives); // a hard disk takes a while: which one it is
            break;
        case IndexService::State::Ready:
            break;
        }
    } else if (m_scope == Content) {
        // Nothing to say: the divider shows the scan, and more matches than
        // listed come as the list is scrolled (loadMore).
    } else if (m_keyword) {
        // The command's list: nothing of the wait for the first 150 ms, then
        // who is being waited for; its failure for a few seconds.
        if (!m_pluginError.isEmpty())
            s = m_pluginError;
        else if (!m_pluginPending.isEmpty() && m_pluginSlow)
            s = pluginWaiting();
    } else {
        QStringList parts;
        if (m_searchSlow && m_replacePending)
            parts.append(tr("正在搜索…")); // the rows on screen are the last query's
        if (state == IndexService::State::Building)
            parts.append(tr("索引尚未建完"));
        // Plugins pushing into the list: who is still being waited for
        // (once it takes a while), who gave up.
        if (!m_pluginPending.isEmpty() && m_pluginSlow) {
            QStringList names;
            for (const QString& id : m_pluginPending)
                names.append(pluginName(id));
            names.sort();
            parts.append(tr("正在等 %1…").arg(names.join(tr("、"))));
        }
        if (!m_pluginError.isEmpty())
            parts.append(m_pluginError);
        s = parts.join(u" · "_s);
    }
    // The panel says how many there are in all (a browse of 3 million files
    // lists 256 at a time): a figure worth reading there.
    if (m_panel && m_haveResults && !m_replacePending && !m_keyword && m_scope != Content && m_totalMatches > 0) {
        const QString count = tr("共 %Ln 项", nullptr, totalMatches());
        s = s.isEmpty() ? count : count + u" · "_s + s;
    }
    if (const int selected = m_results.selectedCount(); selected > 0)
        s = tr("已选择 %Ln 项", nullptr, selected);
    if (s != m_status) {
        m_status = s;
        emit statusChanged();
    } else {
        emit statusChanged(); // busy/searching may have changed
    }
    refreshKeyHints(); // the rows changed with whatever brought us here
    walkHereSoon();
}

void Launcher::setCurrentRow(int row)
{
    if (row == m_currentRow)
        return;
    m_currentRow = row;
    emit currentRowChanged();
    refreshKeyHints();
    followPreview();
}

// What the keys do on the row the keyboard is on (keyHints), from the same
// cases as menuItems(). Enter on a plugin's row is its first action by
// name, else what its default does; the second key is its second action,
// or 打开位置 for a row with a location; Tab completes the row that offers
// it (the hint row of a keyword, completionRow), else switches the scope,
// which means nothing in keyword mode.
void Launcher::refreshKeyHints()
{
    QString enter;
    QString secondKeys;
    QString second;
    // Tab: into a favorite folder in 全部, a completion, else the scopes (the
    // strip under the box is Ctrl+Tab's, GitHub #16, #29).
    const bool narrowing = (m_scope == Files || m_scope == Folders || m_scope == Content) && !m_keyword;
    QString tab = enterableFolder(m_currentRow) && !narrowing ? tr("进入文件夹")
        : completionRow(m_currentRow)                          ? tr("补全")
        : m_keyword                                            ? QString()
                                                               : tr("切换范围");
    QString tabKeys = u"Tab"_s; // the volume's row: its place is Alt+←→'s (Tab still does the same)
    const SearchResult* r = m_results.at(m_currentRow);
    if (!r) {
        // No row, nothing to open: only Tab (it still switches the scope) is offered.
    } else if (r->isPlugin()) {
        const PluginRow& p = *r->plugin;
        if (p.valid) {
            if (!p.actions.isEmpty()) {
                enter = p.actions[0].title;
                if (p.actions.size() > 1) {
                    secondKeys = p.actions[1].key.isEmpty() ? u"Ctrl+Enter"_s : p.actions[1].key;
                    second = p.actions[1].title;
                }
            } else {
                enter = p.openKind == PluginRow::OpenKind::Url ? (p.openTarget.compare(u"winshun:settings", Qt::CaseInsensitive) == 0 ? tr("打开设置") : tr("打开网址"))
                    : p.openKind == PluginRow::OpenKind::Path ? tr("打开")
                    : !p.searchQuery.isEmpty()                ? tr("搜索")
                    : plugin::isHintRow(*r)                   ? tr("进入")
                    : !p.setQuery.isEmpty()                   ? tr("填入搜索框")
                                                              : tr("复制"); // the plugin's default action copies the row's text
            }
        }
    } else if (r->isWeb()) {
        enter = r->words.isEmpty() ? tr("打开网站") : tr("搜索");
    } else if (r->isPlace()) {
        enter = tr("打开");
        if (!r->switchId.isEmpty())
            switchKeyHints(*r, enter, secondKeys, second);
        if (steppable(*r)) {
            tabKeys = u"Alt+←→"_s;
            tab = tr("调音量");
        }
    } else if (r->isApp()) {
        enter = tr("打开");
        if (!r->target.isEmpty()) {
            secondKeys = u"Ctrl+Enter"_s;
            second = r->isPackagedApp() ? tr("打开安装文件夹") : tr("打开位置");
        }
    } else {
        enter = r->isDir ? tr("打开文件夹") : tr("打开");
        secondKeys = u"Ctrl+Enter"_s;
        second = tr("打开位置");
    }
    const QVariantMap hints {{u"enter"_s, enter}, {u"secondKeys"_s, secondKeys}, {u"second"_s, second}, {u"tab"_s, tab},
        {u"tabKeys"_s, tabKeys}};
    if (hints == m_keyHints)
        return;
    m_keyHints = hints;
    emit keyHintsChanged();
}

// The words of Enter and of the second key on a system switch's row: the
// state it goes to, not "打开" (that means a file here, and "Esc 关闭" is
// the window). Before the first read, "切换"; a switch that cannot be
// switched here (no device, not allowed) opens its page; one without a
// backend is an ordinary place.
void Launcher::switchKeyHints(const SearchResult& row, QString& enter, QString& secondKeys, QString& second) const
{
    using Kind = SwitchIntent::Kind;
    if (!m_switches)
        return;
    if (row.intent.kind == Kind::OpenSettings) { // "蓝牙设置": the page is what was asked for
        enter = ResultModel::switchWords(row, row.intent, false);
        return;
    }
    if (!liveSwitch(row))
        return;
    if (!drawsSwitch(row)) {
        enter = tr("打开设置");
        return;
    }
    const SwitchState state = m_switches->state(row.switchId);
    const SwitchIntent resolved = m_switches->resolve(row.switchId, row.intent);
    enter = ResultModel::switchWords(row, resolved, false);
    if (row.switchId == u"theme" && state.on == SwitchState::Value::Mixed && row.intent.kind == Kind::Toggle)
        enter = resolved.kind == Kind::On ? tr("全部切到深色") : tr("全部切到浅色");
    if (row.intent.kind == Kind::SetLevel && row.intent.value > 100) // "音量 150"
        enter = tr("%1（最大 100）").arg(enter);
    if (resolved.kind == Kind::Off && state.carriesInternet) // WLAN off, the internet with it: said, not asked
        enter = tr("%1（会断网）").arg(enter);
    secondKeys = u"Ctrl+Enter"_s;
    second = row.switchId == u"keep-awake" ? tr("电源设置") : tr("打开设置");
}

void Launcher::flash(const QString& message)
{
    m_flash = message;
    m_flashTimer.start();
    emit statusChanged();
}

void Launcher::cycleScope(int delta)
{
    static constexpr Scope kOrder[] {All, Files, Content}; // as the tabs show them (文件夹 within 文件)
    const int scopes = static_cast<int>(std::size(kOrder)) - (m_contentEnabled ? 0 : 1); // Content last
    const Scope tab = m_scope == Folders ? Files : m_scope;
    const int at = static_cast<int>(std::find(std::begin(kOrder), std::end(kOrder), tab) - std::begin(kOrder));
    setScope(kOrder[((at + delta) % scopes + scopes) % scopes]);
}

QVariantList Launcher::fileTypes() const
{
    QVariantList types;
    for (const FileType& type : m_fileTypes)
        types.append(QVariantMap {{u"name"_s, type.name}, {u"extensions"_s, type.extensions.join(u' ')}});
    return types;
}

void Launcher::setFileTypes(const FileTypes& types)
{
    if (types == m_fileTypes)
        return;
    // The kind chosen stays chosen where it went (moved up, renamed: by its
    // id or name); gone, any kind.
    const QString chosen = m_fileType >= 0 ? fileTypeKey(m_fileTypes[m_fileType]) : QString();
    m_fileTypes = types;
    const int at = chosen.isEmpty() ? -1 : fileTypeIndex(chosen);
    if (at != m_fileType) {
        m_fileType = at;
        emit fileTypeChanged();
        emit fileChoiceChanged();
    }
    emit fileTypesChanged();
    emit scopeChanged(); // the placeholder names the kind
    // Its extensions changed: listed again (a refresh when they did not).
    if (m_fileType >= 0 && m_window && m_window->isVisible())
        search();
}

QStringList Launcher::fileExtensions() const
{
    return m_scope == Files && m_fileType >= 0 ? m_fileTypes[m_fileType].extensions : QStringList();
}

void Launcher::chooseFileType(int type)
{
    const Scope scope = type == kFoldersChip ? Folders : Files;
    if (type < -1 || type >= m_fileTypes.size())
        type = -1;
    if (scope == m_scope && type == m_fileType)
        return;
    m_scope = scope;
    if (type != m_fileType || m_foldersChosen != (scope == Folders)) {
        m_fileType = type;
        m_foldersChosen = scope == Folders;
        emit fileTypeChanged();
        emit fileChoiceChanged();
    }
    emit scopeChanged();
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints();
    search(); // the rows stay until the new ones come (see setScope)
}

void Launcher::cycleFileType(int delta)
{
    QList<int> chips {-1, kFoldersChip};
    for (int i = 0; i < m_fileTypes.size(); ++i)
        chips.append(i);
    const int at = std::max(0, static_cast<int>(chips.indexOf(m_scope == Folders ? kFoldersChip : m_fileType)));
    const int count = static_cast<int>(chips.size());
    chooseFileType(chips[((at + delta) % count + count) % count]);
}

void Launcher::editFileTypes()
{
    emit fileTypeSettingsRequested();
}

QString Launcher::fileTypeKey(const FileType& type)
{
    return type.id.isEmpty() ? u"name:"_s + type.name : type.id;
}

int Launcher::fileTypeIndex(const QString& key) const
{
    for (int i = 0; i < m_fileTypes.size(); ++i) {
        if (fileTypeKey(m_fileTypes[i]) == key)
            return i;
    }
    return -1;
}

QString Launcher::fileChoice() const
{
    return m_foldersChosen ? u"folders"_s : m_fileType >= 0 ? fileTypeKey(m_fileTypes[m_fileType]) : QString();
}

void Launcher::setFileChoice(const QString& choice)
{
    const int type = choice == u"folders" || choice.isEmpty() ? -1 : fileTypeIndex(choice);
    if (m_scope == Files || m_scope == Folders) {
        chooseFileType(choice == u"folders" ? kFoldersChip : type);
        return;
    }
    m_foldersChosen = choice == u"folders";
    if (type != m_fileType) {
        m_fileType = type;
        emit fileTypeChanged();
    }
}

namespace {

constexpr QStringView kAllChip = u"all"; // every folder the name typed stands for
constexpr QStringView kPinPrefix = u"pin:"; // a folder chosen: m_contentFolder
constexpr QStringView kPickPrefix = u"pick:"; // one of those the name typed stands for: m_contentPick
constexpr QStringView kMissingChip = u"missing"; // a place not in the index: nothing to choose
constexpr QStringView kTypedChip = u"typed"; // a folder typed, not made out yet
constexpr QStringView kFolderDot = u"#E8B84A"; // as the strip's 文件夹 in 文件

// The last few names of each path, as many as tell them apart (up to
// three): "项目\abc", "备份\abc".
QStringList shortLabels(const QStringList& paths)
{
    QList<QStringList> parts;
    for (const QString& path : paths)
        parts.append(path.split(u'\\', Qt::SkipEmptyParts));
    const auto label = [&](qsizetype i, qsizetype names) {
        const QStringList& p = parts[i];
        if (p.isEmpty())
            return paths[i];
        const qsizetype n = std::min(names, p.size());
        QString text = p.sliced(p.size() - n).join(u'\\');
        return n == 1 && p.size() == 1 ? text + u'\\' : text; // "D:\"
    };
    QStringList labels;
    for (qsizetype i = 0; i < paths.size(); ++i) {
        qsizetype names = 1;
        for (; names < 3; ++names) {
            const QString mine = label(i, names);
            bool same = false;
            for (qsizetype j = 0; j < paths.size() && !same; ++j)
                same = j != i && label(j, names).compare(mine, Qt::CaseInsensitive) == 0;
            if (!same)
                break;
        }
        labels.append(label(i, names));
    }
    return labels;
}

} // namespace

QString Launcher::folderLabel(const QString& path)
{
    return shortLabels({path}).value(0, path);
}

QString Launcher::pickKey(const SearchEngine::Request::ContentPick& pick)
{
    return pick.folder.isEmpty() ? QString() : pick.typed + u'\n' + pick.folder;
}

void Launcher::setContentPlace(SearchEngine::Request& request) const
{
    if (!m_contentFolder.isEmpty())
        request.contentFolders = {m_contentFolder}; // the strip's choice, before the panel's folder
    request.contentPick = m_contentPick;
}

const ContentScope::Place* Launcher::typedPlace() const
{
    if (m_scope != Content)
        return nullptr;
    const ContentQuery typed = splitContentQuery(m_query.trimmed());
    if (typed.folders.empty())
        return nullptr;
    for (const ContentScope::Place& place : m_contentScope.places) {
        if (place.typed == typed.folders.front().typed)
            return &place;
    }
    return nullptr;
}

void Launcher::onContentScope(quint64 id, const ContentScope& scope)
{
    if (id != m_requestId || scope == m_contentScope)
        return;
    m_contentScope = scope;
    // The name picked from is no longer typed: the pick goes with it.
    if (!m_contentPick.folder.isEmpty()
        && std::ranges::none_of(scope.places, [&](const ContentScope::Place& p) { return p.typed == m_contentPick.typed; }))
        m_contentPick = {};
    emit folderChipsChanged();
}

QVariantList Launcher::folderChips() const
{
    QVariantList chips;
    const auto chip = [&](QStringView key, const QString& name, const QString& tip, bool folder) {
        chips.append(QVariantMap {{u"type"_s, key.toString()}, {u"name"_s, name}, {u"tip"_s, tip},
            {u"dot"_s, folder ? kFolderDot.toString() : QString()}});
    };
    if (m_scope != Content)
        return chips;
    // A folder named among the words: that one, or the ones it stands for.
    const ContentQuery typed = splitContentQuery(m_query.trimmed());
    if (!typed.folders.empty()) {
        const ContentFolderSpec& spec = typed.folders.front();
        const QString label = spec.path.isEmpty() ? spec.names.join(u'\\') : spec.path;
        const ContentScope::Place* place = typedPlace();
        if (!place) {
            chip(kTypedChip, label, spec.path.isEmpty() ? tr("名字是 %1 的文件夹").arg(label) : label, true);
            return chips;
        }
        using Status = ContentScope::Status;
        switch (place->status) {
        case Status::NotFound:
            chip(kMissingChip, tr("%1 不在索引里").arg(label), tr("这个文件夹不存在、被排除在索引之外，或在没有收录的磁盘上"), false);
            return chips;
        case Status::OnShare:
            chip(kMissingChip, tr("%1 在网络共享上").arg(label), tr("网络共享上的文件不搜内容"), false);
            return chips;
        case Status::Words:
            break; // no folder so named: what was typed is among the words, the strip as without it
        case Status::Searched: {
            const QStringList labels = shortLabels(place->folders);
            if (place->count > 1) {
                const QString all = place->approximate ? tr("名字含 %1 的 %Ln 个文件夹", nullptr, place->count).arg(label)
                                                       : tr("所有 %1 · %Ln", nullptr, place->count).arg(label);
                const QString tip = place->approximate ? tr("没有名字正好是 %1 的文件夹，这些名字里含有它").arg(label)
                    : place->count > place->folders.size()
                    ? tr("在名字是 %1 的 %Ln 个文件夹里都搜（下面列出前 %2 个）", nullptr, place->count).arg(label).arg(place->folders.size())
                    : tr("在名字是 %1 的 %Ln 个文件夹里都搜", nullptr, place->count).arg(label);
                chip(kAllChip, all, tip, false);
            }
            for (qsizetype i = 0; i < place->folders.size(); ++i)
                chip(kPickPrefix.toString() + place->folders[i], labels[i], place->folders[i], true);
            return chips;
        }
        }
    }
    chip(QString(), tr("不限"), tr("在所有文件里搜"), false);
    const auto add = [&](const QString& path, const QString& name) {
        chip(kPinPrefix.toString() + path, name, path, true);
    };
    if (!m_contentFolder.isEmpty() && (!m_favorites || indexOfFavorite(m_favorites->items(), m_contentFolder) < 0))
        add(m_contentFolder, folderLabel(m_contentFolder)); // chosen from a row: first, after 不限
    if (m_favorites) {
        for (const Favorite& f : m_favorites->items()) {
            if (f.kind == Favorite::Kind::Folder)
                add(f.path, f.name.isEmpty() ? folderLabel(f.path) : f.name);
        }
    }
    return chips;
}

QString Launcher::folderChip() const
{
    if (m_scope != Content)
        return {};
    if (const ContentScope::Place* place = typedPlace()) {
        using Status = ContentScope::Status;
        if (place->status == Status::NotFound || place->status == Status::OnShare)
            return kMissingChip.toString();
        if (place->status == Status::Searched) {
            if (!place->chosen.isEmpty())
                return kPickPrefix.toString() + place->chosen;
            return place->count > 1 || place->folders.isEmpty() ? kAllChip.toString() : kPickPrefix.toString() + place->folders.first();
        }
    } else if (!splitContentQuery(m_query.trimmed()).folders.empty()) {
        return kTypedChip.toString();
    }
    return m_contentFolder.isEmpty() ? QString() : kPinPrefix.toString() + m_contentFolder;
}

QString Launcher::folderHint() const
{
    if (m_scope != Content)
        return {};
    if (const ContentScope::Place* place = typedPlace()) {
        if (place->status == ContentScope::Status::Searched)
            return place->count > 1 ? tr("Ctrl+Tab 选其中一个") : QString();
        if (place->status == ContentScope::Status::Words) { // "/usr": searched for as it is
            QString name = place->typed;
            name.removeIf([](QChar c) { return c == u'/' || c == u'\\' || c == QChar(0xFF0F) || c == QChar(0xFF3C); });
            return tr("没有叫 %1 的文件夹，当作文字搜").arg(name);
        }
    }
    if (!splitContentQuery(m_query.trimmed()).folders.empty())
        return {};
    return tr("或在词后加“文件夹名/”");
}

QString Launcher::contentWords() const
{
    if (typedPlace())
        return m_contentScope.words;
    return splitContentQuery(m_query.trimmed()).words;
}

QString Launcher::contentWhere() const
{
    // A name no folder has (Words): where the folder chosen, if any, says.
    if (const ContentScope::Place* place = typedPlace(); place && place->status != ContentScope::Status::Words) {
        if (place->status != ContentScope::Status::Searched)
            return {};
        if (!place->chosen.isEmpty())
            return folderLabel(place->chosen);
        if (place->count == 1 && !place->folders.isEmpty())
            return folderLabel(place->folders.first());
        return place->typed;
    }
    if (!whereMissing().isEmpty())
        return {};
    return m_contentFolder.isEmpty() ? QString() : folderLabel(m_contentFolder);
}

// The folder chosen (not typed), as the last scan made it out: gone since, say.
static const ContentScope::Place* givenPlace(const ContentScope& scope, const QString& folder)
{
    if (folder.isEmpty())
        return nullptr;
    for (const ContentScope::Place& place : scope.places) {
        if (place.typed.isEmpty())
            return &place;
    }
    return nullptr;
}

QString Launcher::whereMissing() const
{
    const ContentScope::Place* place = typedPlace();
    if (place && place->status == ContentScope::Status::Words)
        place = nullptr;
    if (!place && m_scope == Content
        && std::ranges::none_of(m_contentScope.places, [](const ContentScope::Place& p) {
               return !p.typed.isEmpty() && p.status != ContentScope::Status::Words;
           }))
        place = givenPlace(m_contentScope, m_contentFolder);
    if (!place || (place->status != ContentScope::Status::NotFound && place->status != ContentScope::Status::OnShare))
        return {};
    return place->folders.value(0, place->typed);
}

void Launcher::chooseFolderChip(const QString& key)
{
    if (key == kAllChip) {
        m_contentPick = {};
    } else if (key.startsWith(kPickPrefix)) {
        const ContentQuery typed = splitContentQuery(m_query.trimmed());
        if (typed.folders.empty())
            return;
        m_contentPick = {typed.folders.front().typed, key.mid(kPickPrefix.size())};
    } else if (key.isEmpty() || key.startsWith(kPinPrefix)) {
        m_contentFolder = key.mid(kPinPrefix.size());
    } else {
        return; // a place not there, one not made out yet: nothing to choose
    }
    emit folderChipsChanged();
    emit scopeChanged(); // the placeholder names the folder
    m_results.setSelection({});
    m_held.reset();
    if (m_scope != Content) {
        setScope(Content);
        return;
    }
    refreshKeyHints();
    search(); // the rows stay until the new ones come (see setScope)
}

void Launcher::cycleFolderChip(int delta)
{
    QStringList keys;
    for (const QVariant& c : folderChips()) {
        const QString key = c.toMap()[u"type"_s].toString();
        if (key != kMissingChip && key != kTypedChip)
            keys.append(key);
    }
    if (keys.size() < 2)
        return; // no folder to go to
    const auto count = keys.size();
    const qsizetype at = std::max<qsizetype>(0, keys.indexOf(folderChip()));
    chooseFolderChip(keys[((at + delta) % count + count) % count]);
}

bool Launcher::clearContentWords()
{
    if (m_scope != Content || m_contentFolder.isEmpty() || m_query.trimmed().isEmpty())
        return false;
    setQuery(QString());
    return true;
}

void Launcher::editFolderChips()
{
    emit favoriteSettingsRequested(QString());
}

void Launcher::dismiss()
{
    emit escaped();
    emit dismissRequested();
}

namespace {

// What a row shows in the preview: a file or folder of its own (not an app,
// a place, a web page or a plugin's row; not one gone or out of reach).
std::optional<FilePreview::Item> previewItem(const SearchResult* r)
{
    if (r && r->clipId > 0 && !r->target.isEmpty()) // a screenshot: its picture
        return FilePreview::Item {r->target, r->name, false};
    if (!r || r->isApp() || r->missing || r->offline || r->path.isEmpty())
        return std::nullopt;
    return FilePreview::Item {r->path, r->name, r->isDir};
}

} // namespace

void Launcher::setPreview(FilePreview* preview)
{
    m_preview = preview;
    // New rows: the current one (the first, as QML selects it) is shown, once
    // QML has said which it is.
    connect(this, &Launcher::resultsReplaced, this, [this] {
        QMetaObject::invokeMethod(this, &Launcher::followPreview, Qt::QueuedConnection);
    });
    // 打开 in the preview's header: as Enter on that file's row.
    connect(preview, &FilePreview::closedByUser, this, [this] { setPreviewWanted(false); });
    connect(preview, &FilePreview::openRequested, this, [this](const QString& path) {
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r && !r->isApp() && r->path.compare(path, Qt::CaseInsensitive) == 0) {
                trigger(row, Open);
                return;
            }
        }
    });
}

void Launcher::togglePreview(int row)
{
    if (!m_preview)
        return;
    const SearchResult* r = m_results.at(row);
    const std::optional<FilePreview::Item> item = previewItem(r);
    m_preview->forgetClosedExternal(); // QuickLook or Seer closed on its own meanwhile
    // Open: the same key closes it, unless it was pressed for another file
    // (the row under the pointer), which it then shows.
    if (m_preview->isOpen()) {
        if (!item || m_preview->isShowing(item->path)) {
            m_preview->close();
            setPreviewWanted(false);
        } else {
            m_preview->show(*item);
        }
        return;
    }
    if (!item) {
        flash(r ? tr("这一行没有可预览的文件") : tr("没有选中的结果"));
        return;
    }
    m_preview->show(*item);
    setPreviewWanted(true);
}

void Launcher::setPreviewWanted(bool on)
{
    if (m_previewWanted == on)
        return;
    m_previewWanted = on;
    if (!m_stateFile.isEmpty())
        QSettings(m_stateFile, QSettings::IniFormat).setValue(u"Launcher/PreviewOpen"_s, on);
}

bool Launcher::closePreview()
{
    if (m_preview)
        m_preview->forgetClosedExternal(); // Esc then goes on to the launcher
    if (!m_preview || !m_preview->isOpen())
        return false;
    m_preview->close();
    return true;
}

void Launcher::followPreview()
{
    if (!m_preview)
        return;
    if (m_preview->isOpen()) {
        m_preview->follow(previewItem(m_results.at(m_currentRow)));
        return;
    }
    // Left open last time: back with the launcher, on the first row that
    // has a file to show.
    if (m_previewWanted && m_previewResume) {
        if (const std::optional<FilePreview::Item> item = previewItem(m_results.at(m_currentRow))) {
            m_previewResume = false;
            m_preview->show(*item);
        }
    }
}

void Launcher::keyboardMoved()
{
    m_pointerAtKeys = QCursor::pos();
}

QVariant Launcher::pointerIfMoved() const
{
    const QPoint now = QCursor::pos();
    return now == m_pointerAtKeys ? QVariant() : QVariant(QPointF(now));
}

void Launcher::trigger(int row, int action)
{
    if (action == SystemMenu) { // takes the selection itself, from Win顺's menu (Main.qml says where)
        showSystemMenu(row);
        return;
    }
    if (action == Preview) { // the row it was asked on, not a selection
        togglePreview(row);
        return;
    }
    // A terminal opens for the row it was asked on, not for each of a selection.
    if (action != OpenTerminal && m_results.isSelected(row) && m_results.selectedCount() > 1) {
        performMany(m_results.selection(), static_cast<Action>(action));
        return;
    }
    if (const SearchResult* r = m_results.at(row))
        perform(*r, static_cast<Action>(action));
}

void Launcher::clickRow(int row, bool onSwitch)
{
    const SearchResult* r = m_results.at(row);
    if (r && drawsSwitch(*r)) {
        // Two clicks within the system's double-click interval are one: the
        // second would switch it back (the first press of a double click gets
        // its clicked(), the second may not: see pitfalls, so decided here).
        if (m_clickedSwitch == r->path && m_switchClicked.isValid()
            && m_switchClicked.elapsed() < QGuiApplication::styleHints()->mouseDoubleClickInterval())
            return;
        m_clickedSwitch = r->path;
        m_switchClicked.start();
        if (onSwitch && r->intent.kind != SwitchIntent::Kind::Toggle) { // its switch is the state, whatever the row asks
            SearchResult plain = *r;
            plain.intent = {};
            perform(plain, Open);
            return;
        }
    }
    trigger(row, Open);
}

bool Launcher::isSwitchRow(int row) const
{
    const SearchResult* r = m_results.at(row);
    return r && drawsSwitch(*r);
}

bool Launcher::stepSwitch(int row, int steps)
{
    const SearchResult* r = m_results.at(row);
    if (!r || !steppable(*r))
        return false;
    const SearchResult stepped = *r;
    const SwitchState state = m_switches->state(stepped.switchId);
    const SwitchIntent resolved = state.level >= 0 ? Switches::stepped(state.level, steps)
                                                   : SwitchIntent {SwitchIntent::Kind::StepLevel, steps * 5};
    // Not remembered as a use (Enter is): a key held down would count dozens.
    // Nor are the words selected: the keys go on with the same row.
    switch (m_switches->step(stepped.switchId, steps)) {
    case Switches::Outcome::Already: // at 0 or 100 already
        flash(switchAlready(stepped, resolved));
        emit m_results.switchNudged(stepped.switchId);
        break;
    case Switches::Outcome::NotRead:
        flash(tr("还没读到%1的状态").arg(stepped.name));
        break;
    default: // Started; Busy: that level is on its way already
        break;
    }
    return true;
}

void Launcher::triggerSelection(int action)
{
    const SearchResults items = m_results.selection();
    if (items.size() == 1)
        perform(items.first(), static_cast<Action>(action));
    else if (!items.isEmpty())
        performMany(items, static_cast<Action>(action));
}

void Launcher::dragOut(int row)
{
    const SearchResult* at = m_results.at(row);
    if (!at)
        return;
    const SearchResults items = m_results.isSelected(row) && m_results.selectedCount() > 1 ? m_results.selection()
                                                                                         : SearchResults {*at};
    QStringList paths;
    for (const SearchResult& r : items) {
        // A share that does not answer would hold the shell up for long.
        if (!r.isApp() && !r.offline && serverAnswers(r.path))
            paths.append(r.path);
    }
    if (paths.isEmpty() || !m_window)
        return;
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    dragout::drag(paths, hwnd);
    // OLE's loop took the button's release: Qt still holds the press. Let it
    // go where the pointer is now (over another program, mostly).
    POINT p {};
    ::GetCursorPos(&p);
    ::ScreenToClient(hwnd, &p);
    ::PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y)));
}

void Launcher::toggleSelected(int row)
{
    const SearchResult* r = m_results.at(row);
    if (!r || drawsSwitch(*r)) // a switch's row is acted on alone: Enter switches it
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
        if (const SearchResult* r = m_results.at(row); r && !drawsSwitch(*r))
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
    if (action == EditWebShortcuts) {
        emit webSettingsRequested();
        return;
    }
    if (action == AddFavorite || action == RemoveFavorite || action == FavoriteFolder || action == EditFavorite) {
        performFavorite(r, action);
        return;
    }
    if (r.clipId > 0) {
        performScreenshot(r, action);
        return;
    }
    if (action == PinPicture) {
        if (!r.isApp() && !r.isDir) {
            emit pinRequested(r.path, false);
            emit dismissRequested();
        }
        return;
    }
    if (r.isPlugin()) {
        performPlugin(r, action);
        return;
    }
    if (action == Alternate)
        action = Open; // Shift+Enter means something to a plugin's row only
    // A favorite app uninstalled, a place gone with an update of Windows.
    if (r.missing && r.isApp() && (action == Open || action == RunAsAdmin || action == Reveal || action == CopyItem)) {
        flash(tr("“%1” 已不存在；右键可从收藏夹移除").arg(r.name));
        return;
    }
    if (r.isWeb()) {
        performWeb(r, action);
        return;
    }
    if (r.isPlace()) {
        performPlace(r, action);
        return;
    }
    if (r.isApp()) {
        performApp(r, action);
        return;
    }
    const bool needsFile
        = action == Open || action == Reveal || action == RunAsAdmin || action == Recycle || action == OpenTerminal;
    if (needsFile && !serverAnswers(r.path)) {
        flash(tr("连不上 %1：它离线了，或不在同一网络").arg(QString::fromStdWString(std::wstring(unc::host(r.path.toStdWString())))));
        return;
    }
    if (needsFile && r.offline)
        m_index->shares().networkChanged(); // it answers again: its share is back
    if (action == SearchInFolder) {
        if (!r.isDir)
            return;
        // 内容 there, the box emptied for what to find in it.
        m_contentFolder = r.path;
        m_contentPick = {};
        m_results.setSelection({});
        if (!m_query.isEmpty()) {
            m_query.clear();
            emit queryChanged();
        }
        if (m_scope != Content) {
            setScope(Content);
        } else {
            refreshKeyHints();
            search();
        }
        emit scopeChanged(); // the placeholder names the folder
        emit folderChipsChanged();
        return;
    }
    if (needsFile && !QFileInfo::exists(r.path)) {
        if (action == Recycle)
            m_results.remove(r.path);
        m_history->remove(r.path);
        emit historyChanged();
        // A favorite stays: it may be back (a drive plugged in again), or be picked anew.
        flash(r.favorite ? tr("“%1” 已不存在；右键可从收藏夹移除").arg(r.name) : tr("“%1” 已不存在").arg(r.name));
        return;
    }
    switch (action) {
    case Open:
    case RunAsAdmin:
        remember(r.path, m_shownText);
        // Opened by typing in Explorer, a folder goes to that tab (GitHub #39).
        if (action != Open || !r.isDir || !m_folderOpener || !m_folderOpener(r.path))
            shell::open(r.path, action == RunAsAdmin, reportFailure(r.name));
        emit dismissRequested();
        break;
    case Reveal:
        remember(r.path, m_shownText);
        if (!m_revealer || !m_revealer(r.path))
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
    case ForgetRecent:
        forgetRecent({r.path});
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
    case EditWebShortcuts:
    case AddFavorite:
    case RemoveFavorite:
    case OpenTerminal:
        remember(r.path, m_shownText);
        shell::openTerminal(r.isDir ? r.path : folderOf(r.path), reportFailure(r.name));
        emit dismissRequested();
        break;
    case FavoriteFolder:
    case EditFavorite:
    case SystemMenu:
        break; // not for a file, or done above (SystemMenu: trigger)
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
        remember(app.path, m_shownText);
        shell::launchApp(app.path, action == RunAsAdmin, reportFailure(app.name));
        emit dismissRequested();
        break;
    case Reveal:
        if (app.target.isEmpty() || !QFileInfo::exists(app.target)) {
            flash(tr("“%1”没有可以打开的位置").arg(app.name));
            return;
        }
        remember(app.path, m_shownText);
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
    case ForgetRecent:
        forgetRecent({app.path});
        break;
    case Recycle: // apps are uninstalled in Windows Settings
    case EditWebShortcuts:
    case AddFavorite:
    case RemoveFavorite:
    case FavoriteFolder:
    case EditFavorite:
    case OpenTerminal:
        break;
    }
}

// A place in Windows opens by its command, with the user's normal rights
// (Settings and Control Panel ask for more themselves when they need it).
// It has no file of its own to reveal or copy: copying takes the command.
void Launcher::performPlace(const SearchResult& place, Action action)
{
    if (!place.switchId.isEmpty() && performSwitch(place, action))
        return;
    switch (action) {
    case Open:
    case RunAsAdmin:
        remember(place.path, m_shownText);
        if (place.isCommand())
            shell::runCustom(place.target, place.workingDirectory, place.words, reportFailure(place.name));
        else
            shell::run(place.target, reportFailure(place.name));
        emit dismissRequested();
        break;
    case Reveal:
        flash(tr("“%1”没有可以打开的位置").arg(place.name));
        break;
    case CopyItem:
    case CopyPath:
        shell::copyText(place.target);
        flash(tr("已复制打开它的命令"));
        break;
    case CopyName:
        shell::copyText(place.name);
        flash(tr("已复制名称"));
        break;
    case ForgetRecent:
        forgetRecent({place.path});
        break;
    case Recycle:
    case EditWebShortcuts:
    case AddFavorite:
    case RemoveFavorite:
    case FavoriteFolder:
    case EditFavorite:
    case OpenTerminal:
    case SwitchOn:
    case SwitchOff:
        break;
    }
}

bool Launcher::liveSwitch(const SearchResult& row) const
{
    return !row.switchId.isEmpty() && m_switches
        && m_switches->state(row.switchId).availability != SwitchState::Availability::Unsupported;
}

bool Launcher::drawsSwitch(const SearchResult& row) const
{
    if (!liveSwitch(row))
        return false;
    const auto availability = m_switches->state(row.switchId).availability;
    return availability == SwitchState::Availability::Ready || availability == SwitchState::Availability::Unknown;
}

bool Launcher::steppable(const SearchResult& row) const
{
    return row.switchId == u"volume" && drawsSwitch(row);
}

// A system switch's row: Enter (a click, a number key) switches it with the
// window left open, Ctrl+Enter opens its page. False: no backend for it yet,
// an ordinary place; likewise for copying and the like.
bool Launcher::performSwitch(const SearchResult& row, Action action)
{
    if (!liveSwitch(row))
        return false;
    SwitchIntent intent = row.intent;
    switch (action) {
    case Reveal:
        openSwitchPage(row);
        return true;
    case SwitchOn:
        intent = {SwitchIntent::Kind::On, 0};
        break;
    case SwitchOff:
        intent = {SwitchIntent::Kind::Off, 0};
        break;
    case Open:
    case RunAsAdmin: // no switch needs more rights
        break;
    default:
        return false;
    }
    const SwitchIntent resolved = m_switches->resolve(row.switchId, intent);
    switchOutcome(row, resolved, static_cast<int>(m_switches->apply(row.switchId, intent)));
    return true;
}

void Launcher::switchOutcome(const SearchResult& row, const SwitchIntent& resolved, int outcome)
{
    using Outcome = Switches::Outcome;
    switch (static_cast<Outcome>(outcome)) {
    case Outcome::OpenPage: // nothing to switch here (no device, not allowed): the page, as before
        openSwitchPage(row);
        return;
    case Outcome::NotRead:
        flash(tr("还没读到%1的状态").arg(row.name));
        break;
    case Outcome::Started:
        remember(row.path, m_shownText); // counted for the next search; the list on screen does not move
        if (!m_switchPageTold) { // who pressed Enter on 蓝牙 for its page, once
            m_switchPageTold = true;
            if (!m_stateFile.isEmpty())
                QSettings(m_stateFile, QSettings::IniFormat).setValue(u"Hints/SwitchPage"_s, true);
            flash(tr("要打开设置页：Ctrl+Enter"));
        }
        break;
    case Outcome::Already:
        flash(switchAlready(row, resolved));
        emit m_results.switchNudged(row.switchId);
        break;
    case Outcome::Waiting:
        m_switchWaiting.insert(row.switchId, row);
        break;
    case Outcome::Busy:
        break;
    }
    // The window stays and the keyboard with it: the words are selected, so that
    // what is typed next is the next command.
    emit selectionRequested(0, static_cast<int>(m_query.size()));
}

void Launcher::scheduleAwakeTick()
{
    m_awakeTick.stop();
    if (!m_switches)
        return;
    const SwitchState awake = m_switches->state(u"keep-awake");
    if (awake.on != SwitchState::Value::On || !awake.until.isValid())
        return;
    // The minutes left round up, so the text changes as the time left passes a whole minute.
    const qint64 left = QDateTime::currentDateTime().msecsTo(awake.until);
    if (left <= 0)
        return; // ending: the switch's own timer says so
    const qint64 untilNext = left % 60'000 == 0 ? 60'000 : left % 60'000;
    m_awakeTick.start(static_cast<int>(untilNext) + 100);
}

void Launcher::openSwitchPage(const SearchResult& row)
{
    const QString instead = m_switches ? m_switches->state(row.switchId).openInstead : QString();
    remember(row.path, m_shownText);
    shell::run(instead.isEmpty() ? row.target : instead, reportFailure(row.name));
    emit dismissRequested();
}

QString Launcher::switchAlready(const SearchResult& row, const SwitchIntent& resolved) const
{
    using Kind = SwitchIntent::Kind;
    if (resolved.kind == Kind::SetLevel || resolved.kind == Kind::LevelOnly)
        return tr("音量已经是 %1").arg(resolved.value);
    if (resolved.kind != Kind::On && resolved.kind != Kind::Off)
        return tr("%1已经是这样了").arg(row.name);
    const bool on = resolved.kind == Kind::On;
    const QString& id = row.switchId;
    if (id == u"theme")
        return on ? tr("已经是深色") : tr("已经是浅色");
    if (id == u"hidden-files")
        return on ? tr("已经显示隐藏的文件") : tr("已经不显示隐藏的文件");
    if (id == u"file-extensions")
        return on ? tr("已经显示文件扩展名") : tr("已经隐藏文件扩展名");
    if (id == u"volume")
        return on ? tr("声音已经是开的") : tr("已经静音");
    if (id == u"keep-awake")
        return on ? tr("保持唤醒已经开着") : tr("保持唤醒没有开着");
    return on ? tr("%1已经是开的").arg(row.name) : tr("%1已经是关的").arg(row.name);
}

// A web shortcut's site or search opens in the default browser, with the
// user's normal rights like everything else. Nothing of it is remembered:
// what one searches the web for stays out of the list of recent items.
void Launcher::performWeb(const SearchResult& web, Action action)
{
    switch (action) {
    case Open:
    case RunAsAdmin: {
        // WINSHUN_WEB_DRYRUN (tests): said in the log (WINSHUN_LOG_INFO), not opened.
        static const bool dryRun = qEnvironmentVariableIsSet("WINSHUN_WEB_DRYRUN");
        qInfo().noquote() << (dryRun ? u"Web: would open %1%2"_s : u"Web: open %1%2"_s)
                                 .arg(web.target, web.suggestion ? u" (a suggestion)"_s : QString());
        if (!dryRun)
            shell::openUrl(web.target);
        emit dismissRequested();
        break;
    }
    case Reveal:
        flash(tr("“%1”没有可以打开的位置").arg(web.name));
        break;
    case CopyItem:
    case CopyPath:
        shell::copyText(web.target);
        flash(tr("已复制网址"));
        break;
    case CopyName:
        shell::copyText(web.name);
        flash(tr("已复制名称"));
        break;
    case ForgetRecent:
    case Recycle:
    case EditWebShortcuts: // see perform()
    case AddFavorite:
    case RemoveFavorite:
    case FavoriteFolder:
    case EditFavorite:
    case OpenTerminal:
        break;
    }
}

// The favorites: a row in or out, the folder a file is in, or the row's
// favorite in the settings. The rows refresh on their own (setFavorites);
// the footer says what was done after that (a search clears it).
void Launcher::performFavorite(const SearchResult& result, Action action)
{
    if (!m_favorites)
        return;
    switch (action) {
    case AddFavorite:
        if (const std::optional<Favorite> f = favoriteOf(result))
            flash(m_favorites->add(*f) ? tr("已加入收藏夹") : tr("已在收藏夹里"));
        break;
    case RemoveFavorite:
        if (m_favorites->remove(result.path))
            flash(tr("已从收藏夹移除"));
        break;
    case FavoriteFolder: {
        Favorite f;
        f.kind = Favorite::Kind::Folder;
        f.path = result.isApp() || result.isDir ? QString() : folderOf(result.path);
        if (f.path.isEmpty())
            break;
        f.name = f.path.size() == 3 ? f.path : f.path.mid(f.path.lastIndexOf(u'\\') + 1);
        flash(m_favorites->add(f) ? tr("已把“%1”加入收藏夹").arg(f.name) : tr("“%1”已在收藏夹里").arg(f.name));
        break;
    }
    case EditFavorite:
        emit favoriteSettingsRequested(result.path);
        break;
    default:
        break;
    }
}

// Several selected results at once. Those an action does not apply to
// (running a folder as administrator, recycling an app) are left out.
void Launcher::performMany(const SearchResults& items, Action action)
{
    // Each server asked afresh once, then taken at its word.
    QSet<QString> asked;
    const auto exists = [&asked](const QString& path) {
        const QString host = QString::fromStdWString(std::wstring(unc::host(path.toStdWString()))).toLower();
        const bool fresh = !asked.contains(host);
        asked.insert(host);
        return serverAnswers(path, fresh) && QFileInfo::exists(path);
    };
    if (action == Alternate)
        action = Open;
    if (action == AddFavorite && m_favorites) {
        FavoriteList favorites;
        for (const SearchResult& r : items) {
            if (const std::optional<Favorite> f = favoriteOf(r))
                favorites.append(*f);
        }
        const int added = m_favorites->addAll(favorites);
        flash(added > 0 ? tr("已加入收藏夹 %Ln 项", nullptr, added) : tr("选中的项目都已在收藏夹里"));
        return;
    }
    if (action == RemoveFavorite && m_favorites) {
        QStringList paths;
        for (const SearchResult& r : items)
            paths.append(r.path);
        if (const int removed = m_favorites->removeAll(paths); removed > 0)
            flash(tr("已从收藏夹移除 %Ln 项", nullptr, removed));
        return;
    }
    switch (action) {
    case Open:
    case RunAsAdmin: {
        const bool admin = action == RunAsAdmin;
        int started = 0;
        for (const SearchResult& r : items) {
            if (r.isPlugin()) { // its own default action (which may close the window itself)
                if (admin)
                    continue;
                performPlugin(r, Open);
                ++started;
                continue;
            }
            if (r.isWeb()) {
                if (admin)
                    continue;
                shell::openUrl(r.target);
                ++started;
                continue; // not remembered (see performWeb)
            }
            if (r.isPlace()) {
                if (admin || drawsSwitch(r)) // a switch is not "opened" with the others
                    continue;
                shell::run(r.target, reportFailure(r.name));
            } else if (r.isApp()) {
                if (admin && !r.elevatable)
                    continue;
                shell::launchApp(r.path, admin, reportFailure(r.name));
            } else {
                if (!exists(r.path) || (admin && (r.isDir || !shell::canRunAsAdministrator(r.path))))
                    continue;
                shell::open(r.path, admin, reportFailure(r.name));
            }
            remember(r.path, m_shownText);
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
            if (r.isPlace() || place.isEmpty() || !exists(place))
                continue;
            remember(r.path, m_shownText);
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
        if (paths.size() != 1 || shown != 1 || !m_revealer || !m_revealer(paths.first()))
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
        for (const SearchResult& r : items) {
            if (r.isPlugin())
                paths.append(r.plugin->copy.isEmpty() ? r.name : r.plugin->copy);
            else
                paths.append(r.isApp() && !r.target.isEmpty() ? r.target : r.path);
        }
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
    case ForgetRecent: {
        QStringList paths;
        for (const SearchResult& r : items) {
            if (r.recent)
                paths.append(r.path);
        }
        forgetRecent(paths);
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
    case EditWebShortcuts:
        emit webSettingsRequested();
        break;
    }
}

void Launcher::setRecordHistory(bool on)
{
    if (m_recordHistory == on)
        return;
    m_recordHistory = on;
    emit historyChanged();
    refreshFrequentApps();
    if (m_window && m_window->isVisible())
        search(); // "最近" rows come or go
}

bool Launcher::canClearHistory() const
{
    return m_recordHistory && !m_history->items().isEmpty();
}

void Launcher::clearHistory()
{
    m_history->clear();
    emit historyChanged();
    refreshFrequentApps();
    m_results.setSelection({});
    search(); // the list of recent ones empties, "最近" goes from the rest
    flash(tr("已清除最近使用记录"));
}

void Launcher::remember(const QString& path, const QString& query)
{
    if (m_recordHistory)
        m_history->record(path, query, QDateTime::currentMSecsSinceEpoch());
}

void Launcher::forgetRecent(const QStringList& paths)
{
    for (const QString& path : paths)
        m_history->remove(path);
    emit historyChanged();
    m_results.setSelection({});
    search(); // refreshes the rows in place: no "最近", or gone from the list of recent ones
    flash(paths.size() == 1 ? tr("已从最近使用中移除") : tr("已从最近使用中移除 %Ln 项", nullptr, static_cast<int>(paths.size())));
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
    emit historyChanged();
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
        items.append(entry(Open, tr("打开 %Ln 项", nullptr, static_cast<int>(selection.size())), u"Enter"_s, u"\uE8E5"_s));
        if (any([](const SearchResult& r) { return !r.isApp() || (!r.isPlace() && !r.target.isEmpty()); }))
            items.append(entry(Reveal, tr("打开所在位置"), u"Ctrl+Enter"_s, u"\uE8DA"_s));
        items.append(separator);
        if (any([](const SearchResult& r) { return !r.isApp() || r.hasCopyableTarget(); }))
            items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
        items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        if (any([](const SearchResult& r) { return r.recent; })) {
            items.append(separator);
            items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s)); // Cancel
        }
        if (m_favorites) {
            int addable = 0;
            int kept = 0;
            for (const SearchResult& s : selection) {
                if (favoriteOf(s))
                    ++(m_favorites->contains(s.path) ? kept : addable);
            }
            if (addable > 0 || kept > 0)
                items.append(separator);
            if (addable > 0)
                items.append(entry(AddFavorite, tr("加入收藏夹（%Ln 项）", nullptr, addable), QString(), u"\uE734"_s));
            else if (kept > 0)
                items.append(entry(RemoveFavorite, tr("从收藏夹移除（%Ln 项）", nullptr, kept), QString(), u"\uE8D9"_s));
        }
        if (any([](const SearchResult& r) { return !r.isApp() && !r.missing && !r.offline; })) {
            items.append(separator);
            items.append(entry(SystemMenu, tr("更多选项"), tr("Shift+右键"), u"\uE712"_s)); // More
        }
        return items;
    }

    // Glyphs: Segoe Fluent Icons / MDL2 Assets, or the name of an icon
    // Glyph.qml draws.
    QVariantList items;
    // The favorites' entries, after the copying ones: in or out (and edit),
    // and for a file the folder it is in.
    const auto favoriteItems = [&] {
        if (!m_favorites || !favoriteOf(*r))
            return;
        items.append(separator);
        if (m_favorites->contains(r->path)) {
            items.append(entry(RemoveFavorite, tr("从收藏夹移除"), QString(), u"\uE8D9"_s)); // Unfavorite
            items.append(entry(EditFavorite, tr("编辑收藏（名称、关键词）…"), QString(), u"\uE70F"_s)); // Edit
        } else {
            items.append(entry(AddFavorite, tr("加入收藏夹"), QString(), u"\uE734"_s)); // FavoriteStar
        }
        if (const QString folder = r->isApp() || r->isDir ? QString() : folderOf(r->path);
            !folder.isEmpty() && !m_favorites->contains(folder)) {
            items.append(entry(FavoriteFolder, tr("收藏所在的文件夹"), QString(), u"\uE8B7"_s)); // Folder
        }
    };
    if (r->clipId > 0) { // a screenshot of the history
        items.append(entry(Open, tr("复制并粘贴"), u"Enter"_s, u"\uE77F"_s)); // Paste
        items.append(entry(PinPicture, tr("贴到屏幕"), u"Ctrl+Enter"_s, u"\uE718"_s)); // Pin
        items.append(entry(EditPicture, tr("编辑"), u"Shift+Enter"_s, u"\uE70F"_s)); // Edit
        items.append(entry(Preview, tr("预览"), u"Alt+P"_s, u"\uE890"_s)); // View
        items.append(separator);
        items.append(entry(CopyItem, tr("复制图片"), u"Ctrl+C"_s, u"\uE8C8"_s));
        if (r->plugin && !r->plugin->copy.isEmpty())
            items.append(entry(CopyName, tr("复制识别出的文字"), QString(), u"\uE8AC"_s));
        items.append(separator);
        items.append(entry(RemoveScreenshot, tr("从历史删除"), tr("Delete"), u"\uE74D"_s)); // Delete
        return items;
    }
    if (r->isPlugin()) {
        // Its actions as the plugin named them (Enter, Ctrl+Enter, Shift+Enter
        // in that order), else what its default does; then copying its text.
        const PluginRow& p = *r->plugin;
        static constexpr QStringView kKeys[] {u"Enter", u"Ctrl+Enter", u"Shift+Enter"};
        if (p.valid) {
            for (int i = 0; i < p.actions.size(); ++i) {
                const QString key = !p.actions[i].key.isEmpty() ? p.actions[i].key
                    : i < 3                                     ? kKeys[i].toString()
                                                                : QString();
                items.append(entry(static_cast<Action>(PluginAction + i), p.actions[i].title, key,
                    i == 0 ? u"\uE8A7"_s : u"\uE768"_s));
            }
            if (p.actions.isEmpty()) {
                const QString text = p.openKind == PluginRow::OpenKind::Url ? tr("打开网址")
                    : !p.searchQuery.isEmpty()                              ? tr("搜索")
                    : !p.setQuery.isEmpty()                                 ? tr("填入搜索框")
                                                                            : tr("打开");
                items.append(entry(Open, text, u"Enter"_s, u"\uE8A7"_s));
            }
            items.append(separator);
        }
        items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        return items;
    }
    if (r->isWeb()) {
        items.append(r->words.isEmpty() ? entry(Open, tr("打开网站"), u"Enter"_s, u"\uE774"_s) // Globe
                                        : entry(Open, tr("搜索"), u"Enter"_s, u"\uE721"_s)); // Search
        items.append(separator);
        items.append(entry(CopyPath, tr("复制网址"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        items.append(separator);
        items.append(entry(EditWebShortcuts, tr("编辑网页搜索…"), QString(), u"\uE70F"_s)); // Edit
        return items;
    }
    if (r->isPlace() && liveSwitch(*r)) {
        // Both states by their words, so that the menu says what it will do;
        // the page is Ctrl+Enter's.
        if (drawsSwitch(*r)) {
            items.append(entry(SwitchOn, ResultModel::switchWords(*r, {SwitchIntent::Kind::On, 0}, false), QString(), u"\uE7E8"_s)); // Power
            items.append(entry(SwitchOff, ResultModel::switchWords(*r, {SwitchIntent::Kind::Off, 0}, false), QString(), u"\uE7E8"_s));
            items.append(entry(Reveal, r->switchId == u"keep-awake" ? tr("电源设置") : tr("打开设置"), u"Ctrl+Enter"_s, u"\uE713"_s)); // Settings
        } else {
            items.append(entry(Open, tr("打开设置"), u"Enter"_s, u"\uE713"_s));
        }
        items.append(separator);
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        if (r->recent) {
            items.append(separator);
            items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
        }
        return items;
    }
    if (r->isPlace()) {
        items.append(entry(Open, tr("打开"), u"Enter"_s, u"\uE8A7"_s)); // OpenInNewWindow
        items.append(separator);
        items.append(entry(CopyPath, tr("复制打开命令"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        favoriteItems();
        if (r->recent) {
            items.append(separator);
            items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
        }
        return items;
    }
    if (r->isApp()) {
        items.append(entry(Open, tr("打开"), u"Enter"_s, u"\uE8A7"_s)); // OpenInNewWindow
        if (r->elevatable)
            items.append(entry(RunAsAdmin, tr("以管理员身份运行"), u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
        if (!r->target.isEmpty())
            items.append(entry(Reveal, r->isPackagedApp() ? tr("打开安装文件夹") : tr("打开所在位置"), u"Ctrl+Enter"_s,
                u"\uE8DA"_s));
        items.append(separator);
        if (r->hasCopyableTarget())
            items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
        if (!r->target.isEmpty())
            items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        favoriteItems();
        if (r->recent) {
            items.append(separator);
            items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
        }
        return items;
    }
    items.append(entry(Open, r->isDir ? tr("打开文件夹") : tr("打开"), u"Enter"_s, r->isDir ? u"\uE838"_s : u"\uE8E5"_s));
    items.append(entry(Reveal, tr("打开所在位置"), u"Ctrl+Enter"_s, u"\uE8DA"_s));
    if (!r->missing && !r->offline)
        items.append(entry(Preview, tr("预览"), u"Alt+P"_s, u"\uE890"_s)); // View
    if (!r->isDir && !r->missing && !r->offline && pinnablePicture(r->path))
        items.append(entry(PinPicture, tr("贴到屏幕"), QString(), u"\uE718"_s)); // Pin
    items.append(entry(OpenTerminal, tr("在终端中打开"), QString(), u"\uE756"_s)); // CommandPrompt
    if (r->isDir && m_contentEnabled && !r->path.startsWith(u"\\\\"_s)) // shares: contents are not read there
        items.append(entry(SearchInFolder, tr("在此文件夹中搜内容"), QString(), u"\uE721"_s)); // Search
    if (!r->isDir && shell::canRunAsAdministrator(r->path))
        items.append(entry(RunAsAdmin, tr("以管理员身份运行"), u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
    items.append(separator);
    items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
    items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
    items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
    favoriteItems();
    if (r->recent) {
        items.append(separator);
        items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
    }
    if (!r->missing && !r->offline) { // the shell's menu for it, for whom Shift+right-click is news
        items.append(separator);
        items.append(entry(SystemMenu, tr("更多选项"), tr("Shift+右键"), u"\uE712"_s)); // More
    }
    return items;
}

bool Launcher::hasSystemMenu(int row) const
{
    const auto file = [](const SearchResult& r) { return !r.isApp() && !r.missing && !r.offline; };
    const SearchResult* r = m_results.at(row);
    if (!r)
        return false;
    if (m_results.isSelected(row) && m_results.selectedCount() > 1) { // for the selection: any file in it
        const SearchResults selection = m_results.selection();
        return std::any_of(selection.cbegin(), selection.cend(), file);
    }
    return file(*r);
}

bool Launcher::systemMenuBusy() const
{
    return m_systemMenuOpen || (m_systemMenuAsked.isValid() && m_systemMenuAsked.elapsed() < 500);
}

void Launcher::showSystemMenu(int row, const QVariant& globalPos)
{
    const SearchResult* at = m_results.at(row);
    if (!at || !hasSystemMenu(row) || m_systemMenuOpen || !m_window)
        return;
    // The selection when the row is in it (its files; apps and the like
    // have no shell menu), else the row alone, as Win顺's menu has it.
    const SearchResults items = m_results.isSelected(row) && m_results.selectedCount() > 1 ? m_results.selection()
                                                                                         : SearchResults {*at};
    QStringList paths;
    for (const SearchResult& r : items) {
        // A share that does not answer would hold the shell up for long.
        if (!r.isApp() && !r.missing && !r.offline && serverAnswers(r.path))
            paths.append(r.path);
    }
    if (paths.isEmpty())
        return;
    if (const qsizetype here = paths.indexOf(at->path); here > 0)
        paths.move(here, 0); // the row asked on first: the one the menu falls back to
    const std::optional<QPointF> pos = globalPos.canConvert<QPointF>() ? std::optional(globalPos.toPointF()) : std::nullopt;
    m_systemMenuAsked.start();
    // From the event loop, not from inside the click's handler: the menu's
    // loop runs for as long as it is up (with the queued work going on
    // underneath: results coming in, rows rewritten), and the command may
    // go on to open dialogs of its own.
    QMetaObject::invokeMethod(this, [self = QPointer(this), paths, pos] {
        if (self)
            self->runSystemMenu(paths, pos);
    }, Qt::QueuedConnection);
}

void Launcher::runSystemMenu(const QStringList& paths, std::optional<QPointF> globalPos)
{
    if (!m_window || !m_window->isVisible() || m_systemMenuOpen)
        return;
    POINT pos {};
    if (globalPos) {
        // Logical to physical pixels by the screen the point is on (its own
        // scale on a second monitor): Qt gives every screen the same origin
        // in both (the reverse of FolderMenu's).
        QScreen* screen = QGuiApplication::screenAt(globalPos->toPoint());
        if (!screen)
            screen = m_window->screen();
        const QPoint origin = screen->geometry().topLeft();
        const qreal scale = screen->devicePixelRatio();
        pos.x = origin.x() + qRound((globalPos->x() - origin.x()) * scale);
        pos.y = origin.y() + qRound((globalPos->y() - origin.y()) * scale);
    } else {
        ::GetCursorPos(&pos);
    }
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    const bool extended = ::GetKeyState(VK_SHIFT) < 0; // as Explorer: Shift held as the menu opens adds its extra verbs
    m_systemMenuOpen = true;
    const shellcontext::Choice choice = shellcontext::showMenu(paths, hwnd, pos, extended, [&](const shellcontext::Choice& c) {
        // The menu is gone, the command about to run. One that opens
        // something (a program, 属性, 打开方式) takes the launcher down first,
        // as its own 打开 does; the rest (复制, 删除, 固定…) keep it, through
        // whatever the command shows meanwhile (App leaves the launcher be
        // while systemCommandRunning).
        m_systemMenuOpen = false;
        if (shellcontext::keepsLauncher(c.verb)) {
            m_systemCommand = true;
            return;
        }
        if (c.verb == L"open" || c.verb == L"edit" || c.verb == L"runas") {
            for (const QString& path : paths)
                remember(path, m_shownText);
        }
        emit dismissRequested();
    });
    m_systemMenuOpen = false;
    if (!choice.chosen)
        return; // Esc, or a click elsewhere: the launcher stays, as after its own menu
    if (m_systemCommand) {
        // It stays on (systemCommandRunning): 删除 shows its progress window
        // only after the call returned; the hidden owner window hands the
        // focus back to the launcher when such a dialog closes (ownerProc),
        // and App ends it once the launcher is active again or another
        // program's window took the focus. A command that shows nothing
        // (复制, 固定) would leave it on, keeping the launcher up for a window
        // of ours it should go for (the settings): off once it had its moment.
        // Left inactive by then with the focus elsewhere (nothing handed it
        // back), the launcher goes, as for a click elsewhere.
        QTimer::singleShot(3000, this, [this] {
            if (!m_window || !m_systemCommand)
                return;
            if (m_window->isActive()) {
                m_systemCommand = false;
                return;
            }
            DWORD pid = 0;
            ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
            if (m_window->isVisible() && pid != ::GetCurrentProcessId()) {
                m_systemCommand = false;
                emit dismissRequested();
            }
        });
    }
    // What Win顺's own menu says for the like of it.
    if (choice.verb == L"copy")
        flash(paths.size() == 1 ? tr("已复制，可在资源管理器中粘贴")
                                : tr("已复制 %Ln 项，可在资源管理器中粘贴", nullptr, static_cast<int>(paths.size())));
    else if (choice.verb == L"cut")
        flash(tr("已剪切，可在资源管理器中粘贴"));
    else if (choice.verb == L"copyaspath")
        flash(paths.size() == 1 ? tr("已复制路径") : tr("已复制 %Ln 个路径", nullptr, static_cast<int>(paths.size())));
    forgetGone(paths, 0);
}

void Launcher::forgetGone(const QStringList& paths, int attempt)
{
    for (const QString& path : paths) {
        if (QFileInfo::exists(path))
            continue;
        if (const SearchResult* row = m_results.at(m_results.indexOf(path)))
            forgetRecycled(*row); // the row and its count, and the history
        else
            m_history->remove(path);
    }
    refreshStatus();
    // The shell may still be at it: a confirmation, a progress window for a
    // big folder; a paste elsewhere comes later still (the index tells then).
    static constexpr int kAgain[] {250, 1500, 4000};
    if (attempt < static_cast<int>(std::size(kAgain)))
        QTimer::singleShot(kAgain[attempt], this, [this, paths, attempt] { forgetGone(paths, attempt + 1); });
}

// ---- plugins ------------------------------------------------------------------

QString Launcher::pluginKeyword() const
{
    return m_keyword ? m_keyword->keyword : QString();
}

QString Launcher::pluginLabel() const
{
    return m_keyword ? m_keyword->label : QString();
}

QString Launcher::pluginIcon() const
{
    return m_keyword ? m_keyword->icon : QString();
}

// After the keyword, while nothing follows it: the user is about to type
// the words, so the hint sits where they go.
QString Launcher::pluginHint() const
{
    return m_keyword && m_keyword->words.isEmpty() && m_query.endsWith(u' ') ? m_keyword->hint : QString();
}

QString Launcher::pluginWaiting() const
{
    if (!m_keyword || m_pluginPending.isEmpty() || !m_pluginSlow)
        return {};
    return m_pluginProgress.isEmpty() ? tr("正在等 %1…").arg(m_keyword->label) : m_pluginProgress;
}

QString Launcher::pluginName(const QString& pluginId) const
{
    if (m_plugins) {
        if (const plugin::Entry* e = m_plugins->catalog().find(pluginId); e && e->manifest)
            return e->manifest->name.pick(uiLocale());
    }
    return pluginId;
}

bool Launcher::clearToKeyword()
{
    if (!m_keyword || m_keyword->words.isEmpty())
        return false;
    setQuery(m_keyword->keyword + u' ');
    return true;
}

// The row with an autocomplete: `row` itself, else (outside keyword mode,
// where the row's own is all Tab means) the hint row offering a keyword
// that begins with what was typed, which sits among the rows, not under
// the keyboard ("uu": Tab gives "uuid " from the app row above it).
const SearchResult* Launcher::completionRow(int row) const
{
    const SearchResult* r = m_results.at(row);
    if (r && ((r->isPlugin() && !r->plugin->autocomplete.isEmpty()) || r->suggestion))
        return r;
    if (m_keyword)
        return nullptr;
    for (int i = 0; i < m_results.count(); ++i) {
        const SearchResult* h = m_results.at(i);
        if (plugin::isHintRow(*h) && !h->plugin->autocomplete.isEmpty())
            return h;
    }
    return nullptr;
}

// A favorite folder's row: Tab lists what it holds, as typing its path does
// ("D:\工作文档\"). Other folders' rows keep Tab for the scopes.
const SearchResult* Launcher::enterableFolder(int row) const
{
    const SearchResult* r = m_results.at(row);
    return r && !m_keyword && r->favorite && r->isDir && !r->isApp() && !r->offline && !r->missing ? r : nullptr;
}

bool Launcher::autocomplete(int row)
{
    // Not in 文件 and 内容: there a folder is narrowed to by the strip
    // (Ctrl+Tab), and Tab and Shift+Tab are the scopes'.
    const bool narrowing = m_scope == Files || m_scope == Folders || m_scope == Content;
    if (const SearchResult* folder = narrowing ? nullptr : enterableFolder(row)) {
        setQuery(folder->path.endsWith(u'\\') ? folder->path : folder->path + u'\\');
        return true;
    }
    const SearchResult* r = completionRow(row);
    if (!r)
        return false;
    // A site's suggestion: into the box after its keyword, to type on from.
    setQuery(r->suggestion ? r->keyword + u' ' + r->words : r->plugin->autocomplete);
    return true;
}

void Launcher::leaveKeyword()
{
    if (m_keyword)
        setQuery({});
}

// ---- The screenshot history (jt) ----------------------------------------

void Launcher::setScreenshots(const QString& keyword, ScreenshotHooks hooks)
{
    const bool changed = keyword != m_shotKeyword;
    m_shotKeyword = keyword;
    m_shotHooks = std::move(hooks);
    if (changed && m_window && m_window->isVisible())
        search(); // the keyword typed is one no longer, or now is
}

bool Launcher::screenshotMode() const
{
    return m_keyword && m_keyword->pluginId == kShotPlugin;
}

void Launcher::refreshScreenshots()
{
    if (screenshotMode() && m_window && m_window->isVisible())
        search();
}

bool Launcher::undoScreenshotRemoval()
{
    if (!screenshotMode() || !m_shotHooks.undo || !m_shotHooks.undo())
        return false;
    search();
    return true;
}

// As a plugin's keyword: first in the box, then a space; the words after it
// are looked for in what was read in the pictures. In 全部 only, as theirs.
std::optional<Launcher::KeywordMode> Launcher::matchScreenshotKeyword(const QString& text) const
{
    if (m_shotKeyword.isEmpty() || !m_shotHooks.list || m_scope != All)
        return std::nullopt;
    QStringView typed(text);
    while (!typed.isEmpty() && typed.front().isSpace())
        typed = typed.mid(1);
    if (typed.size() <= m_shotKeyword.size() || !typed.startsWith(m_shotKeyword, Qt::CaseInsensitive)
        || !typed.at(m_shotKeyword.size()).isSpace())
        return std::nullopt;
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    return KeywordMode {kShotPlugin.toString(), kShotCommand.toString(), typed.left(m_shotKeyword.size()).toString(),
        typed.mid(m_shotKeyword.size() + 1).trimmed().toString(), tr("截图历史"), tr("输入文字，在截图里识别出的文字中找"),
        FileIconProvider::pluginIconUrl(kShotPlugin.toString(), kShotIcon.toString(), dark)};
}

// A row per screenshot, newest first: the start of its words for a title,
// its size and when it was taken under it, its picture for an icon.
SearchResults Launcher::screenshotRows(const QString& words) const
{
    SearchResults rows;
    if (!m_shotHooks.list)
        return rows;
    const std::vector<Screenshot> shots = m_shotHooks.list(words);
    for (const Screenshot& shot : shots) {
        if (rows.size() >= kShotRows)
            break;
        auto p = std::make_shared<PluginRow>();
        p->pluginId = kShotPlugin.toString();
        p->commandId = kShotCommand.toString();
        p->itemId = u'#' + QString::number(shot.id);
        p->source = tr("截图");
        const QString taken = QDateTime::fromMSecsSinceEpoch(shot.created).toString(u"yyyy-MM-dd HH:mm"_s);
        p->subtitle = tr("%1 × %2 · %3").arg(shot.width).arg(shot.height).arg(taken);
        p->icon = kShotIcon.toString();
        const QString text = shot.ocr.simplified();
        p->copy = text;
        p->actions = {{u"paste"_s, tr("复制并粘贴"), u"Enter"_s}, {u"pin"_s, tr("贴到屏幕"), u"Ctrl+Enter"_s},
            {u"edit"_s, tr("编辑"), u"Shift+Enter"_s}};
        SearchResult r;
        r.name = text.isEmpty() ? tr("（没有识别出文字）") : text.left(120);
        r.path = plugin::pluginPath(p->pluginId, p->commandId, p->itemId);
        r.target = shot.imagePath;
        r.modified = static_cast<std::uint32_t>(std::max<qint64>(0, shot.created / 1000));
        r.app = AppKind::Plugin;
        r.score = 100;
        r.clipId = shot.id;
        r.plugin.row = std::move(p);
        rows.append(std::move(r));
    }
    return rows;
}

// The keyword typed alone ("jt"): a row offering it, first and current, as
// a plugin keyword's hint row (plugin::hintRow): Enter, Tab or a space
// opens the list.
void Launcher::placeScreenshotHint()
{
    if (m_shotKeyword.isEmpty() || !m_shotHooks.list || m_scope != All
        || m_query.trimmed().compare(m_shotKeyword, Qt::CaseInsensitive) != 0)
        return;
    auto p = std::make_shared<PluginRow>();
    p->pluginId = kShotPlugin.toString();
    p->commandId = kShotCommand.toString();
    p->itemId = plugin::kHintItem.toString();
    p->source = tr("截图");
    p->subtitle = tr("按空格进入；Enter 或 Tab 也行");
    p->icon = kShotIcon.toString();
    p->autocomplete = m_shotKeyword + u' ';
    p->setQuery = p->autocomplete;
    p->score = 100;
    SearchResult r;
    r.name = m_shotKeyword + u" — "_s + tr("截图历史");
    r.path = plugin::pluginPath(p->pluginId, p->commandId, p->itemId);
    r.app = AppKind::Plugin;
    r.score = 100;
    r.plugin.row = std::move(p);
    for (int i = 0; i < m_results.count(); ++i) {
        if (m_results.at(i)->path == r.path)
            return;
    }
    m_results.insert(0, r);
    emit betterFirstRow();
}

void Launcher::performScreenshot(const SearchResult& result, Action action)
{
    const qint64 id = result.clipId;
    switch (static_cast<int>(action)) {
    case Open:
    case PluginAction:
        if (m_shotHooks.paste)
            m_shotHooks.paste(id);
        return;
    case Reveal:
    case PinPicture:
    case PluginAction + 1:
        emit pinRequested(result.target, true);
        emit dismissRequested();
        return;
    case Alternate:
    case EditPicture:
    case PluginAction + 2:
        if (m_shotHooks.edit) {
            emit dismissRequested(); // first: the editing window takes the front
            m_shotHooks.edit(id);
        }
        return;
    case CopyItem:
        if (m_shotHooks.copy)
            m_shotHooks.copy(id);
        flash(tr("已复制截图"));
        return;
    case CopyName:
    case CopyPath:
        if (result.plugin && !result.plugin->copy.isEmpty()) {
            shell::copyText(result.plugin->copy);
            flash(tr("已复制文字"));
        }
        return;
    case RemoveScreenshot:
        if (m_shotHooks.remove) {
            m_shotHooks.remove(id);
            search();
            flash(tr("已从历史删除；Ctrl+Z 撤销"));
        }
        return;
    default:
        return;
    }
}

std::optional<Launcher::KeywordMode> Launcher::matchKeyword(const QString& text) const
{
    if (std::optional<KeywordMode> shots = matchScreenshotKeyword(text))
        return shots;
    if (!m_plugins || !m_merge || m_scope != All || text.trimmed().isEmpty())
        return std::nullopt;
    for (const plugin::Match& m : m_plugins->catalog().match(text)) {
        if (m.kind != plugin::Match::Kind::Keyword)
            continue;
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        const QString locale = uiLocale();
        const QString icon = !m.command->icon.isEmpty() ? m.command->icon : m.entry->manifest->icon;
        return KeywordMode {m.entry->id, m.command->id, m.keyword, m.text,
            m.command->title.pick(locale).isEmpty() ? m.entry->manifest->name.pick(locale) : m.command->title.pick(locale),
            m.command->hint.pick(locale), FileIconProvider::pluginIconUrl(m.entry->id, icon, dark)};
    }
    return std::nullopt;
}

// Part of search(): the plugins the text triggers are asked (and the
// earlier query cancelled); a keyword's command takes the list (m_keyword).
void Launcher::startPluginQuery(const QString& text, std::optional<KeywordMode> keyword)
{
    const bool was = m_keyword.has_value();
    m_keyword.reset();
    m_pluginPending.clear();
    m_pluginWait.stop();
    m_pluginSlow = false;
    m_pluginProgress.clear();
    if (keyword && keyword->pluginId == kShotPlugin) {
        // Win顺's own list: no plugin is asked, and whatever they were
        // asked before is moot (their answers come for an older id).
        const std::uint64_t id = ++m_pluginQueryId;
        if (m_plugins)
            m_plugins->cancelAll();
        m_keyword = std::move(keyword);
        if (m_merge)
            m_merge->begin(id, text, m_keyword->pluginId, m_keyword->commandId);
        emit pluginChanged();
        return;
    }
    if (!m_plugins || !m_merge) {
        if (was)
            emit pluginChanged();
        return;
    }
    const auto scope = static_cast<ws::Scope>(m_scope);
    const std::uint64_t id = ++m_pluginQueryId;
    m_keyword = std::move(keyword);
    m_merge->begin(id, text, m_keyword ? m_keyword->pluginId : QString(), m_keyword ? m_keyword->commandId : QString());
    m_pluginTimer.start();
    m_plugins->query(text, scope, id); // queryTaken() for each plugin asked; zero-code ones answer within
    if (!m_pluginPending.isEmpty())
        m_pluginWait.start();
    if (was || m_keyword)
        emit pluginChanged();
}

void Launcher::onPluginTaken(std::uint64_t queryId, const QString& pluginId)
{
    if (queryId == m_pluginQueryId)
        m_pluginPending.insert(pluginId);
}

void Launcher::onPluginResults(std::uint64_t queryId, const QString& pluginId, const QString& commandId,
    const QList<ws::plugin::Item>& items, bool partial, bool replace)
{
    if (queryId != m_pluginQueryId || !m_merge)
        return;
    m_merge->results(queryId, pluginId, commandId, items, partial, replace);
    if (m_merge->takeBetterFirstRow())
        emit betterFirstRow();
    refreshStatus();
}

void Launcher::onPluginFinished(std::uint64_t queryId, const QString& pluginId)
{
    if (queryId == m_pluginQueryId)
        pluginSettled(pluginId);
}

void Launcher::onPluginFailed(std::uint64_t queryId, const QString& pluginId, const QString& why)
{
    if (queryId != m_pluginQueryId)
        return;
    m_pluginError = tr("%1：%2").arg(pluginName(pluginId), why);
    m_pluginErrorTimer.start();
    // The keyword's command gave nothing: an empty list, not the last query's rows.
    if (m_keyword && m_keyword->pluginId == pluginId && m_replacePending)
        showPluginRows({});
    pluginSettled(pluginId);
}

void Launcher::onPluginProgress(std::uint64_t queryId, const QString& pluginId, const QString& text)
{
    if (queryId != m_pluginQueryId || !m_keyword || m_keyword->pluginId != pluginId)
        return;
    m_pluginProgress = text;
    refreshStatus();
}

void Launcher::pluginSettled(const QString& pluginId)
{
    m_pluginPending.remove(pluginId);
    if (m_pluginPending.isEmpty()) {
        m_pluginWait.stop();
        m_pluginSlow = false;
        m_pluginElapsedUs = m_pluginTimer.isValid() ? m_pluginTimer.nsecsElapsed() / 1000 : 0;
    }
    refreshStatus();
}

// A plugin row's action: Enter, Ctrl+Enter and Shift+Enter are the first
// three of its actions (its own, else its command's); the menu names them
// by number. The default one is done here when the row says how (open,
// setQuery, search: no round trip, and no process needed); every other
// goes to the plugin, whose effects come back (onPluginActionDone). Never
// as administrator: a plugin's rows open with the user's rights, always.
void Launcher::performPlugin(const SearchResult& result, Action action)
{
    const PluginRow& p = *result.plugin;
    const QString copyText = p.copy.isEmpty() ? result.name : p.copy;
    switch (action) {
    case CopyItem:
    case CopyPath:
        shell::copyText(copyText);
        flash(tr("已复制"));
        return;
    case CopyName:
        shell::copyText(result.name);
        flash(tr("已复制名称"));
        return;
    case RunAsAdmin:
        flash(tr("插件的结果不以管理员身份运行"));
        return;
    case Recycle:
    case ForgetRecent:
    case EditWebShortcuts:
        return;
    default:
        break;
    }
    const int index = action == Open ? 0 : action == Reveal ? 1 : action == Alternate ? 2 : action >= PluginAction ? action - PluginAction : -1;
    if (index < 0 || !p.valid)
        return;
    if (index == 0) {
        switch (p.openKind) {
        case PluginRow::OpenKind::Path:
            // A program with arguments or a working folder is started as
            // such; anything else opens as the shell would open it.
            if (p.openArgs.isEmpty() && p.openCwd.isEmpty())
                shell::open(p.openTarget, false, reportFailure(result.name));
            else if (!shell::startUnelevated(p.openTarget.toStdWString(), p.openArgs.toStdWString(), p.openCwd.toStdWString()).valid())
                emit openFailed(result.name);
            emit dismissRequested();
            return;
        case PluginRow::OpenKind::Url:
            // "winshun:settings": Win顺's own settings at the plugin's card
            // (a row saying what to set up); never handed to the shell.
            if (p.openTarget.compare(u"winshun:settings", Qt::CaseInsensitive) == 0)
                emit pluginSettingsRequested(p.pluginId);
            else
                shell::openUrl(p.openTarget);
            emit dismissRequested();
            return;
        case PluginRow::OpenKind::None:
            break;
        }
        if (!p.searchQuery.isEmpty()) {
            const QString scope = p.searchScope.toLower();
            setScope(scope == u"files" ? Files : scope == u"folders" ? Folders : scope == u"content" ? Content : All);
            setQuery(p.searchQuery);
            return;
        }
        if (!p.setQuery.isEmpty()) {
            setQuery(p.setQuery);
            return;
        }
    }
    if (index >= p.actions.size() && !(index == 0 && p.actions.isEmpty()))
        return; // no such action
    if (!m_plugins)
        return;
    const QString actionId = index < p.actions.size() ? p.actions[index].id : u"default"_s;
    const QString words = m_keyword ? m_keyword->words : m_query.trimmed();
    m_plugins->action(++m_pluginRequest, p.pluginId, p.commandId, actionId, p.item, words);
}

void Launcher::onPluginActionDone(std::uint64_t requestId, const QList<ws::plugin::Effect>& effects, bool ok, const QString& error)
{
    (void)requestId;
    if (!ok) {
        flash(error.isEmpty() ? tr("插件没有完成这个操作") : error);
        return;
    }
    applyEffects(effects);
}

// What a plugin asked for after an action, in its order: hide (the
// default), keep, setQuery, notify, copy, open, replace (its items as the list).
void Launcher::applyEffects(const QList<ws::plugin::Effect>& effects)
{
    bool hide = false;
    for (const plugin::Effect& e : effects) {
        switch (e.kind) {
        case plugin::Effect::Kind::Hide:
            hide = true;
            break;
        case plugin::Effect::Kind::Keep:
            break;
        case plugin::Effect::Kind::SetQuery:
            setQuery(e.text);
            break;
        case plugin::Effect::Kind::Notify:
            flash(e.text);
            break;
        case plugin::Effect::Kind::Copy:
            shell::copyText(e.text);
            break;
        case plugin::Effect::Kind::Open:
            if (e.open.kind == plugin::Open::Kind::Url && e.open.target.compare(u"winshun:settings", Qt::CaseInsensitive) == 0) {
                if (m_keyword)
                    emit pluginSettingsRequested(m_keyword->pluginId); // its own card (see performPlugin)
            } else if (e.open.kind == plugin::Open::Kind::Url)
                shell::openUrl(e.open.target);
            else if (e.open.args.isEmpty() && e.open.cwd.isEmpty())
                shell::open(e.open.target, false);
            else
                shell::startUnelevated(e.open.target.toStdWString(), e.open.args.toStdWString(), e.open.cwd.toStdWString());
            break;
        case plugin::Effect::Kind::Replace:
            if (m_merge && m_keyword)
                m_merge->replace(m_keyword->pluginId, m_keyword->commandId, e.items);
            break;
        }
    }
    if (hide)
        emit dismissRequested();
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
    win::styleFramelessWindow(menu);
    win::setDarkFrame(menu, dark);
}

} // namespace ws
