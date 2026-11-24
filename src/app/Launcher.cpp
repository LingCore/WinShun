#include "Launcher.h"
#include "platform/ContentOpen.h"
#include "Phosphor.h"
#include "PreviewShortcut.h"
#include "RenameCheck.h"
#include "ResultColumns.h"
#include "Settings.h"
#include "Wsl.h"

#include "AppCatalog.h"
#include "Crawler.h"
#include "Details.h"
#include "FileIconProvider.h"
#include "FilePreview.h"
#include "History.h"
#include "Host.h"
#include "IndexService.h"
#include "Merge.h"
#include "ZeroCode.h"
#include "OpenPolicy.h"
#include "PathText.h"
#include "PictureFile.h"
#include "Query.h"
#include "SearchEngine.h"
#include "ShareLister.h"
#include "ShareReach.h"
#include "SwitchBackend.h"
#include "Switches.h"
#include "SystemCatalog.h"
#include "ToastPolicy.h"
#include "Unc.h"
#include "WebDav.h"
#include "Win32Util.h"
#include "DragPicture.h"
#include "platform/DragOut.h"
#include "platform/FileManagers.h"
#include "platform/Power.h"
#include "platform/Shell.h"
#include "platform/ShellContextMenu.h"
#include "platform/WindowEffects.h"
#include "platform/WindowSearch.h"

#include <QCollator>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QStyleHints>
#include <QTimeZone>
#include <QVarLengthArray>

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <numeric>
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
static_assert(std::size(kSortNames) == Launcher::MostOpened + 1); // a quick filter's sort is one by name

// A sort of the compact list as the panel's columns order it (setPanel, a
// quick filter's own order); by best match or most opened: as ranked.
static SortOrder columnOrder(Launcher::Sort sort)
{
    switch (sort) {
    case Launcher::NewestFirst:
        return {SortKey::Modified, true};
    case Launcher::OldestFirst:
        return {SortKey::Modified, false};
    case Launcher::ByName:
        return {SortKey::Name, false};
    case Launcher::ByFolder:
        return {SortKey::Path, false};
    case Launcher::ByType:
        return {SortKey::Type, false};
    case Launcher::LargestFirst:
        return {SortKey::Size, true};
    case Launcher::SmallestFirst:
        return {SortKey::Size, false};
    case Launcher::BestMatch:
    case Launcher::MostOpened:
        break;
    }
    return {};
}

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
// Files on shares and in WSL whose contents are not indexed (GitHub #114):
// the settings' section of those folders, where they are turned on.
constexpr QLatin1StringView kRemoteHint("remote");
constexpr QLatin1StringView kDeferredHint("deferred");
constexpr QLatin1StringView kDriveHint("drive:"); // + "E:"
// With only the chosen folders indexed (GitHub #52): a folder not among them
// walked for the query (+ its path), the same folder added to them (+ its
// path), the settings' page of them, and the general note closed for good.
constexpr QLatin1StringView kFolderHint("folder:");
constexpr QLatin1StringView kAddFolderHint("addfolder:");
constexpr QLatin1StringView kFoldersHint("folders");
constexpr QLatin1StringView kFoldersDismissHint("folders-dismiss");
// Typed in a folder's window with only that folder searched (GitHub #110):
// the rest of the index after its rows, this time (switchTypingFolder).
constexpr QLatin1StringView kEverywhereHint("everywhere");
// ... and whether its subfolders are in it (GitHub #182): the other way, this time.
constexpr QLatin1StringView kLevelHint("level");
// Matches left out by exclusion rules: in-memory counts (everythingCompat or
// skipped files), on-demand disk walk of excludedPaths, and settings links.
constexpr QLatin1StringView kHiddenFoldersHint("hidden-folders");
constexpr QLatin1StringView kExcludedFilesHint("excluded-files");
constexpr QLatin1StringView kExcludedFoldersHint("excluded-folders");
constexpr QLatin1StringView kExcludedSettingsHint("excluded-settings");
constexpr QLatin1StringView kExcludedFileSettingsHint("excluded-file-settings");
constexpr QLatin1StringView kScopeSettingsHint("scope-settings");

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

// How long an action on a file on a server waits for it (GitHub #116: a NAS
// answering after 2.5-4 s): the probe and Windows alike (share::reach); all
// its servers at once, and no longer (share::checkFiles); "正在连接 …" says
// so meanwhile.
constexpr std::chrono::milliseconds kServerWait {8000};
constexpr std::chrono::milliseconds kServersWait = 2 * kServerWait + std::chrono::seconds(1); // share::reach at most
constexpr std::chrono::milliseconds kServerWaitShown = kServersWait + std::chrono::seconds(1);
// WebClient stopped (GitHub #114): an action on a WebDAV file starts it
// first (share::actionSteps), "正在启动 Windows 的 WebDAV 服务…" meanwhile.
// It ran in 70 ms here and its first access took 0.1 s, but that first
// access is said to take up to a minute: Windows waited for that long
// (twice kClientStartWait once the probe said yes), the action's other
// servers with it.
constexpr std::chrono::milliseconds kClientStartWait {30000};
constexpr std::chrono::milliseconds kClientStartsWait = 2 * kClientStartWait + std::chrono::seconds(1);
constexpr std::chrono::milliseconds kClientStartShown = kClientStartsWait + std::chrono::seconds(1);
// A server that answered this recently answers again at once: its
// "正在连接 …" waits a moment, not to flicker past.
constexpr std::chrono::seconds kJustHeard {3};
constexpr std::chrono::milliseconds kQuietWait {300};
// A drag asks no server that did not answer this recently (each try would
// hold the window up again), nor one heard to answer this recently (Windows
// getting there too: the probe may say no there, GitHub #116); the others
// all at once, for this long together.
constexpr std::chrono::seconds kNoAgain {10};
constexpr std::chrono::seconds kTrustYes {30};
constexpr std::chrono::milliseconds kDragWait {1500};

// Whether the server of a network path answers, asked here and now
// (share::reachablePath: a few milliseconds, at most 1.5 seconds; a WebDAV
// server over HTTP, GitHub #114); true for a local path, WSL's state without
// asking. Asked afresh, as asking Windows about a file on a server that
// went away (even seconds ago) holds the launcher up for half a minute.
// Actions on files on servers ask off the UI thread (checkServers).
bool serverAnswers(const QString& path)
{
    if (!path.startsWith(u"\\\\"))
        return true;
    const std::wstring p = path.toStdWString();
    if (!unc::wslDistribution(p).empty())
        return wsl::cachedState(p) == wsl::State::Running;
    return share::reachablePath(p, true);
}

// A path on an SMB or WebDAV server: asked about off the UI thread
// (checkServers). WSL's are not: wsl::probe, and its own state.
bool onServer(const QString& path)
{
    const std::wstring p = path.toStdWString();
    return unc::isUnc(p) && unc::wslDistribution(p).empty();
}

// How the footer names the server of a path: "nas", "nas:5006" (WebDAV).
QString serverName(const QString& path)
{
    return QString::fromStdWString(webdav::displayHost(unc::host(path.toStdWString())));
}

// The server of a path, for asking each once: an SMB one and a WebDAV one
// of the same name are two.
QString serverKey(const QString& path)
{
    return serverName(path).toLower() + (share::smbPath(path.toStdWString()) ? u""_s : u" webdav"_s);
}

// For a drag, which starts now or not at all (the shell reads the files as
// it goes: a server gone away holds it up for long): which of `asked` (a
// path on each server) answer. A server heard from lately is taken at its
// word (kNoAgain; Windows' yes for kTrustYes, the probe's no since
// notwithstanding); the others are asked all at once, kDragWait for all of
// them on the UI thread (a WebDAV server's name looked up within it too:
// webdav::answers).
QSet<QString> dragAnswers(const QStringList& asked)
{
    struct Probes {
        std::mutex mutex;
        std::condition_variable cv;
        QSet<QString> yes;
        int asking = 0;
    };
    const auto probes = std::make_shared<Probes>(); // a probe late for the drag still holds it
    QSet<QString> answer;
    for (const QString& path : asked) {
        const std::wstring p = path.toStdWString();
        if (const std::optional<bool> heard = share::lastHeard(p, kNoAgain, kTrustYes)) {
            if (*heard)
                answer.insert(path);
            continue;
        }
        {
            std::lock_guard lock(probes->mutex);
            ++probes->asking;
        }
        std::thread([probes, path, p] {
            const bool yes = share::reachablePath(p, true, kDragWait - std::chrono::milliseconds(100));
            std::lock_guard lock(probes->mutex);
            if (yes)
                probes->yes.insert(path);
            --probes->asking;
            probes->cv.notify_all();
        }).detach();
    }
    std::unique_lock lock(probes->mutex);
    probes->cv.wait_for(lock, kDragWait, [&] { return probes->asking == 0; });
    answer.unite(probes->yes);
    return answer;
}

// Off the UI thread: whether `path` on a server is there, the probe and
// Windows asked at once (share::reach; Windows gets where the probe may not,
// GitHub #116; a WebDAV one's through WebClient), Windows for `windowsWait`.
share::Reach reachFile(const std::wstring& path, std::chrono::milliseconds windowsWait)
{
    return share::reach(share::actionSteps(path, kServerWait), windowsWait);
}

// Off the UI thread: whether `path` is there, on a server that answered a
// moment ago (reachFile): Windows alone, waited for as long.
share::Reach lookUp(const std::wstring& path, std::chrono::milliseconds windowsWait)
{
    share::ReachSteps steps = share::actionSteps(path, kServerWait);
    steps.probe = [] { return false; }; // asked already
    return share::reach(std::move(steps), windowsWait);
}

// Off the UI thread: what is there of `paths`, all on servers: each server
// asked once (reachFile), the rest of its files then while it answers
// (lookUp); every server at once, kServersWait for all (share::checkFiles).
// With WebClient started first (clientStarts), its servers longer.
QHash<QString, share::Reach> checkServers(const QStringList& paths, bool clientStarting)
{
    std::vector<std::wstring> files;
    for (const QString& path : paths)
        files.push_back(path.toStdWString());
    const auto windowsWait = [clientStarting](const std::wstring& file) {
        return clientStarting && webdav::isWebDavPath(file) ? kClientStartWait : kServerWait;
    };
    share::FileSteps steps;
    steps.server = [](const std::wstring& file) { return serverKey(QString::fromStdWString(file)).toStdWString(); };
    steps.first = [windowsWait](const std::wstring& file) { return reachFile(file, windowsWait(file)); };
    steps.next = [windowsWait](const std::wstring& file) { return lookUp(file, windowsWait(file)); };
    QHash<QString, share::Reach> found;
    for (const auto& [file, reach] : share::checkFiles(files, clientStarting ? kClientStartsWait : kServersWait, steps))
        found.insert(QString::fromStdWString(file), reach);
    return found;
}

// Whether an action on `paths` starts WebClient first (share::actionSteps):
// a WebDAV file among them, the service stopped and not disabled. Asked of
// Windows now: a moment.
bool clientStarts(const QStringList& paths)
{
    const bool webDav = std::any_of(
        paths.cbegin(), paths.cend(), [](const QString& p) { return webdav::isWebDavPath(p.toStdWString()); });
    return webDav && !webdav::clientRunning() && !webdav::clientDisabled();
}

// Why nothing was done with files on servers: the most telling of what was
// found for them (`paths` in their order; others left out). A server that
// did not answer, a share not there now, WebClient off; empty when they
// are gone (what is done says so then), or there.
QString whyNot(const QStringList& paths, const QHash<QString, share::Reach>& found)
{
    using R = share::Reach;
    QString shareAway;
    QString noService;
    for (const QString& path : paths) {
        const auto it = found.constFind(path);
        if (it == found.cend())
            continue;
        switch (*it) {
        case R::There:
        case R::Missing:
            break;
        case R::ShareAway:
            if (shareAway.isEmpty())
                shareAway = Launcher::tr("%1 上的共享暂时不可用，稍后再试").arg(serverName(path));
            break;
        case R::NoService:
            if (!share::smbPath(path.toStdWString())) {
                if (noService.isEmpty())
                    noService = webdav::clientDisabled() ? Launcher::tr("WebClient 服务被禁用，打不开 WebDAV 文件夹")
                                                         : Launcher::tr("WebClient 服务没能启动，打不开 WebDAV 文件夹");
                break;
            }
            [[fallthrough]]; // no service of WebDAV's: the network's
        case R::Unreachable:
            return Launcher::tr("连不上 %1（离线或不在同一网络）").arg(serverName(path));
        }
    }
    return !shareAway.isEmpty() ? shareAway : noService;
}

// What an action found out about files on servers goes to the shares they
// are in (GitHub #116, #114), found by the path that holds them, written
// either way for WebDAV (unc::canonical). One Online whose file was not got
// to has its server asked whether it is still there (checkReach: nothing is
// read); an offline one whose server answered comes back (verifyNow, as
// 立即核对). The card shows it, the rows follow (sharesChanged).
void tellShares(ShareIndexer& shares, const QHash<QString, share::Reach>& found)
{
    std::vector<std::pair<std::wstring, ShareIndexer::Status>> roots;
    for (ShareIndexer::Status& s : shares.statuses())
        roots.emplace_back(unc::canonical(s.path), std::move(s));
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        if (*it == share::Reach::NoService)
            continue; // Windows did not get as far as the server
        const std::wstring file = unc::canonical(it.key().toStdWString());
        const auto in = std::find_if(roots.cbegin(), roots.cend(), [&](const auto& root) {
            return unc::contains(root.first, file) || webdav::rootHolds(root.first, file);
        });
        if (in == roots.cend())
            continue;
        const ShareIndexer::Status& s = in->second;
        if (*it == share::Reach::Unreachable) {
            if (s.state == ShareIndexer::State::Online)
                shares.checkReach(s.path);
        } else if (s.state == ShareIndexer::State::Problem) {
            shares.verifyNow(s.path);
        }
    }
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
// The screenshot plugin is winshun.capture too (its own "cap" keyword, command "capture"): the history
// is told from it by its command, or "cap 白板" would list pictures and never ask the plugin.
bool isShotHistory(const QString& pluginId, const QString& commandId)
{
    return pluginId == kShotPlugin && commandId == kShotCommand;
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
    // Typed in a folder's window (GitHub #110), a row put among the rows
    // found elsewhere is listed as one of them: they stay together, under
    // their caption (ResultModel::elsewhereRow).
    void append(const SearchResults& rows) override
    {
        SearchResults placed = rows;
        for (SearchResult& r : placed)
            r.elsewhere = m_launcher.m_results.elsewhereRow() >= 0;
        m_launcher.m_results.append(placed);
    }
    void insert(int index, const SearchResult& row) override
    {
        const int elsewhere = m_launcher.m_results.elsewhereRow();
        SearchResult placed = row;
        placed.elsewhere = elsewhere >= 0 && index > elsewhere;
        m_launcher.m_results.insert(index, placed);
    }
    void update(int index, const SearchResult& row) override
    {
        SearchResult placed = row;
        if (const SearchResult* was = m_launcher.m_results.at(index))
            placed.elsewhere = was->elsewhere;
        m_launcher.m_results.update(index, placed);
    }
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
    , m_queryHistory(QFileInfo(history->filePath()).dir().filePath(u"query-history.json"_s))
    , m_sink(std::make_unique<ModelSink>(*this))
{
    m_columns.load(Settings::filePath());
    m_queryHistory.load();
    m_panelSidebarVisible = QSettings(Settings::filePath(), QSettings::IniFormat).value(u"Panel/SidebarVisible"_s, true).toBool();
    m_panelFiltersOnTop = QSettings(Settings::filePath(), QSettings::IniFormat).value(u"Panel/FiltersOnTop"_s, false).toBool();
    m_contentPane = std::make_unique<ContentPane>(index);
    const auto syncPane = [this] { syncContentPane(); };
    connect(this, &Launcher::currentRowChanged, this, syncPane);
    connect(this, &Launcher::statusChanged, this, syncPane);
    connect(this, &Launcher::scopeChanged, this, syncPane);
    connect(this, &Launcher::folderChipsChanged, this, syncPane);
    connect(&m_results, &ResultModel::selectionChanged, this, syncPane);
    connect(m_contentPane.get(), &ContentPane::openRequested, this, [this](const QString& key, const QString& path, int line) {
        // A plugin's row (GitHub #202): its file, only as its own Enter
        // would open it (held to the plugin's permissions, OpenPolicy).
        if (plugin::isPluginPath(key)) {
            const int row = m_results.indexOf(key);
            const SearchResult* r = m_results.at(row);
            if (r && r->isPlugin() && plugin::asksDetails(*r->plugin)) {
                trigger(row, Open); // no file of its own: what its Enter does
                return;
            }
            if (!r || !r->isPlugin() || plugin::rowFile(*r->plugin).compare(path, Qt::CaseInsensitive) != 0
                || !pluginOpenAllowed(r->plugin->pluginId, plugin::Open {plugin::Open::Kind::Path, path, {}, {}}))
                return;
        }
        remember(path, m_query);
        contentopen::open(path, line, reportFailure(QFileInfo(path).fileName()));
    });
    m_results.setFolderIndex([index] { return index ? index->index() : nullptr; });
    // A plugin's details row on show when the pane comes to its matches,
    // text or preview: asked for now.
    const auto paneShown = [this] {
        const SearchResult* r = m_results.at(m_currentRow);
        if (r && r->isPlugin() && plugin::asksDetails(*r->plugin))
            syncContentPane();
    };
    connect(m_contentPane.get(), &ContentPane::preferencesChanged, this, paneShown);
    connect(m_contentPane.get(), &ContentPane::activeChanged, this, paneShown);
    // Into the panel's details and out again: the preview window gives way to
    // their 预览 tab, and comes back after (GitHub #224).
    connect(this, &Launcher::panelChanged, this, &Launcher::handOverPreview);
    connect(this, &Launcher::scopeChanged, this, &Launcher::handOverPreview);
    connect(m_contentPane.get(), &ContentPane::preferencesChanged, this, &Launcher::handOverPreview);
    m_detailsTimer.setSingleShot(true);
    m_detailsTimer.setInterval(120ms); // as a once plugin's queries: not for each row the keys pass
    connect(&m_detailsTimer, &QTimer::timeout, this, &Launcher::askDetails);
    connect(this, &Launcher::resultsReplaced, this, [this] { m_detailsFailed.clear(); });
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
    connect(&m_contentDebounce, &QTimer::timeout, this, [this] {
        // The pause waited for is no lateness: how long the rows take is
        // counted from here (showRows' line in the log; it came with every
        // search of 内容 otherwise).
        if (m_replacePending && m_replaceClock.isValid())
            m_replaceClock.restart();
        startContentSearch();
    });

    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(2s);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        m_flash.clear();
        emit statusChanged();
    });

    m_armedPowerTimer.setSingleShot(true);
    m_armedPowerTimer.setInterval(5s);
    connect(&m_armedPowerTimer, &QTimer::timeout, this, [this] { disarmPower(); });

    m_runningTick.setInterval(1500ms);
    connect(&m_runningTick, &QTimer::timeout, this, &Launcher::runningTicked);

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
    connect(m_engine, &SearchEngine::nameLeftOut, this, &Launcher::onNameLeftOut);
    connect(m_engine, &SearchEngine::contentResults, this, &Launcher::onContentResults);
    connect(m_engine, &SearchEngine::contentProgress, this, &Launcher::onContentProgress);
    connect(m_engine, &SearchEngine::contentLeftOut, this, &Launcher::onContentLeftOut);
    connect(m_engine, &SearchEngine::contentScope, this, &Launcher::onContentScope);
    connect(this, &Launcher::typingFolderChanged, this, &Launcher::refreshKeyHints);
    connect(this, &Launcher::folderChipsChanged, this, &Launcher::refreshKeyHints); // Tab: 换个文件夹, or none to go to
    connect(this, &Launcher::queryChanged, this, &Launcher::folderChipsChanged); // a folder typed, or no longer
    connect(this, &Launcher::scopeChanged, this, &Launcher::folderChipsChanged);
    connect(m_engine, &SearchEngine::removableDrives, this, &Launcher::onRemovableDrives);
    connect(m_engine, &SearchEngine::driveResults, this, &Launcher::onDriveResults);
    connect(m_engine, &SearchEngine::driveProgress, this, &Launcher::onDriveProgress);
    m_indexSettled = m_index->state() == IndexService::State::Ready && !m_index->isRefreshing();
    m_searchAgain.setSingleShot(true);
    connect(&m_searchAgain, &QTimer::timeout, this, &Launcher::searchAgainNow);
    connect(m_index, &IndexService::stateChanged, this, [this] {
        // A finished index (built, or read again) can only improve the
        // current results: once, when it gets there, not at every word of it.
        const bool settled = m_index->state() == IndexService::State::Ready && !m_index->isRefreshing();
        if (settled && !std::exchange(m_indexSettled, settled) && m_window && m_window->isVisible() && m_scope != Content)
            searchFor("the index's state changed");
        m_indexSettled = settled;
        refreshStatus();
    });
    connect(m_index, &IndexService::sharesChanged, this, [this] {
        // A share gone offline or back: its rows say so now, as its card
        // does, not at the next search (GitHub #116); only those that change.
        if (!m_window || !m_window->isVisible())
            return;
        bool current = false;
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (!r || r->isApp() || !r->path.startsWith(u"\\\\"))
                continue;
            if (const bool offline = m_index->shares().offline(r->path.toStdWString()); offline != r->offline) {
                SearchResult changed = *r;
                changed.offline = offline;
                m_results.update(row, changed);
                current = current || row == m_currentRow;
            }
        }
        if (current)
            followPreview(); // an offline row's file is not previewed
    });
    connect(m_apps, &AppCatalog::changed, this, [this] {
        // Apps installed or removed (or the first list arrived): 全部 shows them.
        if (m_window && m_window->isVisible() && (m_scope == All || m_scope == Apps))
            searchFor("the apps changed");
        refreshFrequentApps(); // one whose program was deleted goes from the row too (#207)
        refreshStatus();
    });
    connect(m_places, &SystemCatalog::changed, this, [this] {
        if (m_window && m_window->isVisible() && m_scope == All)
            searchFor("the settings places changed");
    });
    refreshStatus();
}

Launcher::~Launcher()
{
    m_windowsUsesWorker.request_stop();
    if (m_windowsUsesWorker.joinable())
        m_windowsUsesWorker.join();
}

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

void Launcher::updateQueryPrefix()
{
    // Less the quick filters named in it: ":nobin :pic 报告" is both.
    const FileTypeQuery parsed = splitFileTypeQuery(typedQuick().filters.isEmpty() ? m_query : typedQuick().words, m_fileTypes, m_folderKeyword);
    const Scope oldScope = m_scope;
    const int oldType = m_fileType;
    const auto oldExtra = m_extraFileTypes;
    const bool oldFolders = m_foldersChosen;
    if (parsed.type) {
        if (!m_filePrefix) {
            QStringList keys;
            for (int type : selectedFileTypes())
                keys.append(fileTypeKey(m_fileTypes[type]));
            m_filePrefix = PrefixScope{m_scope, keys, m_foldersChosen};
        }
        m_prefixWords = parsed.words;
        m_scope = *parsed.type == kFoldersChip ? Folders : Files;
        m_fileType = std::max(-1, *parsed.type);
        m_extraFileTypes = parsed.types;
        m_extraFileTypes.removeAll(m_fileType);
        m_foldersChosen = m_scope == Folders;
    } else if (m_filePrefix) {
        m_scope = m_filePrefix->scope;
        QList<int> types;
        for (const QString& key : m_filePrefix->typeKeys) {
            const int type = fileTypeIndex(key);
            if (type >= 0)
                types.append(type);
        }
        m_fileType = types.isEmpty() ? -1 : types.takeFirst();
        m_extraFileTypes = types;
        m_foldersChosen = m_filePrefix->folders;
        m_filePrefix.reset();
        m_prefixWords.clear();
    }
    if (oldType != m_fileType || oldExtra != m_extraFileTypes)
        emit fileTypeChanged();
    if (oldType != m_fileType || oldExtra != m_extraFileTypes || oldFolders != m_foldersChosen)
        emit fileChoiceChanged();
    if (oldScope != m_scope || oldType != m_fileType || oldExtra != m_extraFileTypes)
        emit scopeChanged();
}

bool Launcher::clearQueryPrefix()
{
    if (!m_filePrefix)
        return false;
    m_query = m_prefixWords;
    m_filePrefix.reset();
    m_prefixWords.clear();
    emit queryChanged();
    // The prefix's kinds stay on the strip as the user's own now; what is
    // kept was the strip before the prefix (fileChoice) until here.
    emit fileChoiceChanged();
    return true;
}

void Launcher::setFolderKeyword(const QString& keyword)
{
    if (m_folderKeyword == keyword)
        return;
    m_folderKeyword = keyword;
    emit fileTypesChanged();
    updateQueryPrefix();
    if (m_window && m_window->isVisible())
        search();
    else
        m_shownStale = true;
}

void Launcher::setSearchKinds(bool apps, bool places)
{
    if (apps == m_searchApps && places == m_searchPlaces)
        return;
    m_searchApps = apps;
    m_searchPlaces = places;
    if (m_window && m_window->isVisible())
        search();
    else
        m_shownStale = true;
}

void Launcher::setRegexMode(bool on)
{
    if (on == m_regexMode)
        return;
    m_regexMode = on;
    emit regexModeChanged();
    if (m_window && m_window->isVisible())
        search();
    else
        m_shownStale = true;
}

void Launcher::toggleRegexMode()
{
    setRegexMode(!m_regexMode);
    if (m_window && m_window->isVisible())
        flash(!m_regexMode ? tr("已关闭正则模式")
            : m_scope == Content ? tr("正则模式：每个词都按正则表达式匹配文件里的文字")
            : tr("正则模式：每个词都按正则表达式匹配名字"));
}

void Launcher::setFuzzyMode(const QString& mode)
{
    const QString clean = fuzzyModeName(fuzzyModeFromName(mode));
    if (clean == m_fuzzyMode)
        return;
    if (clean != u"on")
        m_fuzzyBeforeOn = clean;
    m_fuzzyMode = clean;
    emit fuzzyModeChanged();
    if (m_window && m_window->isVisible())
        search();
}

void Launcher::toggleFuzzyMode()
{
    const bool turningOff = m_fuzzyMode == u"on";
    setFuzzyMode(turningOff ? m_fuzzyBeforeOn : u"on"_s);
    if (m_window && m_window->isVisible()) {
        flash(m_fuzzyMode == u"on" ? tr("已始终开启模糊匹配（常规结果仍排在前面）")
            : m_fuzzyMode == u"off" ? tr("已关闭模糊匹配（仍可用 fz: 按需触发）")
            : tr("已切回无结果时自动模糊兜底"));
    }
}

void Launcher::setQuery(const QString& query)
{
    if (query == m_query)
        return;
    disarmPower();
    m_query = query;
    m_queryRecorded = false;
    if (!m_recallingQuery) m_queryHistory.resetNavigation();
    // The folder typed in, edited: a path typed from now on (GitHub #110).
    if (!m_typingPrefix.isEmpty() && !m_query.startsWith(m_typingPrefix, Qt::CaseInsensitive))
        setTypingFolder({});
    // GitHub #361: the list for these words starts on its first row, chosen
    // by nobody; back out of a folder (returnTo), on that folder's row. The
    // steps into folders the box has left are forgotten.
    m_chosenPath = m_returnRow;
    m_results.setStartPath(m_returnRow);
    m_tabHeld = false; // typed on: a Tab waiting for the rows before is not for these
    const QString inFolder = pathtext::splitTyped(m_query).folder;
    while (!m_folderSteps.isEmpty() && (inFolder.isEmpty() || !navigation::within(m_folderSteps.last().folder, inFolder)))
        m_folderSteps.removeLast();
    updateQueryPrefix();
    emit queryChanged();
    followFilterSort(); // a quick filter typed by its keyword, or taken away
    m_results.setSelection({});
    m_held.reset(); // typed on: a key held for the rows before is not for these
    search();
}

void Launcher::setMergeFileScope(bool merged)
{
    if (m_mergeFileScope == merged) return;
    m_mergeFileScope = merged;
    if (merged && m_scope == Files && m_fileType < 0) setScope(All);
    emit scopeChanged();
}

void Launcher::setHistoryKeys(const QString& keys)
{
    if (m_historyKeys == keys) return;
    m_historyKeys = keys;
    emit historyChanged();
}

void Launcher::setLogoRightClick(const QString& action)
{
    if (m_logoRightClick == action) return;
    m_logoRightClick = action;
    emit logoRightClickChanged();
}

void Launcher::rememberQuery()
{
    // A plugin's keyword too ("= 3*7", GitHub #395), unless its command keeps
    // what is typed to it out (hashes, Base64: passwords and tokens go there).
    if (m_recordHistory && !m_queryRecorded && (!m_keyword || m_keyword->history) && !m_systemCommand && !m_folderSearch) {
        m_queryRecorded = true;
        if (m_queryHistory.record(m_query)) {
            emit queryHistoryChanged();
            emit historyChanged();
        }
    }
}

bool Launcher::navigateQueryHistory(int direction)
{
    if (!m_recordHistory) return false;
    const auto query = m_queryHistory.step(m_query, direction);
    if (!query) return false;
    const QScopedValueRollback recalling(m_recallingQuery, true);
    setQuery(*query);
    emit caretRequested(static_cast<int>(m_query.size()), false);
    return true;
}

void Launcher::recallQuery(int index)
{
    if (!m_recordHistory || index < 0 || index >= m_queryHistory.items().size()) return;
    setQuery(m_queryHistory.items()[index]);
    emit caretRequested(static_cast<int>(m_query.size()), false);
}

void Launcher::removeQuery(int index)
{
    if (index >= 0 && index < m_queryHistory.items().size() && m_queryHistory.items()[index] == m_query.trimmed())
        m_queryRecorded = true;
    if (m_queryHistory.remove(index)) {
        emit queryHistoryChanged();
        emit historyChanged();
    }
}

void Launcher::setScope(Scope scope)
{
    const bool hadPrefix = clearQueryPrefix();
    if (scope == Content && !m_contentEnabled)
        scope = All; // a plugin's row may still ask for it
    if (scope == Apps && !appsTab())
        scope = All;
    m_appsLeft = false; // chosen: nothing to come back to
    // Back in 文件: as it was left (文件夹, or the kind chosen kept meanwhile).
    if (scope == Files && m_scope != Folders && m_foldersChosen)
        scope = Folders;
    if (scope == m_scope) {
        if (hadPrefix)
            search();
        return;
    }
    // A different search scope must not carry a floating preview of its
    // old results over the new controls (#186).
    const bool intoDetails = m_panel && scope == Content && m_contentPane && m_contentPane->opened();
    if (m_preview && m_preview->isOpen() && !intoDetails) {
        closePreview();
        m_previewResume = false;
        m_previewHandedOver = false;
    }
    m_scope = scope;
    if ((scope == Files || scope == Folders) && m_foldersChosen != (scope == Folders)) {
        m_foldersChosen = scope == Folders;
        emit fileChoiceChanged();
    }
    emit scopeChanged();
    m_results.setSelection({});
    m_held.reset();
    m_chosenPath.clear(); // another list: its first row, as typing gives (GitHub #361)
    m_results.setStartPath({});
    m_tabHeld = false;
    refreshKeyHints(); // Tab: through the kinds in 文件
    // Keep the old rows until the new results replace them: clearing first
    // collapses the window and then grows it back (flicker).
    search();
}

void Launcher::chooseScope(Scope scope)
{
    setScope(scope);
    rememberScope();
}

void Launcher::rememberScope() const
{
    if (m_stateFile.isEmpty())
        return;
    // By its tab: 文件夹 is under 文件 (which of the two, the strip's choice keeps: Search/FileType).
    const QString tab = m_scope == Content ? u"content"_s : m_scope == Apps ? u"apps"_s : m_scope == All ? u"all"_s : u"files"_s;
    QSettings(m_stateFile, QSettings::IniFormat).setValue(u"Search/Scope"_s, tab);
}

void Launcher::restoreScope()
{
    if (m_stateFile.isEmpty())
        return;
    const QString tab = QSettings(m_stateFile, QSettings::IniFormat).value(u"Search/Scope"_s).toString();
    Scope scope = All;
    if (tab == u"content" && m_contentEnabled)
        scope = Content;
    else if (tab == u"apps" && appsTab())
        scope = Apps;
    else if (tab == u"files" && !(m_mergeFileScope && m_fileType < 0 && !m_foldersChosen)) // as setMergeFileScope has it
        scope = m_foldersChosen ? Folders : Files;
    if (scope == m_scope)
        return;
    // Not through setScope: nothing is searched before the first showing,
    // which searches itself (handleShown), as after startOver.
    m_scope = scope;
    emit scopeChanged();
    refreshKeyHints();
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
    if (!m_haveResults || m_replacePending || m_keyword || searchText().isEmpty())
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

void Launcher::setExcludedFolders(const QStringList& paths, const QStringList& names, bool everythingCompat,
    const std::optional<QStringList>& selectedDrives)
{
    if (paths == m_excludedPathsRaw && names == m_excludedNamesRaw && everythingCompat == m_everythingCompat
        && selectedDrives == m_selectedDrives)
        return;
    const bool compatChanged = everythingCompat != m_everythingCompat;
    m_excludedPathsRaw = paths;
    m_excludedNamesRaw = names;
    m_everythingCompat = everythingCompat;
    m_selectedDrives = selectedDrives;
    m_outsideKnown = false;

    CrawlRules rules;
    for (const QString& p : paths) {
        if (p.trimmed().isEmpty())
            continue;
        if (std::wstring key = Settings::excludedCompareKey(p); !key.empty())
            rules.excludedPaths.push_back(std::move(key));
    }
    for (const QString& n : names) {
        const QString trimmed = n.trimmed();
        if (!trimmed.isEmpty())
            rules.excludedNames.push_back(QDir::toNativeSeparators(trimmed).toStdWString());
    }
    m_excludedCrawler = !rules.excludedPaths.empty() || !rules.excludedNames.empty()
        ? std::make_unique<Crawler>(rules)
        : nullptr;

    const auto atOrUnder = [](std::wstring_view path, std::wstring_view dir) {
        return !dir.empty() && path.size() >= dir.size() && (path.size() == dir.size() || path[dir.size()] == L'\\')
            && win32::equalsIgnoreCase(path.substr(0, dir.size()), dir);
    };
    const std::wstring windir = Settings::excludedCompareKey(u"%WINDIR%"_s);
    const std::wstring winSxs = Settings::excludedCompareKey(u"%WINDIR%\\WinSxS"_s);
    const std::wstring winRe = Settings::excludedCompareKey(u"%SystemDrive%\\$WinREAgent"_s);

    struct Candidate {
        ExcludedFolder entry;
        std::wstring wide;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(paths.size()));
    for (const QString& raw : paths) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty())
            continue;
        std::wstring norm = Settings::excludedCompareKey(trimmed);
        if (norm.size() < 3 || norm[1] != L':' || norm[2] != L'\\' || norm.find(L'%') != std::wstring::npos
            || unc::isUnc(norm))
            continue;
        const int priority = atOrUnder(norm, winSxs) ? 2
                           : (atOrUnder(norm, windir) || atOrUnder(norm, winRe)) ? 1
                                                                                 : 0;
        const QString qpath = QString::fromStdWString(norm);
        const bool bareEnvVar = trimmed.size() >= 3 && trimmed.front() == u'%' && trimmed.back() == u'%'
            && trimmed.indexOf(u'%', 1) == trimmed.size() - 1;
        const QString label = bareEnvVar ? trimmed : folderLabel(qpath);
        candidates.push_back({ExcludedFolder {trimmed, qpath, label, priority}, std::move(norm)});
    }
    m_excludedFolders.clear();
    m_excludedFolders.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const std::wstring& path = candidates[i].wide;
        const bool duplicate = std::any_of(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(i),
            [&](const Candidate& prev) { return win32::equalsIgnoreCase(prev.wide, path); });
        const bool nested = std::ranges::any_of(candidates, [&](const Candidate& other) {
            return other.wide.size() < path.size() && atOrUnder(path, other.wide);
        });
        if (!duplicate && !nested)
            m_excludedFolders.push_back(std::move(candidates[i].entry));
    }
    std::ranges::stable_sort(m_excludedFolders, {}, &ExcludedFolder::priority);

    if (m_window && m_window->isVisible()) {
        if (compatChanged)
            search();
        else
            refreshStatus();
    }
}

void Launcher::noteSearchHere(const QString& folder)
{
    m_hereFolder = folder;
    m_hereSeen = false;
}

void Launcher::setTypingFolder(const QString& prefix)
{
    const QString folder = pathtext::pathFromText(prefix);
    if (prefix.isEmpty() || folder.isEmpty()) {
        if (m_typingPrefix.isEmpty())
            return;
        m_typingPrefix.clear();
        m_typingFolder.clear();
        emit typingFolderChanged();
        return;
    }
    m_typingPrefix = prefix;
    m_typingFolder = folder;
    m_typingOnly = m_typingOnlyDefault; // as set, each time; switched, for this time only
    m_typingLevel = m_typingOnly && m_typingLevelDefault;
    emit typingFolderChanged();
    // The words after it, if any are typed already, are looked for anew.
    if (!typingWords().isEmpty() && m_window && m_window->isVisible())
        search();
}

void Launcher::awaitTyping(const QString& text)
{
    m_awaitedText = text.trimmed();
    setTypingAwaited(true);
}

void Launcher::setTypingAwaited(bool awaited)
{
    if (m_typingAwaited == awaited)
        return;
    m_typingAwaited = awaited;
    emit typingAwaitedChanged();
}

void Launcher::setFolderSearch(bool enabled)
{
    if (m_folderSearch == enabled)
        return;
    m_folderSearch = enabled;
    if (!m_window || !m_window->isVisible())
        m_shownStale = true;
    emit folderSearchChanged();
    appsTabMoved();
}

void Launcher::setFolderTyping(const QString& how)
{
    m_typingLevelDefault = how == u"level";
    m_typingOnlyDefault = m_typingLevelDefault || how == u"only";
}

QString Launcher::typingFolder() const
{
    return m_typingPrefix.isEmpty() ? QString() : folderLabel(m_typingFolder);
}

const QString& Launcher::typedWithoutKind() const
{
    return m_filePrefix ? m_prefixWords : m_query;
}

bool Launcher::typedInFolder() const
{
    // A kind of file named after the folder ("D:\work\ doc: 报告") is taken
    // out of the box's text (updateQueryPrefix); what is left still starts
    // with the folder, and its words are looked for there first, then
    // elsewhere, as without it (GitHub #366).
    return !m_typingPrefix.isEmpty() && m_scope != Content && !m_panel
        && typedWithoutKind().startsWith(m_typingPrefix, Qt::CaseInsensitive);
}

QString Launcher::typingWords() const
{
    return typedInFolder() ? typedWithoutKind().mid(m_typingPrefix.size()).trimmed() : QString();
}

QString Launcher::nameText() const
{
    const QString words = typingWords();
    if (!words.isEmpty())
        return words;
    if (const auto files = commandFiles())
        return files->words;
    return searchText();
}

std::optional<Launcher::CommandFiles> Launcher::commandFiles() const
{
    if (!m_favorites || m_keyword || m_filePrefix || (m_scope != All && m_scope != Files) || typedInFolder())
        return std::nullopt;
    const QString text = searchText();
    const qsizetype at = typedCommandKeyword(m_favorites->items(), text);
    if (at < 0)
        return std::nullopt;
    const Favorite& f = m_favorites->items()[at];
    if (!f.searchFiles || !f.command.contains(u"{query}"))
        return std::nullopt;
    const QString words = commandWords(f, text);
    if (words.isEmpty())
        return std::nullopt;
    return CommandFiles {f, words};
}

bool Launcher::fileRow(const SearchResult& r)
{
    return !r.isApp() && !r.isCommand() && r.line == 0 && !r.missing && !r.path.isEmpty();
}

QString Launcher::typingKey() const
{
    if (typingWords().isEmpty())
        return {};
    return (m_typingLevel ? u"level:"_s : m_typingOnly ? u"only:"_s : u"first:"_s) + m_typingFolder;
}

bool Launcher::switchTypingFolder()
{
    // Its rows first, then only the folder and below, then only its own level.
    return setTypingScope(m_typingLevel ? TypingScope::Everywhere
                                        : m_typingOnly ? TypingScope::Level : TypingScope::Folder);
}

bool Launcher::setTypingScope(TypingScope scope)
{
    if (!typedInFolder())
        return false;
    m_typingOnly = scope != TypingScope::Everywhere;
    m_typingLevel = scope == TypingScope::Level;
    emit typingFolderChanged();
    if (!typingWords().isEmpty())
        search(); // a list of its own (typingKey): the first row is current again
    else
        refreshStatus();
    return true;
}

Launcher::OutsideTarget Launcher::outsideTarget() const
{
    if (m_scope == Content || m_keyword || regexTerms())
        return {};
    const QString text = searchText();
    const QString panelFolder = m_panel ? m_folderFilter : QString();
    const QString cacheKey = panelFolder.isEmpty() ? text : text + u'\0' + panelFolder;
    if (m_outsideKnown && m_outsideQuery == cacheKey)
        return m_outside;
    m_outsideKnown = true;
    m_outsideQuery = cacheKey;
    m_outside = {};
    const pathtext::TypedPath typed = pathtext::splitTyped(text);
    const QString target = !typed.folder.isEmpty() ? typed.folder : panelFolder;
    if (target.size() < 2 || !target[0].isLetter() || target[1] != u':')
        return m_outside;

    OutsideTarget::Kind kind = OutsideTarget::Kind::None;
    const std::wstring key = Settings::excludedCompareKey(target);
    const bool excluded = !key.empty() && m_excludedCrawler
        && (!m_everythingCompat
                ? m_excludedCrawler->isExcludedPath(key)
                : m_excludedCrawler->isExcludedDir(key.substr(0, 2), key.substr(0, 2)));
    if (excluded)
        kind = OutsideTarget::Kind::Excluded;
    else if (m_foldersOnly && !pathtext::inChosenFolders(m_chosenFolders, target))
        kind = OutsideTarget::Kind::NotChosen;
    else if (!m_foldersOnly && m_selectedDrives && !m_selectedDrives->contains(target.left(2), Qt::CaseInsensitive))
        kind = OutsideTarget::Kind::UnselectedDrive;
    if (kind == OutsideTarget::Kind::None)
        return m_outside;

    // A fixed disk of this PC, and the folder there: a removable drive has its
    // own hint, a share its own list, and a folder that is not there holds nothing.
    const std::wstring root = (target.left(2) + u'\\').toStdWString(); // "D:\"
    if (::GetDriveTypeW(root.c_str()) == DRIVE_FIXED && QFileInfo(target).isDir())
        m_outside = {kind, target};
    return m_outside;
}

std::vector<const Launcher::ExcludedFolder*> Launcher::applicableExcludedFolders() const
{
    std::vector<const ExcludedFolder*> result;
    if (m_everythingCompat || m_excludedFolders.empty())
        return result;
    const QString text = searchText();
    const pathtext::TypedPath typed = regexTerms() ? pathtext::TypedPath() : pathtext::splitTyped(text);
    const QString scopeFolder = !typed.folder.isEmpty() ? typed.folder
                              : (m_panel && !m_folderFilter.isEmpty()) ? m_folderFilter
                                                                       : QString();
    for (const ExcludedFolder& entry : m_excludedFolders) {
        if (m_foldersOnly) {
            if (!pathtext::inChosenFolders(m_chosenFolders, entry.path))
                continue;
        } else if (m_selectedDrives && !m_selectedDrives->contains(entry.path.left(2), Qt::CaseInsensitive)) {
            continue;
        }
        if (!scopeFolder.isEmpty() && !pathtext::inChosenFolders({scopeFolder}, entry.path))
            continue;
        result.push_back(&entry);
    }
    return result;
}

// "Search here" named a folder that is not indexed: what is typed after it is
// looked for in it all the same, the user having picked that folder. Once the
// names are listed, and when there is something to look for.
void Launcher::walkHereSoon()
{
    if (m_hereFolder.isEmpty())
        return;
    const OutsideTarget outside = outsideTarget();
    if (outside.folder.isEmpty() || !pathtext::sameFolder(outside.folder, m_hereFolder)) {
        if (m_hereSeen || (!searchText().isEmpty() && !pathtext::sameFolder(pathtext::splitTyped(searchText()).folder, m_hereFolder)))
            m_hereFolder.clear(); // typed on elsewhere
        return;
    }
    m_hereSeen = true;
    const QString kind = kFolderHint + outside.folder;
    if (!m_hintRunning.isEmpty() || m_hintsTaken.contains(kind) || pathtext::splitTyped(searchText()).name.trimmed().isEmpty()
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
    if (m_scope == Apps)
        return hints; // nothing is passed over: the apps are all there
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
        else if (m_hintRunning == kHiddenFoldersHint)
            text = tr("正在读取排除目录里的结果…");
        else if (m_hintRunning == kExcludedFilesHint)
            text = tr("正在读取已跳过的文件…");
        else if (m_hintRunning == kExcludedFoldersHint)
            text = tr("正在搜排除目录… 已看 %Ln 项", nullptr, m_walkSeen);
        else
            text = tr("正在搜 %1… 已看 %Ln 项", nullptr, m_walkSeen).arg(QDir::toNativeSeparators(walkedPath(m_hintRunning)));
        add(m_hintRunning, text, tr("停止"));
        return hints;
    }
    if (!hintsSettled())
        return hints;
    // Typed in a folder's window, only that folder searched (GitHub #110):
    // first, the rest of the index after its rows; under no rows at all, the
    // way out of 没有找到. Not drives then: only that folder was asked for.
    const bool onlyTypingFolder = m_typingOnly && !typingWords().isEmpty();
    if (onlyTypingFolder) {
        add(kEverywhereHint,
            m_typingLevel ? tr("只在 %1 这一层搜，不含子文件夹").arg(typingFolder()) : tr("只在 %1 里搜").arg(typingFolder()),
            tr("也搜其他位置"));
        add(kLevelHint, m_typingLevel ? tr("没有搜子文件夹里的") : tr("包含子文件夹里的"),
            m_typingLevel ? tr("含子文件夹") : tr("只搜这一层"));
    }
    // Name-search left-out hints come first so an empty filename search offers
    // excluded folders / skipped files before content size-limit hints.
    const bool noLocalResults = m_results.count() == 0 || m_webFallbackOnly;
    const bool sparseResults = noLocalResults || m_results.count() < m_maxRows;
    OutsideTarget outside;
    if (m_scope != Content && !m_moreNames) {
        if (!onlyTypingFolder && sparseResults) {
            if (m_nameLeftOut.hiddenFolders > 0 && !m_hintsTaken.contains(kHiddenFoldersHint)) {
                add(kHiddenFoldersHint,
                    noLocalResults ? tr("排除目录里有 %Ln 项", nullptr, m_nameLeftOut.hiddenFolders)
                                   : tr("排除目录里另有 %Ln 项", nullptr, m_nameLeftOut.hiddenFolders),
                    tr("这次也显示"), kExcludedSettingsHint, tr("排除设置…"));
            }
            if (m_nameLeftOut.excludedFiles > 0 && !m_hintsTaken.contains(kExcludedFilesHint)) {
                add(kExcludedFilesHint,
                    noLocalResults ? tr("已跳过的文件里有 %Ln 项", nullptr, m_nameLeftOut.excludedFiles)
                                   : tr("已跳过的文件里另有 %Ln 项", nullptr, m_nameLeftOut.excludedFiles),
                    tr("这次也显示"), kExcludedFileSettingsHint, tr("设置…"));
            }
        }
        outside = outsideTarget();
        const bool outsideOnDrive = !outside.folder.isEmpty()
            && std::any_of(m_drives.cbegin(), m_drives.cend(),
                [&](const RemovableDrive& drive) { return outside.folder.startsWith(drive.root, Qt::CaseInsensitive); });
        if (outside.kind != OutsideTarget::Kind::None && !outsideOnDrive) {
            const QString walkKind = kFolderHint + outside.folder;
            switch (outside.kind) {
            case OutsideTarget::Kind::Excluded:
                if (!m_hintsTaken.contains(walkKind))
                    add(walkKind, tr("这个文件夹在排除目录里，未建索引"), tr("这次也搜它"), kExcludedSettingsHint, tr("排除设置…"));
                else
                    add(kExcludedSettingsHint, tr("这个文件夹在排除目录里，未建索引"), tr("排除设置…"));
                break;
            case OutsideTarget::Kind::NotChosen:
                if (!m_hintsTaken.contains(walkKind))
                    add(walkKind, tr("这个文件夹不在索引范围内"), tr("这次也搜它"), kAddFolderHint + outside.folder, tr("加入索引"));
                else if (!m_hintsTaken.contains(kAddFolderHint + outside.folder))
                    add(kAddFolderHint + outside.folder, tr("这个文件夹不在索引范围内"), tr("加入索引"));
                break;
            case OutsideTarget::Kind::UnselectedDrive:
                if (!m_hintsTaken.contains(walkKind))
                    add(walkKind, tr("这个磁盘不在索引范围内"), tr("这次也搜它"), kScopeSettingsHint, tr("搜索范围…"));
                else
                    add(kScopeSettingsHint, tr("这个磁盘不在索引范围内"), tr("搜索范围…"));
                break;
            case OutsideTarget::Kind::None:
                break;
            }
        } else if (!onlyTypingFolder && noLocalResults && m_queryError.isEmpty() && !m_everythingCompat
            && !m_hintsTaken.contains(kExcludedFoldersHint)) {
            const std::vector<const ExcludedFolder*> excluded = applicableExcludedFolders();
            if (excluded.size() == 1) {
                add(kExcludedFoldersHint, tr("排除目录 %1 没搜").arg(excluded.front()->label), tr("这次也搜它"),
                    kExcludedSettingsHint, tr("排除设置…"));
            } else if (!excluded.empty()) {
                const QString text = excluded.front()->priority == 0
                    ? tr("%1 等 %Ln 个排除目录没搜", nullptr, static_cast<int>(excluded.size())).arg(excluded.front()->label)
                    : tr("另有 %Ln 个排除目录没搜", nullptr, static_cast<int>(excluded.size()));
                add(kExcludedFoldersHint, text, tr("也搜这些"), kExcludedSettingsHint, tr("排除设置…"));
            }
        }
    }
    if (m_scope == All && m_scanComplete && !m_moreContent && m_leftOut.deferred > 0)
        add(kDeferredHint, tr("还有 %Ln 个文件未搜索内容", nullptr, m_leftOut.deferred), tr("搜索内容"));
    // The counts are of files not read, whatever was typed: offered under
    // every list they would be noise. Only when what was found does not fill
    // the window (Main.qml maxRows), where "is that all?" is the question.
    if (m_scanComplete && !m_moreContent && m_results.count() < m_maxRows) {
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
        if (m_scope == Content && m_leftOut.remote > 0)
            add(kRemoteHint, tr("网络位置上还有 %Ln 个文件没建内容索引", nullptr, m_leftOut.remote), tr("设置…"));
    }
    if (m_scope != Content && !m_moreNames && !onlyTypingFolder) {
        for (const RemovableDrive& drive : m_drives) {
            const QString kind = kDriveHint + drive.root;
            if (m_hintsTaken.contains(kind))
                continue;
            add(kind,
                drive.label.isEmpty() ? tr("U 盘 %1 没收录").arg(drive.root) : tr("U 盘 %1（%2）没收录").arg(drive.root, drive.label),
                tr("这次也搜它"));
        }
    }
    // Only the chosen folders are indexed (GitHub #52): when few rows came,
    // a general note once (closed for good).
    if (m_scope != Content && !m_moreNames && outside.kind == OutsideTarget::Kind::None
        && pathtext::scopeNote(m_foldersOnly, false, m_folderNoteDismissed, static_cast<int>(m_results.count()),
               m_maxRows, searchText().size())
            == pathtext::ScopeNote::Chosen) {
        add(kFoldersHint, tr("只索引了你选的文件夹，可能有别的地方没搜到"), tr("设置文件夹…"), kFoldersDismissHint, tr("不再提示"));
    }
    return hints;
}

QString Launcher::approximateHeading() const
{
    if (!m_approximate || !m_haveResults || m_shownText.isEmpty())
        return {};
    // What was typed, on one line (as Main.qml shows it when nothing is found).
    static const QRegularExpression kSpaces(u"\\s+"_s);
    const QString shown = QString(m_shownText).replace(kSpaces, u" "_s);
    return m_approximateFuzzy
        ? tr("没有直接匹配“%1”的结果，按模糊匹配找到：").arg(shown)
        : tr("没有叫“%1”的，按读音找到：").arg(shown);
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
    if (kind == kDeferredHint) {
        setScope(Content); // explicit opt-in to read files, with the same query
        return;
    }
    // Not searches: the settings' page, the note closed, a folder added.
    if (kind == kRemoteHint) {
        emit shareSettingsRequested();
        return;
    }
    if (kind == kExcludedSettingsHint) {
        emit excludedSettingsRequested();
        return;
    }
    if (kind == kExcludedFileSettingsHint) {
        emit excludedFilesSettingsRequested();
        return;
    }
    if (kind == kScopeSettingsHint) {
        emit folderSettingsRequested();
        return;
    }
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
    if (kind == kEverywhereHint) { // a list of its own, the folder's rows first (GitHub #110)
        setTypingScope(TypingScope::Everywhere);
        return;
    }
    if (kind == kLevelHint) { // the folder's own level, or with its subfolders
        setTypingScope(m_typingLevel ? TypingScope::Folder : TypingScope::Level);
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
    request.excludedFileNames = m_excludedFileNames;
    if (kind == kHiddenFoldersHint || kind == kExcludedFilesHint) {
        request.text = nameText();
        if (!typingWords().isEmpty()) {
            request.currentFolder = m_typingFolder;
            request.currentFolderOnly = m_typingOnly;
            request.currentFolderLevel = m_typingOnly && m_typingLevel;
        }
        request.scope = m_scope == Folders ? ws::Scope::Folders : ws::Scope::Files;
        request.extensions = fileExtensions();
        request.typeFolders = fileTypeFolders();
        request.conditions = quickConditions();
        request.kindOrder = m_kindOrder;
        if (m_panel) {
            request.filter = panelFilter();
            if (!m_folderFilter.isEmpty())
                request.folders = {m_folderFilter};
        }
        request.order = nameOrder();
        request.limit = kPage;
        request.regex = regexTerms();
        request.nameSet = kind == kHiddenFoldersHint ? SearchEngine::NameSet::HiddenFolders
                                                     : SearchEngine::NameSet::ExcludedFiles;
        m_walkRunning = true;
        m_walkSeen = 0;
    } else if (kind == kExcludedFoldersHint) {
        const QString text = searchText();
        const pathtext::TypedPath typed = regexTerms() ? pathtext::TypedPath() : pathtext::splitTyped(text);
        request.text = !typed.folder.isEmpty() ? typed.name.trimmed() : nameText();
        request.scope = m_scope == Folders ? ws::Scope::Folders : ws::Scope::Files;
        request.extensions = fileExtensions();
        request.typeFolders = fileTypeFolders();
        request.conditions = quickConditions();
        if (m_panel)
            request.filter = panelFilter();
        request.limit = kPage;
        for (const ExcludedFolder* entry : applicableExcludedFolders())
            request.walkFolders.append(entry->path);
        request.regex = regexTerms();
        m_walkRunning = true;
        m_walkSeen = 0;
    } else if (kind.startsWith(kDriveHint) || kind.startsWith(kFolderHint)) {
        // A folder typed as a path: the words after it are what is looked for
        // (typed in a folder's window, on a drive, or in a panel folder filter: nameText).
        const pathtext::TypedPath typed = pathtext::splitTyped(searchText());
        request.text = kind.startsWith(kFolderHint) && !typed.folder.isEmpty() ? typed.name.trimmed() : nameText();
        request.scope = m_scope == Folders ? ws::Scope::Folders : ws::Scope::Files; // 全部: its files and folders
        request.extensions = fileExtensions();
        request.typeFolders = fileTypeFolders();
        request.conditions = quickConditions();
        if (m_panel)
            request.filter = panelFilter();
        request.limit = kPage;
        request.walkFolder = walkedPath(kind);
        request.regex = regexTerms();
        request.fuzzyMode = fuzzyModeFromName(m_fuzzyMode);
        m_walkRunning = true;
        m_walkSeen = 0;
    } else {
        request.text = m_scope == Content ? searchText() : contentNeedle();
        request.scope = ws::Scope::Content;
        request.regex = m_regexMode;
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
    else if (kind == kHiddenFoldersHint)
        m_nameLeftOut.hiddenFolders = 0;
    else if (kind == kExcludedFilesHint)
        m_nameLeftOut.excludedFiles = 0;
    QString said;
    if (stopped)
        said = hits > 0 ? tr("已停止，找到 %Ln 条", nullptr, hits) : tr("已停止");
    else if (kind == kLargeHint)
        said = hits > 0 ? tr("大文件里找到 %Ln 条", nullptr, hits) : tr("这些大文件里也没有");
    else if (kind == kSystemHint)
        said = hits > 0 ? tr("系统和程序文件夹里找到 %Ln 条", nullptr, hits) : tr("系统和程序文件夹里也没有");
    else if (kind == kTypesHint)
        said = hits > 0 ? tr("这些类型的文件里找到 %Ln 条", nullptr, hits) : tr("这些类型的文件里也没有");
    else if (kind == kHiddenFoldersHint)
        said = hits > 0 ? tr("排除目录里显示了 %Ln 项", nullptr, hits) : tr("排除目录里也没有");
    else if (kind == kExcludedFilesHint)
        said = hits > 0 ? tr("已跳过的文件里显示了 %Ln 项", nullptr, hits) : tr("已跳过的文件里也没有");
    else if (kind == kExcludedFoldersHint)
        said = hits > 0 ? tr("排除目录里找到 %Ln 项", nullptr, hits) : tr("排除目录里也没有");
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
    case Apps:
        return tr("搜索应用");
    case Files:
        if (m_fileType >= 0) {
            QStringList names;
            for (int type : selectedFileTypes())
                names.append(m_fileTypes[type].name);
            return tr("搜索%1", "a kind of file, as 图片").arg(names.join(tr("、")));
        }
        return tr("搜索文件和文件夹");
    case Folders:
        return tr("搜索文件夹");
    case Content:
        if (!m_contentTypes.isEmpty()) { // GitHub #447: the kinds chosen, 图片 or 文档
            QStringList names;
            for (int type : m_contentTypes)
                names.append(m_fileTypes[type].name);
            return m_contentFolder.isEmpty()
                ? tr("搜索%1中的文字", "kinds of file, as 图片").arg(names.join(tr("、")))
                : tr("在 %1 里搜索%2中的文字", "a folder, then kinds of file").arg(folderLabel(m_contentFolder), names.join(tr("、")));
        }
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
    m_contentPane->setTextExtensions(extensions); // its 预览 shows them as text, as the content search reads them
    m_contentSizeLimits = sizeLimits;
    if (!enabled && m_scope == Content) {
        setScope(All);
        return;
    }
    emit scopeChanged(); // placeholder text mentions the extensions
    if (enabled != wasEnabled && m_scope == All) {
        if (m_window && m_window->isVisible())
            search(); // 全部: content matches below the names come or go
        else
            m_shownStale = true;
    }
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
    if (m_scope == All && !searchText().isEmpty()) {
        if (m_window && m_window->isVisible())
            search();
        else
            m_shownStale = true;
    }
}

void Launcher::openWebAddress(const QString& url) const
{
    if (m_webBrowser.isEmpty())
        shell::openUrl(url);
    else
        shell::openUrlWith(m_webBrowser, url);
}

void Launcher::setWebFallback(bool on)
{
    if (on == m_webFallback)
        return;
    m_webFallback = on;
    if (m_scope == All && !searchText().isEmpty()) {
        if (m_window && m_window->isVisible())
            search();
        else
            m_shownStale = true;
    }
}

// Nothing found for words typed in 全部: the row that searches the web for
// them, with the first site of the user's list that searches (the one the
// OCR's 网页搜索 uses too). Not for what is not plain words to look up: a
// folder typed in or a path, a filter, a regular expression, a mistake in
// the query, a command's or a site's keyword (those have their own rows).
std::optional<SearchResult> Launcher::webFallbackRow() const
{
    const QString words = m_query.trimmed();
    if (!m_webFallback || m_scope != All || m_panel || m_folderSearch || m_keyword || m_filePrefix || m_regexMode
        || words.isEmpty() || !m_queryError.isEmpty() || !m_typingPrefix.isEmpty() || !typedQuick().filters.isEmpty()
        || typedKeyword(m_web, m_query) || words.contains(QDir::separator()) || words.contains(u'/'))
        return std::nullopt;
    const qsizetype first = defaultWebSearch(m_web);
    if (first < 0)
        return std::nullopt;
    const WebShortcut& web = m_web[first];
    SearchResult r; // as SearchEngine's fromWeb makes a search's row
    r.name = web.name;
    r.path = webPath(web.keyword, words);
    r.app = AppKind::Web;
    r.target = web.searchUrl(words);
    r.keyword = web.keyword;
    r.words = words;
    r.icon = web.iconRef();
    return r;
}

// A web address typed in 全部 (GitHub #171): the row that opens it, with the
// web search plugin on. Listed with what else the words find, never in
// place of it: which one Enter means is the user's to see and choose.
std::optional<SearchResult> Launcher::webAddressRow() const
{
    if (m_web.isEmpty() || m_scope != All || m_panel || m_folderSearch || m_keyword || m_filePrefix || m_regexMode
        || !m_queryError.isEmpty() || !m_typingPrefix.isEmpty() || !typedQuick().filters.isEmpty() || typedKeyword(m_web, m_query))
        return std::nullopt;
    const QString words = m_query.trimmed();
    const std::optional<QString> address = typedWebAddress(words);
    if (!address)
        return std::nullopt;
    SearchResult r;
    r.name = words; // as typed: the row's second line says what opens
    r.path = webAddressPath(*address);
    r.app = AppKind::Web;
    r.target = *address;
    r.icon = *address; // its site's own
    r.exact = true;
    r.score = 1000; // as a typed keyword's row (SearchEngine's kTypedKeywordScore)
    return r;
}

void Launcher::setWebSuggestions(bool on)
{
    if (on == m_webSuggestions)
        return;
    m_webSuggestions = on;
    if (!on) {
        m_suggester.cancel();
        dropSuggestions();
    } else if (m_window && m_window->isVisible() && m_scope == All && !searchText().isEmpty()) {
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
            if (!m_keyword && m_scope != Content) {
                if (m_window && m_window->isVisible())
                    searchFor("the favorites changed"); // the rows refreshed in place: 收藏 comes or goes
                else
                    m_shownStale = true;
            }
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
    m_switchWaiting.clear();
    m_stateFile = stateFile;
    if (!stateFile.isEmpty()) {
        const QString view = QSettings(stateFile, QSettings::IniFormat).value(u"Search/AppsView"_s).toString();
        m_appsView = view == u"favorites" ? FavoriteApps
            : view == u"all"          ? AllApps
            : view == u"running"      ? RunningApps
                                      : FrequentApps;
        emit appsViewChanged();
    }
    m_contentPane->loadPreferences(stateFile);
    m_switchPageTold = !stateFile.isEmpty() && QSettings(stateFile, QSettings::IniFormat).value(u"Hints/SwitchPage"_s).toBool();
    m_previewWanted = !stateFile.isEmpty() && QSettings(stateFile, QSettings::IniFormat).value(u"Launcher/PreviewOpen"_s).toBool();
    m_results.setSwitches(switches);
    refreshKeyHints();
    scheduleAwakeTick();
    if (m_window && m_window->isVisible())
        search(); // remove a keep-awake status row when its provider is off
    if (!switches)
        return;
    // What Enter does on the row the keyboard is on follows its switch, whoever changed it.
    connect(switches, &Switches::changed, this, [this](const QString& id) {
        const SearchResult* r = m_results.at(m_currentRow);
        if (r && r->switchId == id)
            refreshKeyHints();
        if (id == u"keep-awake" && m_window && m_window->isVisible()) {
            scheduleAwakeTick(); // turned on or off, or given another end time
            if (m_scope == All && m_query.trimmed().isEmpty()) {
                bool pinned = false;
                for (int row = 0; row < m_results.count(); ++row) {
                    const SearchResult* item = m_results.at(row);
                    if (item->statusRow && item->switchId == id) {
                        pinned = true;
                        break;
                    }
                }
                const SwitchState awake = m_switches->state(id);
                const bool wanted = awake.ready() && awake.on == SwitchState::Value::On;
                if (m_pending || pinned != wanted)
                    searchFor("keep-awake changed"); // async startup, expiry or a provider crash changed the status row
            }
        }
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

void Launcher::setResultOptions(KindOrder kindOrder, bool showModified, bool showFileSize, bool thumbnails, const QString& numberKeys)
{
    m_results.setThumbnails(thumbnails);
    const QString keys = numberKeys == u"off" ? QString() : numberKeys;
    if (kindOrder == m_kindOrder && showModified == m_showModified && showFileSize == m_showFileSize && keys == m_numberKeys)
        return;
    const bool reorder = kindOrder != m_kindOrder;
    m_kindOrder = kindOrder;
    m_showModified = showModified;
    m_showFileSize = showFileSize;
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

void Launcher::setDefaultSystemMenu(bool on)
{
    if (on == m_defaultSystemMenu)
        return;
    m_defaultSystemMenu = on;
    emit resultOptionsChanged();
}

void Launcher::setClickPreviews(bool on)
{
    if (on == m_clickPreviews)
        return;
    m_clickPreviews = on;
    emit resultOptionsChanged();
}

void Launcher::setTabEntersFavoriteFolder(bool on)
{
    if (on == m_tabEntersFavoriteFolder)
        return;
    m_tabEntersFavoriteFolder = on;
    refreshKeyHints();
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

void Launcher::setExcludedFiles(const QStringList& types, const QStringList& names)
{
    if (types == m_excludedTypes && names == m_excludedFileNames)
        return;
    m_excludedTypes = types;
    m_excludedFileNames = names;
    // Files come or go in whatever is listed now: asked again, in place.
    if (m_window && m_window->isVisible())
        search();
    else
        m_shownStale = true;
}

void Launcher::setLetterJump(bool on, const QString& key, const QString& keys)
{
    if (m_letterJump == on && m_letterJumpKey == key && m_letterJumpKeys == keys)
        return;
    m_letterJump = on;
    m_letterJumpKey = key;
    m_letterJumpKeys = keys;
    emit resultOptionsChanged();
}

void Launcher::setNumberHints(const QString& hints)
{
    if (m_numberHints == hints)
        return;
    m_numberHints = hints;
    emit resultOptionsChanged();
}

void Launcher::setRowDeleteButton(bool shown)
{
    if (m_rowDeleteButton == shown)
        return;
    m_rowDeleteButton = shown;
    emit resultOptionsChanged();
}

void Launcher::setKeyboardHints(bool help, bool footer)
{
    if (m_shortcutHelp == help && m_footerHints == footer)
        return;
    m_shortcutHelp = help;
    m_footerHints = footer;
    emit resultOptionsChanged();
}

void Launcher::setMaxRows(int rows)
{
    rows = std::clamp(rows, 4, 20);
    if (m_maxRows == rows)
        return;
    m_maxRows = rows;
    emit resultOptionsChanged();
}

void Launcher::setScopeButton(bool on)
{
    if (m_scopeButton == on)
        return;
    m_scopeButton = on;
    emit resultOptionsChanged();
}

void Launcher::setPureSearch(bool on)
{
    if (m_pureSearch == on)
        return;
    m_pureSearch = on;
    emit resultOptionsChanged();
    if (m_query.trimmed().isEmpty()) {
        if (m_window && m_window->isVisible())
            search();
        else
            m_shownStale = true;
    }
}

void Launcher::setPanelListsAll(bool on)
{
    if (m_panelListsAll == on)
        return;
    m_panelListsAll = on;
    if (m_panel && m_query.trimmed().isEmpty()) {
        if (m_window && m_window->isVisible())
            search();
        else
            m_shownStale = true;
    }
}

void Launcher::setPreviewHotkey(const QString& shortcut)
{
    const QString value = shortcut.isEmpty() ? QString() : previewkeys::normalized(shortcut);
    if ((!shortcut.isEmpty() && value.isEmpty()) || m_previewHotkey == value)
        return;
    m_previewHotkey = value;
    m_previewSequence = QKeySequence::fromString(value, QKeySequence::PortableText);
    emit resultOptionsChanged();
}

bool Launcher::isPreviewKey(int key, int modifiers) const
{
    return previewkeys::matches(m_previewSequence, key, modifiers);
}

void Launcher::setOpenHotkey(const QString& shortcut)
{
    const QString value = previewkeys::normalizedOpen(shortcut);
    if (m_openHotkey == value)
        return;
    m_openHotkey = value;
    m_openSequence = QKeySequence::fromString(value, QKeySequence::PortableText);
    emit resultOptionsChanged();
}

bool Launcher::isOpenKey(int key, int modifiers) const
{
    return previewkeys::matches(m_openSequence, key, modifiers);
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
    // Chosen: in force from now on, a quick filter's own set aside (GitHub #392).
    const Sort before = this->sort();
    const bool chosen = sort != m_sort;
    m_filterSort.reset();
    m_sort = sort;
    if (chosen || sort != before)
        emit sortChanged();
    if (sort == before)
        return;
    // A quick filter named alone (":nobin") lists what it lets through, in this order too.
    if (!m_window || !m_window->isVisible() || (searchText().isEmpty() && typedQuick().filters.isEmpty()) || m_keyword)
        return; // hidden: handleShown() searches
    // 内容, every match on screen: put in the other order, not searched again
    // (that would read the files once more).
    if (m_scope == Content && m_shownScope == Content && m_haveResults && !m_replacePending && m_scanComplete
        && !m_contentRunning && m_contentScanned >= m_contentTotal && m_shownText == searchText()) {
        SearchResults rows;
        rows.reserve(m_results.count());
        for (int row = 0; row < m_results.count(); ++row)
            rows.append(*m_results.at(row));
        const RankBy by = rankBy();
        std::ranges::stable_sort(rows, [by](const SearchResult& a, const SearchResult& b) { return rankedBefore(a, b, by); });
        std::ranges::stable_partition(rows, [](const SearchResult& r) { return !r.isPlugin(); }); // the providers' last
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

void Launcher::setFavoriteTiles(bool on)
{
    if (m_favoriteTilesOn == on)
        return;
    m_favoriteTilesOn = on;
    if (searchText().isEmpty()) {
        if (m_window && m_window->isVisible())
            search();
        else
            m_shownStale = true;
    }
}

void Launcher::moveFavoriteTile(int from, int to)
{
    const SearchResult* a = m_tiles.at(from);
    const SearchResult* b = m_tiles.at(to);
    if (!m_favorites || !a || !b)
        return;
    const qsizetype at = m_favorites->indexOf(a->path);
    const qsizetype there = m_favorites->indexOf(b->path);
    if (at >= 0 && there >= 0 && at != there)
        m_favorites->move(at, there); // saved; the tiles come again in the new order (setFavorites)
}

void Launcher::setStartOptions(int recentItems, bool favorites)
{
    if (m_recentItems == recentItems && m_favoritesOnStart == favorites)
        return;
    m_recentItems = recentItems;
    m_favoritesOnStart = favorites;
    if (searchText().isEmpty()) {
        if (m_window && m_window->isVisible())
            search();
        else
            m_shownStale = true;
    }
}

RankBy Launcher::rankBy() const
{
    if (m_panel && m_panelSort.key == SortKey::Modified)
        return m_panelSort.descending ? RankBy::Newest : RankBy::Oldest;
    // The panel with nothing typed: ranked means nothing, the newest first.
    if (m_panel && m_panelSort.key == SortKey::Rank && searchText().isEmpty())
        return RankBy::Newest;
    // The sorts by a column are the names' (nameOrder); content matches stay as ranked.
    return sort() <= OldestFirst ? static_cast<RankBy>(sort()) : RankBy::Match;
}

SortOrder Launcher::nameOrder() const
{
    if (m_panel)
        return m_panelSort; // its columns; the sorts below are the compact list's
    // By time: ranked so (rankBy), the names in no other order.
    return sort() == NewestFirst || sort() == OldestFirst ? SortOrder {} : columnOrder(sort());
}

bool Launcher::showsStartList() const
{
    return m_scope != Content && m_scope != Apps && searchText().isEmpty() && !m_panel && !m_filePrefix
        && typedQuick().filters.isEmpty(); // a quick filter named alone lists its own (GitHub #392)
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
        setSort(sort() <= OldestFirst ? static_cast<Sort>((sort() + 1) % 3) : BestMatch);
}

void Launcher::setInTaskbarBox(bool on)
{
    if (on == m_inTaskbarBox)
        return;
    m_inTaskbarBox = on;
    if (!m_window || !m_window->isVisible())
        m_shownStale = true;
    emit inTaskbarBoxChanged();
}

void Launcher::setKeysRouted(bool routed)
{
    if (routed == m_keysRouted)
        return;
    m_keysRouted = routed;
    emit keysRoutedChanged();
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
    m_filePrefix.reset();
    m_prefixWords.clear();
    setTypingFolder({});
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
        m_quickFiltersOn.clear();
        emit filtersChanged();
    }
    if (m_scope != All) {
        m_scope = All;
        emit scopeChanged();
    }
    if (!m_query.isEmpty()) {
        m_query.clear();
        updateQueryPrefix();
        emit queryChanged();
    }
    m_folderSteps.clear(); // no folder to step back out of (GitHub #361)
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints();
}

void Launcher::handleShown(bool deferBackgroundWork)
{
    m_queryHistory.resetNavigation();
    m_index->contentIndexer()->setSearchBoxOpen(true); // the files it searches are read before the indexer's
    m_statusPoll.start();
    scheduleAwakeTick();
    m_previewResume = m_previewWanted;
    // Up again, it starts on its first row, as ever: the row a step out of a
    // folder came back to, or one chosen before, is no longer meant (GitHub #361).
    m_chosenPath.clear();
    m_results.setStartPath({});
    // Unless it is to come back where it was left (Settings::reopenWhereLeft):
    // the same words still there (not cleared on opening), on the row left
    // on, the pages scrolled through listed again.
    if (m_reopenWhereLeft && !m_leftPath.isEmpty() && m_query == m_leftQuery) {
        m_chosenPath = m_leftPath;
        m_results.setStartPath(m_leftPath);
        m_pages = m_leftPages;
    }
    m_tabHeld = false;
    m_deferredCatalogRefresh = deferBackgroundWork;
    if (!deferBackgroundWork) {
        m_apps->refresh(); // reads the list again only if apps were installed or removed
        m_places->refresh(); // ... if Windows was updated or its language changed
        if (m_switches)
            m_switches->refresh(); // the switches' rows show what they are now
    }
    // Reopening to the exact list already on screen: reveal immediately and
    // refresh quietly in place once visible (finishShownRefresh), instead of
    // holding the window cloaked behind a cold FileIndex scan or disk stat.
    const bool recentNeedsFresh = searchText().isEmpty() && m_startList == RecentlyOpened && m_historyDirty;
    const bool deferSearch = deferBackgroundWork && !m_typingAwaited && !m_shownStale && !recentNeedsFresh
        && m_scope != Apps && matchesShownRows();
    m_deferredSearchRefresh = deferSearch;
    if (!deferSearch)
        search(); // new query, changed settings, or updated recent history
    emit historyChanged(); // things were opened since
    refreshFrequentApps(!deferBackgroundWork);
    emit shown();
}

void Launcher::finishShownRefresh()
{
    if (!m_window || !m_window->isVisible()) {
        m_deferredCatalogRefresh = false;
        m_deferredSearchRefresh = false;
        return;
    }
    if (std::exchange(m_deferredCatalogRefresh, false)) {
        m_apps->refresh();
        m_places->refresh();
        if (m_switches)
            m_switches->refresh();
        refreshFrequentApps(true);
    }
    if (std::exchange(m_deferredSearchRefresh, false)) {
        m_searchWhy = "deferred open refresh";
        search();
    }
}

void Launcher::refreshFrequentApps(bool refreshWindows)
{
    constexpr std::size_t kFrequentApps = 6; // a row over the taskbar
    std::vector<FrequentApp> picked;
    const std::shared_ptr<const AppList> apps = m_apps->apps();
    if (m_recordHistory && apps) {
        if (refreshWindows)
            refreshWindowsUsesAsync();
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

void Launcher::refreshWindowsUsesAsync()
{
    if (m_windowsUsesLoading.load(std::memory_order_acquire))
        return;
    if (m_windowsUsesAge.isValid() && !m_windowsUsesAge.hasExpired(60'000))
        return;
    m_windowsUsesLoading.store(true, std::memory_order_release);
    QPointer<Launcher> self(this);
    m_windowsUsesWorker = std::jthread([self](std::stop_token stop) {
        std::vector<AppUse> uses = windowsAppUses(); // some 500+ registry values: keep off the GUI thread
        if (stop.stop_requested() || !self)
            return;
        QMetaObject::invokeMethod(self, [self, uses = std::move(uses)]() mutable {
            if (!self)
                return;
            self->m_windowsUsesLoading.store(false, std::memory_order_release);
            self->m_windowsUses = std::move(uses);
            self->m_windowsUsesAge.start();
            self->refreshFrequentApps(false);
            if (self->m_scope == Apps && self->m_window && self->m_window->isVisible())
                self->showApps();
        }, Qt::QueuedConnection);
    });
}

void Launcher::setAppsTab(bool wanted)
{
    if (m_appsTabWanted == wanted)
        return;
    m_appsTabWanted = wanted;
    m_appsLeft = false;
    appsTabMoved();
}

// The tab came or went (the setting, the panel, a file dialog's bar): 程序
// cannot stay without it, and is come back to when it went only for a while.
void Launcher::appsTabMoved()
{
    if (m_scope == Apps && !appsTab()) {
        m_appsLeft = m_appsTabWanted;
        m_scope = All;
        m_results.setSelection({});
        m_held.reset();
        refreshKeyHints();
        emit scopeChanged();
        if (m_window && m_window->isVisible())
            search();
        return;
    }
    if (m_scope == All && m_appsLeft && appsTab()) {
        m_appsLeft = false;
        m_scope = Apps;
        m_results.setSelection({});
        m_held.reset();
        refreshKeyHints();
        emit scopeChanged();
        if (m_window && m_window->isVisible())
            search();
        return;
    }
    emit scopeChanged(); // appsTab
}

void Launcher::setAppsView(AppsView view)
{
    if (view != FrequentApps && view != FavoriteApps && view != AllApps && view != RunningApps)
        return;
    if (m_appsView == view)
        return;
    m_appsView = view;
    if (!m_stateFile.isEmpty()) {
        QSettings(m_stateFile, QSettings::IniFormat)
            .setValue(u"Search/AppsView"_s,
                view == FavoriteApps ? u"favorites"_s
                : view == AllApps   ? u"all"_s
                : view == RunningApps ? u"running"_s
                                    : u"frequent"_s);
    }
    emit appsViewChanged();
    if (m_scope == Apps) {
        m_results.setSelection({});
        m_held.reset();
        search();
    }
}

void Launcher::cycleAppsView(int delta)
{
    static constexpr AppsView kOrder[] {RunningApps, FrequentApps, FavoriteApps, AllApps}; // as the strip shows them
    const int at = static_cast<int>(std::find(std::begin(kOrder), std::end(kOrder), m_appsView) - std::begin(kOrder));
    const int size = static_cast<int>(std::size(kOrder));
    setAppsView(kOrder[((at + delta) % size + size) % size]);
}

namespace {

// What tells one list of windows from another: which they are, and their titles.
QStringList windowKeys(const SearchResults& windows)
{
    QStringList keys;
    keys.reserve(windows.size());
    for (const SearchResult& w : windows)
        keys.append(w.path + u'\n' + w.name);
    return keys;
}

} // namespace

// search() in 程序: the apps the strip chose, as rows; narrowed by what is
// typed, matched and ranked as 全部 does apps (names, pinyin, other names,
// abbreviations). Nothing else is asked: no file, no plugin, no site.
void Launcher::showApps()
{
    constexpr std::size_t kFrequentShown = 60; // some six rows of the grid
    m_engine->cancel();
    m_requestId = 0;
    m_pending = false;
    m_contentRunning = false;
    m_pluginPending.clear();
    m_pluginWait.stop();
    m_pluginSlow = false;
    m_pluginProgress.clear();
    ++m_pluginQueryId; // what an earlier search asked for is moot now
    if (m_plugins)
        m_plugins->cancelAll();
    if (m_merge)
        m_merge->begin(m_pluginQueryId, searchText(), {}, {}, false, false);
    if (m_keyword) {
        m_keyword.reset();
        emit pluginChanged();
    }
    m_suggestRows.clear();
    m_withContent = false;
    m_unconfirmed.clear();
    m_quiet = m_refresh;
    m_scanComplete = true;

    SearchResults rows;
    QStringList highlights;
    QStringList fuzzyHighlights;
    QList<int> counts {0, 0, 0, 0};
    const QString text = searchText();
    const SearchResults running = windowsearch::snapshot(windowsearch::Own::Included); // 运行: the windows open on this desktop
    counts[RunningApps] = static_cast<int>(running.size());
    if (m_appsView == RunningApps)
        rows = text.trimmed().isEmpty() ? running : windowsearch::matching(running, text, regexTerms());
    m_runningShown = windowKeys(running);
    if (const std::shared_ptr<const AppList> apps = m_apps->apps()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        std::vector<std::size_t> frequent;
        if (m_recordHistory) {
            refreshWindowsUsesAsync();
            frequent = ws::frequentApps(*apps, *m_history, m_windowsUses, now, kFrequentShown);
        }
        std::vector<std::size_t> favorites; // in the favorites' own order: each keeps its place
        if (m_favorites) {
            QHash<QString, std::size_t> byPath;
            byPath.reserve(static_cast<qsizetype>(apps->size()));
            for (std::size_t i = 0; i < apps->size(); ++i)
                byPath.insert((*apps)[i].launchPath(), i);
            for (const Favorite& f : m_favorites->items()) {
                if (f.kind != Favorite::Kind::App)
                    continue;
                if (const auto it = byPath.constFind(f.path); it != byPath.cend())
                    favorites.push_back(*it);
            }
        }
        std::vector<std::size_t> all; // uninstallers and manuals aside, by name
        all.reserve(apps->size());
        for (std::size_t i = 0; i < apps->size(); ++i) {
            if (!(*apps)[i].auxiliary)
                all.push_back(i);
        }
        QCollator byName;
        byName.setCaseSensitivity(Qt::CaseInsensitive);
        byName.setNumericMode(true);
        std::stable_sort(all.begin(), all.end(),
            [&](std::size_t a, std::size_t b) { return byName.compare((*apps)[a].name, (*apps)[b].name) < 0; });
        counts[FrequentApps] = static_cast<int>(frequent.size());
        counts[FavoriteApps] = static_cast<int>(favorites.size());
        counts[AllApps] = static_cast<int>(all.size());

        if (m_appsView != RunningApps) { // 运行's rows are the windows, made above
            const std::vector<std::size_t>& chosen
                = m_appsView == FavoriteApps ? favorites : m_appsView == AllApps ? all : frequent;
            if (text.trimmed().isEmpty()) {
                rows.reserve(static_cast<qsizetype>(chosen.size()));
                for (const std::size_t i : chosen)
                    rows.push_back(ws::appResult((*apps)[i]));
            } else if (const ParsedQuery query = parseQuery(text, regexTerms()); query.error.isEmpty()) {
                const FuzzyMode appFuzzy = regexTerms() ? FuzzyMode::Off : fuzzyModeFromName(m_fuzzyMode);
                const NameMatcher matcher(query, appFuzzy == FuzzyMode::On);
                const QSet<std::size_t> among(chosen.cbegin(), chosen.cend());
                const QStringList history = m_recordHistory ? m_history->ranked(text, now) : QStringList();
                const History::Affinities affinities
                    = m_recordHistory ? m_history->affinities(text, now) : History::Affinities();
                for (const AppHit& hit : searchApps(*apps, query, matcher, history, affinities, appFuzzy)) {
                    if (!among.contains(hit.index))
                        continue;
                    rows.push_back(ws::appResult((*apps)[hit.index], false, hit.otherName));
                    rows.back().exact = hit.exact;
                    rows.back().fuzzy = hit.fuzzy;
                }
                highlights = query.highlights;
                fuzzyHighlights = query.fuzzyHighlights;
            }
            if (m_favorites) { // the tiles' stars
                for (SearchResult& r : rows) {
                    if (const qsizetype at = m_favorites->indexOf(r.path); at >= 0) {
                        r.favorite = true;
                        r.favoriteKeyword = m_favorites->items()[at].keyword;
                    }
                }
            }
        }
    }
    if (counts != m_appCounts) {
        m_appCounts = counts;
        emit appCountsChanged();
    }
    m_totalMatches = rows.size();
    m_shownAppsView = m_appsView;
    showRows(std::move(rows), std::move(highlights), std::move(fuzzyHighlights));
    refreshStatus();
    // 运行 follows the windows while it is looked at: one opened or closed
    // meanwhile, or named anew, is in the grid within the tick.
    if (m_appsView == RunningApps && m_window && m_window->isVisible())
        m_runningTick.start();
    else
        m_runningTick.stop();
}

// The tick of 运行. The grid is made again only when the windows are not the
// ones shown: rows given again for nothing would take the selection back to
// the first tile under the arrow keys.
void Launcher::runningTicked()
{
    if (m_scope != Apps || m_appsView != RunningApps || !m_window || !m_window->isVisible()) {
        m_runningTick.stop();
        return;
    }
    if (windowKeys(windowsearch::snapshot(windowsearch::Own::Included)) != m_runningShown)
        showApps();
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
    m_columns.retranslate();
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
    setTypingAwaited(false);
    rememberQuery();
    closePreview(); // the wish to have it stays (m_previewWanted) ...
    if (!m_previewRestore)
        setPreviewWanted(false); // ... unless the user has it close with the launcher (GitHub #258)
    dropServerWait();
    m_previewResume = false;
    m_previewHandedOver = false; // back with the launcher as left (m_previewWanted), wherever it opens
    m_index->contentIndexer()->setSearchBoxOpen(false);
    m_systemCommand = false;
    // Where it was left, should it open there again (handleShown).
    m_leftQuery = m_query;
    m_leftPath = m_results.pathAt(m_currentRow);
    m_leftPages = m_pages;
    m_results.setSelection({});
    m_held.reset();
    m_statusPoll.stop();
    m_awakeTick.stop();
    m_contentDebounce.stop();
    m_slowSearch.stop();
    m_searchSlow = false;
    m_replaceClock.invalidate(); // nobody waits for those rows now
    m_searchAgain.stop(); // showing the window searches again
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
    forgetRowChanges();
    // Opened again, a page, as quick as ever; more as it is scrolled again.
    m_pages = 1;
    m_loadingMore = false;
    m_deferredCatalogRefresh = false;
    m_deferredSearchRefresh = false;
}

void Launcher::searchFor(const char* why)
{
    m_searchAgainWhy = why;
    if (m_searchAgain.isActive())
        return; // one on its way takes this too
    constexpr qint64 kEvery = 1000;
    const qint64 since = m_lastSearchAgain.isValid() ? m_lastSearchAgain.elapsed() : kEvery;
    m_searchAgain.start(static_cast<int>(std::max<qint64>(0, kEvery - since)));
}

void Launcher::searchAgainNow()
{
    // What is under way searches the index as it is now: its rows come first
    // (asked again, they would never come while the background keeps changing).
    if (m_pending || m_contentRunning || m_walkRunning) {
        m_searchAgain.start(100);
        return;
    }
    if (!m_window || !m_window->isVisible())
        return; // showing the window searches again anyway
    m_lastSearchAgain.start();
    m_searchWhy = std::exchange(m_searchAgainWhy, nullptr);
    search();
}

bool Launcher::matchesShownRows() const
{
    if (!m_haveResults || m_replacePending)
        return false;
    const QString text = searchText();
    const std::optional<KeywordMode> keyword = m_filePrefix || m_scope == Apps ? std::nullopt : matchKeyword(m_query);
    const QString keywordKey = keyword ? keyword->pluginId + u'/' + keyword->commandId : QString();
    return text == m_shownText && m_scope == m_shownScope
        && fileExtensions() == m_shownExtensions && fileTypeFolders() == m_shownTypeFolders
        && rankBy() == m_shownRankBy && nameOrder() == m_shownOrder && (m_panel || sort() == m_shownSort)
        && m_kindOrder == m_shownKindOrder
        && m_preferredTypes == m_shownPreferredTypes && keywordKey == m_shownKeyword
        && (m_scope != Content || (m_contentFolder == m_shownContentFolder && pickKey(m_contentPick) == m_shownPick
                                   && contentTypesKey() == m_shownContentTypes))
        && (!text.isEmpty() || m_startList == m_shownStartList) && panelKey() == m_shownPanelKey
        && typingKey() == m_shownTyping && regexTerms() == m_shownRegex && m_fuzzyMode == m_shownFuzzyMode
        && (m_scope != Apps || m_appsView == m_shownAppsView);
}

// Nothing is cleared up front: the rows on screen stay until results replace
// them. Searching what is already shown again (the window reopens, the index
// changes) is a refresh: the rows are updated in place, and content matches
// from the last scan stay until the new scan has had a chance to find them.
void Launcher::search(bool more)
{
    m_deferredSearchRefresh = false;
    const char* why = std::exchange(m_searchWhy, nullptr);
    const bool wasReplacing = m_replacePending && m_replaceClock.isValid();
    m_flash.clear();
    m_contentDebounce.stop();
    m_slowSearch.stop();
    m_searchSlow = false;
    const QString text = searchText();
    // Sorted another way, or the other start list, the rows are new ones
    // (the first is selected). So is a plugin command's list in place of
    // the names' (or back): "=" and "= " are the same text trimmed.
    std::optional<KeywordMode> keyword = m_filePrefix || m_scope == Apps ? std::nullopt : matchKeyword(m_query);
    const QString error = keyword || typedKeyword(m_web, m_query) ? QString()
        : m_scope == Content ? contentPattern(splitContentQuery(text, m_regexMode).words, m_regexMode).error
        : parseQuery(nameText(), regexTerms()).error;
    if (m_queryError != error) {
        m_queryError = error;
        emit queryErrorChanged();
    }
    // The folder typed in, first or alone (typingKey), is a list of its own too.
    m_refresh = matchesShownRows();
    m_replacePending = !m_refresh;
    if (m_replacePending) {
        if (!wasReplacing) { // the rows on screen just became another query's
            m_replaceClock.start();
            m_replaceAsks = 0;
            m_replaceWhy.clear();
        } else { // asked again before the new rows came
            ++m_replaceAsks;
            const QString reason = why ? QString::fromLatin1(why) : u"typed on, or another choice"_s;
            if (!m_replaceWhy.contains(reason))
                m_replaceWhy.append(reason);
        }
    }
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
        m_nameLeftOut = {};
        m_webFallbackOnly = false;
        m_drives.clear();
        m_hintsTaken.clear();
        m_leftOutFound.clear();
        m_approximate = false;
        m_approximateFuzzy = false;
    }
    if (m_scope == Apps) {
        showApps();
        return;
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
        if (suggestsPaths())
            searchSuggestedPaths();
        refreshStatus();
        if (isShotHistory(m_keyword->pluginId, m_keyword->commandId)) // the screenshots: in memory, at once
            showPluginRows(screenshotRows(m_keyword->words));
        return;
    }

    // The words typed in a file manager are on their way (awaitTyping).
    if (m_typingAwaited && m_query.trimmed() != m_awaitedText)
        setTypingAwaited(false);
    const bool pureEmpty = (m_pureSearch && !m_panel && !m_folderSearch && !m_inTaskbarBox && m_query.trimmed().isEmpty())
        || (m_typingAwaited && m_scope != Content)
        // The panel as set to list nothing of itself (GitHub #321): a filter
        // chosen, or a kind of file, is something asked for, and lists.
        || (m_panel && !m_panelListsAll && m_scope != Content && m_query.trimmed().isEmpty() && !filtersActive()
            && !(m_scope == Files && m_fileType >= 0));
    if (m_scope == Content || pureEmpty) {
        m_engine->cancel();
        m_requestId = 0;
        m_pending = false;
        m_contentRunning = false;
        if (text.isEmpty() || pureEmpty) {
            m_replacePending = false;
            m_results.clear();
            m_haveResults = false;
            m_shownStale = false;
            m_historyDirty = false;
            m_chosenPath.clear(); // nothing listed, nothing chosen (GitHub #361)
            emit resultsReplaced();
        } else if (m_refresh) {
            // Asked again, the rows not found again go (not those a one-off
            // brought: this scan does not read those files, nor the content
            // providers': their new answer rewrites them, plugin::Merge); for
            // a page more, what is listed stays (the new scan finds those first).
            if (!m_loadingMore) {
                for (int row = 0; row < m_results.count(); ++row) {
                    const SearchResult* r = m_results.at(row);
                    if (!r->isPlugin() && !m_leftOutFound.contains(r->path))
                        m_unconfirmed.insert(r->path);
                }
            }
            // The rows are this query's already: the providers' answers go in
            // as they come (showRows, which says so otherwise, is not called).
            if (m_merge && !m_merge->keywordMode()) {
                m_merge->namesShown();
                if (m_merge->takeBetterFirstRow())
                    emit betterFirstRow();
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
    request.text = nameText();
    if (const auto files = commandFiles()) {
        request.commandPath = files->command.path;
        request.commandText = searchText();
    }
    // Typed in a folder's window (GitHub #110): its words, there first (or
    // only there). The folder alone is listed as the path typed it is.
    if (!typingWords().isEmpty()) {
        request.currentFolder = m_typingFolder;
        request.currentFolderOnly = m_typingOnly;
        request.currentFolderLevel = m_typingOnly && m_typingLevel;
    }
    request.scope = static_cast<ws::Scope>(m_scope);
    request.extensions = fileExtensions();
    request.typeFolders = fileTypeFolders();
    request.kindOrder = m_kindOrder;
    request.rankBy = rankBy();
    request.preferredTypes = m_preferredTypes;
    request.excludedTypes = m_excludedTypes;
    request.excludedFileNames = m_excludedFileNames;
    request.startList = m_startList == RecentlyModified ? ws::StartList::Modified : ws::StartList::Opened;
    request.recentItems = m_recentItems;
    request.favoritesOnStart = m_favoritesOnStart;
    request.limit = kPage * m_pages;
    // A quick filter named alone (":nobin") lists what it lets through, as a kind of file does.
    request.browse = m_filePrefix.has_value() || (m_scope == Files && m_fileType >= 0) || !typedQuick().filters.isEmpty();
    request.conditions = quickConditions();
    if (m_panel) { // its filters and order; nothing typed lists everything
        request.filter = panelFilter();
        if (!m_folderFilter.isEmpty())
            request.folders = {m_folderFilter};
        request.browse = true;
    }
    if (m_panel || m_filePrefix || !text.isEmpty() || !typedQuick().filters.isEmpty())
        request.order = nameOrder();
    // With no start list, nothing opened shows before something is typed.
    if (m_recordHistory && !(text.isEmpty() && m_startList == NoStartList)) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        request.history = m_history->ranked(text, now);
        request.historyAffinity = byOpenCount() ? m_history->counts() : m_history->affinities(text, now);
    }
    request.web = m_web;
    request.apps = m_searchApps;
    if (m_searchApps && !m_folderSearch && !typedKeyword(m_web, m_query)) {
        request.openWindows = [query = request.text, regex = regexTerms()] {
            return windowsearch::matching(windowsearch::snapshot(), query, regex);
        };
    }
    request.places = m_searchPlaces;
    request.ownPlaces = true; // carried out here (performPlace)
    request.regex = regexTerms();
    request.fuzzyMode = fuzzyModeFromName(m_fuzzyMode);
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
    m_lastAsk.start();
    m_pending = true;
    m_contentRunning = false;
    if (m_replacePending)
        m_slowSearch.start();
    refreshStatus();
}

// Rows for the current query and scope. Replacing another query's rows
// moves the selection back to the first one; a refresh keeps it.
bool Launcher::byOpenCount() const
{
    return sort() == MostOpened && !m_panel && !m_keyword && !searchText().isEmpty();
}

void Launcher::showRows(SearchResults rows, QStringList highlights, QStringList fuzzyHighlights)
{
    // 打开最多: the rows opened most often first, each by its own count; a
    // status row and a keyword typed (a site's) stay first, the rest
    // (never opened) as best matched.
    if (byOpenCount() && m_recordHistory) {
        const auto count = [&](const SearchResult& r) {
            // 1000: a typed keyword's row (SearchEngine's kTypedKeywordScore)
            return r.statusRow || r.score >= 1000 ? std::numeric_limits<int>::max() : m_history->use(r.path).count;
        };
        std::vector<int> counts;
        counts.reserve(rows.size());
        for (const SearchResult& r : rows)
            counts.push_back(count(r));
        std::vector<qsizetype> order(rows.size());
        std::iota(order.begin(), order.end(), 0);
        std::ranges::stable_sort(order, [&](qsizetype a, qsizetype b) { return counts[a] > counts[b]; });
        SearchResults sorted;
        sorted.reserve(rows.size());
        for (const qsizetype i : order)
            sorted.append(std::move(rows[i]));
        rows = std::move(sorted);
    }
    m_webFallbackOnly = false;
    if (rows.isEmpty()) {
        if (std::optional<SearchResult> web = webFallbackRow()) {
            rows.append(std::move(*web)); // Enter looks the words up instead of 没有找到 (GitHub #320)
            m_webFallbackOnly = true;
        }
    }
    if (std::optional<SearchResult> address = webAddressRow()) {
        // First, but after a file or an app of that very name ("baidu.txt"
        // there on the disk is what Enter opens; the address is the next row).
        const bool named = !rows.isEmpty() && !rows.first().isWeb()
            && rows.first().name.compare(address->name, Qt::CaseInsensitive) == 0;
        rows.removeIf([&](const SearchResult& r) { return r.path == address->path; });
        rows.insert(named ? 1 : 0, std::move(*address));
    }
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
    if (m_rememberAdmin && m_recordHistory) {
        for (SearchResult& r : rows) {
            if (adminRemembered(r))
                r.runAsAdmin = true; // the row says so (管理员)
        }
    }
    // Nothing typed in 全部, the favorites as tiles: their rows first (over
    // a keep-awake's, which the tiles stand over anyway), drawn as tiles by
    // Main.qml and left out of the list there. Rows still, so the keys, the
    // menu, the preview and the selection are a row's.
    SearchResults tiles;
    if (m_favoriteTilesOn && m_favoritesOnStart && !m_panel && !m_keyword && m_scope == All && searchText().isEmpty()) {
        std::stable_partition(rows.begin(), rows.end(), [](const SearchResult& r) { return r.favorite && !r.statusRow; });
        for (const SearchResult& r : rows) {
            if (!r.favorite || r.statusRow)
                break;
            tiles.append(r);
        }
    }
    const int tileRows = static_cast<int>(tiles.size());
    m_tiles.assign(std::move(tiles), {}); // what the tiles draw: these rows' copies
    if (m_tileRows != tileRows) {
        m_tileRows = tileRows;
        emit tileRowsChanged(); // before the rows: the list counts them out as it takes these
    }
    m_results.assign(std::move(rows), std::move(highlights), std::move(fuzzyHighlights));
    m_shownText = searchText();
    m_shownScope = m_scope;
    m_shownExtensions = fileExtensions();
    m_shownTypeFolders = fileTypeFolders();
    m_shownRankBy = rankBy();
    m_shownOrder = nameOrder();
    m_shownSort = sort();
    m_shownStartList = m_startList;
    m_shownKindOrder = m_kindOrder;
    m_shownPreferredTypes = m_preferredTypes;
    m_shownKeyword = m_keyword ? m_keyword->pluginId + u'/' + m_keyword->commandId : QString();
    m_shownContentFolder = m_contentFolder;
    m_shownContentTypes = contentTypesKey();
    m_shownPick = pickKey(m_contentPick);
    m_shownPanelKey = panelKey();
    m_shownTyping = typingKey();
    m_shownRegex = regexTerms();
    m_shownFuzzyMode = m_fuzzyMode;
    m_haveResults = true;
    m_shownStale = false;
    m_historyDirty = false;
    if (m_replacePending) {
        if (m_replaceClock.isValid() && m_replaceClock.elapsed() >= 300) {
            // Dimmed long enough to be seen (after 250 ms): why, in the log.
            qWarning("WinShun: new rows %lld ms after the list asked for them (asked again %d times meanwhile%s%s; "
                     "the last name search answered %lld ms after it was asked, %lld ms of it searching)",
                static_cast<long long>(m_replaceClock.elapsed()), m_replaceAsks, m_replaceWhy.isEmpty() ? "" : ": ",
                qUtf8Printable(m_replaceWhy.join(u", "_s)), static_cast<long long>(m_lastAsk.isValid() ? m_lastAsk.elapsed() : -1),
                static_cast<long long>(m_elapsedUs / 1000));
        }
        m_replaceClock.invalidate();
        m_replacePending = false;
        m_slowSearch.stop();
        m_searchSlow = false;
        forgetRowChanges(); // another list: what changed in the last one says nothing here
        // Its first row is the list's choice, not the user's; back out of a
        // folder, the folder's row (GitHub #361), chosen as it was.
        m_chosenPath = m_results.startRow() >= 0 ? m_results.startPath() : QString();
        if (!m_armedPowerCommand.isEmpty()) {
            const SearchResult* cur = m_results.at(m_currentRow);
            if (!cur || !cur->isPlace() || cur->target != m_armedPowerCommand)
                disarmPower();
        }
        emit resultsReplaced();
        if (const auto held = std::exchange(m_held, std::nullopt))
            emit heldKey(held->first, held->second); // on the rows it was pressed for
        // A Tab that waited for these rows (autocomplete), on them once they
        // are in; as Main.qml's Tab, the scopes when nothing completes.
        if (std::exchange(m_tabHeld, false)) {
            QMetaObject::invokeMethod(this, [this, query = m_query] {
                if (query == m_query && !autocomplete(m_currentRow))
                    cycleScope(1);
            }, Qt::QueuedConnection);
        }
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
    sortShownPanel();
}

// Keyword mode: the command's answer is the list (ModelSink::assign).
void Launcher::showPluginRows(SearchResults rows)
{
    if (m_keyword && !m_keyword->suggest.isEmpty()) {
        m_keywordOwn = std::move(rows);
        showSuggestedPaths();
        return;
    }
    if (m_panel && m_panelSort.key != SortKey::Rank)
        std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return resultColumnLess(a, b, m_panelSort); });
    showRows(std::move(rows), m_keyword ? ws::parseQuery(m_keyword->words).highlights : QStringList());
    refreshStatus();
}

bool Launcher::suggestsPaths() const
{
    return m_keyword && !m_keyword->suggest.isEmpty() && !m_keyword->words.trimmed().isEmpty();
}

// "code 项目" (GitHub #344): the words looked for as files or folders, as
// a command of the user's own does (commandFiles), the program's row kept.
void Launcher::searchSuggestedPaths()
{
    SearchEngine::Request request;
    request.text = m_keyword->words.trimmed();
    request.scope = m_keyword->suggest == u"folders" ? ws::Scope::Folders : ws::Scope::Files;
    request.kindOrder = m_kindOrder;
    request.rankBy = rankBy();
    request.preferredTypes = m_preferredTypes;
    request.excludedTypes = m_excludedTypes;
    request.excludedFileNames = m_excludedFileNames;
    request.limit = kPage;
    request.order = nameOrder();
    request.regex = regexTerms();
    request.fuzzyMode = fuzzyModeFromName(m_fuzzyMode);
    if (m_recordHistory) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        request.history = m_history->ranked(request.text, now);
        request.historyAffinity = byOpenCount() ? m_history->counts() : m_history->affinities(request.text, now);
    }
    m_requestId = m_engine->submit(std::move(request));
    m_nameRequestId = m_requestId;
    m_lastAsk.start();
    m_pending = true;
}

// The paths found, then the program's own row (the words as they are),
// or that row first when the words are a path there is ("code D:\项目").
void Launcher::showSuggestedPaths()
{
    const QString key = m_keyword->pluginId + u'/' + m_keyword->commandId;
    const QString words = m_keyword->words.trimmed();
    SearchResults rows;
    if (suggestsPaths() && m_keywordFoundFor == key)
        rows = m_keywordFound;
    const QFileInfo typed(words);
    const bool path = typed.isAbsolute() && !words.startsWith(u"\\\\") && typed.exists();
    rows = path ? m_keywordOwn + rows : rows + m_keywordOwn;
    showRows(std::move(rows), ws::parseQuery(words).highlights);
    refreshStatus();
}

bool Launcher::runWithPath(const SearchResult& row)
{
    const plugin::Entry* entry = m_plugins ? m_plugins->catalog().find(m_keyword->pluginId) : nullptr;
    const plugin::Command* command = entry && entry->manifest ? entry->manifest->command(m_keyword->commandId) : nullptr;
    if (!command || command->run.kind != plugin::Run::Kind::Program)
        return false;
    const QString locale = uiLocale();
    const plugin::Items items = plugin::runZeroCode(*command, plugin::zeroCodePathArgument(command->run, row.path), locale);
    if (items.isEmpty())
        return false;
    remember(row.path, m_keyword->words.trimmed()); // found for these words: first for them next time
    performPlugin(plugin::toRow(*entry, *command, items.front(), 0, locale), Open);
    return true;
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
// (contentPattern: the words, "a phrase", either of some, expressions, or a
// line of code as it is). Not for
// name syntax (ext:, wildcards, !exclusions: "*.log", "report !draft"),
// unless the text reads as code anyway ("if (!ok)", "char* p = s;"). Nor
// after a web shortcut's keyword: that is a search on the web ("gh
// WinShun"), and reading files for it only costs time. Nor for a path
// ("C:\Users\me\x"): that is a place to go to. Nor for a favorite's keyword.
// Nor typed in a folder's window (GitHub #110), whatever the words: every key
// there would start a scan of the whole disk.
QString Launcher::contentNeedle() const
{
    if (typedInFolder())
        return {};
    const QString text = searchText();
    const ParsedQuery query = ws::parseQuery(text, regexTerms());
    if (!query.error.isEmpty())
        return {};
    // "报告 | 总结": each part is a query of its own, and what goes for one
    // goes for them all.
    const std::vector<ParsedQuery> one {query};
    const std::vector<ParsedQuery>& parts = query.alternatives.empty() ? one : query.alternatives;
    const auto anyTerm = [&](const auto& is) {
        return std::ranges::any_of(parts, [&](const ParsedQuery& q) { return std::ranges::any_of(q.terms, is); });
    };
    if (!anyTerm([](const QueryTerm&) { return true; }) || m_keyword || typedKeyword(m_web, m_query)
        || !pathtext::splitTyped(text).folder.isEmpty())
        return {};
    // An expression is one for the text of files too (GitHub #295): in regex
    // mode every word, else the words that say so (regex:). "^IMG" and
    // ".pdf$" are about names.
    if (!regexTerms()) {
        qsizetype expressions = 0;
        for (const ParsedQuery& q : parts)
            expressions += std::ranges::count_if(q.terms, &QueryTerm::regex);
        const QStringList words = ws::queryWords(text);
        const qsizetype said = std::ranges::count_if(words, [](const QString& w) {
            return w.startsWith(u"regex:", Qt::CaseInsensitive) || w.startsWith(u"re:", Qt::CaseInsensitive);
        });
        if (expressions != said)
            return {};
    }
    if (m_favorites && (typedFavoriteKeyword(m_favorites->items(), text) >= 0
        || typedCommandKeyword(m_favorites->items(), text) >= 0))
        return {};
    const bool nameSyntax = std::ranges::any_of(parts, [](const ParsedQuery& q) { return !q.extensions.empty(); })
        || anyTerm([](const QueryTerm& t) { return t.negated || t.wildcard || t.fuzzy; });
    if (nameSyntax && !ws::looksLikeCode(text))
        return {};
    return text;
}

SearchEngine::Request Launcher::contentRequest() const
{
    SearchEngine::Request request;
    request.text = m_scope == Content ? searchText() : contentNeedle();
    request.scope = ws::Scope::Content;
    request.regex = m_regexMode; // every word an expression, on the files' text (GitHub #295)
    request.rankBy = rankBy();
    request.excludedTypes = m_excludedTypes;
    request.excludedFileNames = m_excludedFileNames;
    request.maxContentResults = kPage * m_pages;
    request.contentExtensions = m_contentExtensions;
    request.contentSizeLimits = m_contentSizeLimits;
    request.contentDocuments = m_contentDocuments;
    if (m_panel) {
        request.filter = panelFilter();
        if (!m_folderFilter.isEmpty())
            request.contentFolders = {m_folderFilter};
    }
    request.conditions = quickConditions();
    if (m_scope == Content) {
        // Asked for: system and program folders too if so set, and many
        // files at once (each open mostly waits for the antivirus).
        request.contentInLowPriority = m_contentInLowPriority;
        request.contentThreads = kContentThreads;
        setContentPlace(request);
    } else {
        // Typing a file name must not start a parallel document-parsing
        // workload (#153). Verify indexed matches on one reader; offer the
        // unsearched contents explicitly instead of silently omitting them.
        request.contentIndexedOnly = true;
        request.contentThreads = 1;
    }
    return request;
}

namespace {

// What the content pane counts as files: the files' rows, and a plugin's row
// that opens a plain file (a content provider's; GitHub #202) as that file's
// row, unless the file is listed already.
SearchResults paneFiles(const SearchResults& rows)
{
    SearchResults files;
    files.reserve(rows.size());
    QSet<QString> listed;
    for (const SearchResult& row : rows) {
        if (!row.isDir && row.app == AppKind::None) {
            files.append(row);
            listed.insert(row.path.toCaseFolded());
        }
    }
    for (const SearchResult& row : rows) {
        const QString file = row.isPlugin() && !plugin::asksDetails(*row.plugin) ? plugin::rowFile(*row.plugin) : QString();
        if (file.isEmpty() || listed.contains(file.toCaseFolded()))
            continue;
        listed.insert(file.toCaseFolded());
        SearchResult copy = row;
        copy.path = file;
        copy.app = AppKind::None;
        copy.plugin = {};
        copy.clipId = 0;
        copy.target.clear();
        files.append(std::move(copy));
    }
    return files;
}

} // namespace

void Launcher::syncContentPane()
{
    if (!m_contentPane) return;
    if (m_scope != Content) {
        m_contentPane->setActive(false);
        m_contentPane->cancelReport();
        return;
    }
    SearchResults rows;
    ContentPane::Subject current;
    int pluginRows = 0; // no file's: the summary says how many
    if (!stale()) {
        SearchResults all;
        all.reserve(m_results.count());
        for (int i = 0; i < m_results.count(); ++i) {
            if (const auto* row = m_results.at(i))
                all.append(*row);
        }
        rows = paneFiles(all);
        for (const SearchResult& row : all)
            pluginRows += row.isPlugin() && !plugin::isHintRow(row);
        pluginRows -= static_cast<int>(rows.size() - std::ranges::count_if(all, [](const SearchResult& row) {
            return !row.isDir && row.app == AppKind::None;
        }));
        if (const SearchResult* row = m_results.at(m_currentRow)) {
            if (!row->isPlugin())
                current = {row->path, row->path};
            else if (const auto details = pluginDetails(*row, m_contentPane->showsDetails()))
                current = {row->path, details->file, row->name, details->text, details->cut, details->message, details->waiting};
            else if (const QString file = plugin::rowFile(*row->plugin); !file.isEmpty())
                current = {row->path, file};
        }
    }
    // A plugin's keyword list: its words (as its details request has them,
    // GitHub #202), not those of the content search before it.
    const QString words = m_keyword ? m_keyword->words
        : m_contentScope.words.isEmpty() ? splitContentQuery(searchText(), m_regexMode).words : m_contentScope.words;
    m_contentPane->setContentIndexEnabled(m_contentIndex);
    m_contentPane->update(contentRequest(), words, searchText() + u'\n' + panelKey() + u'\n' + pickKey(m_contentPick)
            + u'\n' + m_contentFolder, std::move(rows), stale() ? SearchResults{} : paneFiles(m_results.selection()), current,
        m_contentScanned, m_contentTotal, m_scanComplete && !m_moreContent, searching(), m_leftOut, pluginRows);
}

void Launcher::startContentSearch()
{
    m_requestId = m_engine->submit(contentRequest());
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
    if (m_keyword) { // the paths for a program's words (searchSuggestedPaths)
        m_keywordFound.clear();
        for (const SearchResult& r : results) {
            if (fileRow(r) && !(m_keyword->suggest == u"files" && r.isDir))
                m_keywordFound.append(r);
        }
        m_keywordFoundFor = m_keyword->pluginId + u'/' + m_keyword->commandId;
        m_replacePending = true; // another list: its first row is current (what Enter hands over)
        showSuggestedPaths();
        return;
    }

    // More to list than came: names beyond the page, or the files written
    // lately filling it (the favorites before them not counted).
    if (showsStartList()) {
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
    if (const auto files = commandFiles()) {
        // The files and folders its words found, for the command; what a
        // command cannot be handed (apps, settings, sites) is not listed.
        // Its own row, the words as they are: after them, or before when
        // the words are a path there is ("code D:\\项目" runs as ever).
        const QString own = files->command.path;
        rows.removeIf([&](const SearchResult& r) { return r.path != own && !fileRow(r); });
        const auto it = std::ranges::find_if(rows, [&](const SearchResult& r) { return r.path == own; });
        const QFileInfo typed(files->words);
        const bool path = typed.isAbsolute() && !files->words.startsWith(u"\\\\") && typed.exists();
        if (it != rows.end() && !path)
            std::rotate(it, it + 1, rows.end());
    }
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
        // index has nothing to find them by again. After the names, as they
        // came; the folder typed in's own before the rest (GitHub #110).
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r->line == 0 && !r->isPlugin() && m_leftOutFound.contains(r->path) && !names.contains(r->path)) {
                const auto at = r->elsewhere
                    ? rows.end()
                    : std::ranges::find_if(rows, [](const SearchResult& x) { return x.elsewhere; });
                rows.insert(at, *r);
            }
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
        // Typed in a folder's window, one of the rows found elsewhere when it
        // now falls among them (GitHub #110), as ModelSink puts one there.
        const SearchResults& listed = std::as_const(rows);
        const qsizetype elsewhere
            = std::ranges::find_if(listed, [](const SearchResult& r) { return r.elsewhere && !r.isPlugin(); }) - listed.begin();
        for (qsizetype i = 0; i < rows.size(); ++i) {
            if (rows.at(i).isPlugin() && rows.at(i).elsewhere != (i > elsewhere))
                rows[i].elsewhere = i > elsewhere;
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
    // Rows found by how their names read (homophone) or by automatic FZF fuzzy
    // fallback: the heading says so (approximateHeading). Homophone highlights
    // are expanded to pinyin; fuzzy fallback keeps the typed query terms for
    // discrete FZF character highlighting.
    m_approximate = std::ranges::any_of(results, [](const SearchResult& r) { return r.approximate; });
    m_approximateFuzzy = std::ranges::any_of(results, [](const SearchResult& r) { return r.approximate && r.fuzzy; });
    const ParsedQuery query = ws::parseQuery(nameText(), regexTerms()); // typed in a folder, its words each (GitHub #110)
    QStringList highlights = query.highlights;
    QStringList fuzzyHighlights = query.fuzzyHighlights;
    if (m_approximate && !m_approximateFuzzy) {
        highlights.clear();
        fuzzyHighlights.clear();
        for (const ParsedQuery& reading : ws::homophoneQueries(query)) {
            for (const QString& word : reading.highlights) {
                if (!highlights.contains(word))
                    highlights.append(word);
            }
        }
    }
    showRows(std::move(rows), std::move(highlights), std::move(fuzzyHighlights));
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
        bool provided = false;
        if (m_merge) {
            for (int i = 0; i < m_results.count() && !provided; ++i) {
                const auto* existing = m_results.at(i);
                provided = existing->isPlugin() && existing->plugin->openTarget.compare(r.path, Qt::CaseInsensitive) == 0
                    && m_merge->contentRow(*existing);
            }
        }
        if (provided)
            continue;
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
    if (m_loadingMore) { // a page more: after the rows, as they come (loadMore); before the providers' (contentEnd)
        int row = m_merge ? m_merge->contentEnd() : m_results.count();
        if (row <= m_currentRow || row == m_results.count()) { // the keyboard among those: they stay where they are seen
            m_results.append(fresh);
            return;
        }
        for (const SearchResult& r : fresh)
            m_results.insert(row++, r);
        return;
    }
    // Each among the content rows by how well it matches (they come best
    // first, a tie after those already there); by time, among the files
    // (timePlace). Never above the row the keyboard is on, nor the rows
    // above it: they stay where they are seen; on the first row, a better
    // match takes its place. Above the content providers' rows, which come
    // after the native matches, unless the keyboard is among those.
    const int current = m_currentRow;
    const int floor = current > 0 ? current + 1 : 0;
    const RankBy by = rankBy();
    bool aboveFirst = false;
    for (const SearchResult& r : fresh) {
        int row = m_merge ? m_merge->contentEnd() : m_results.count();
        if (row < floor)
            row = m_results.count();
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

void Launcher::onNameLeftOut(quint64 id, const NameLeftOut& leftOut)
{
    if (id != m_nameRequestId || !m_hintRunning.isEmpty())
        return; // superseded
    m_nameLeftOut = leftOut;
    if (m_hintsTaken.contains(kHiddenFoldersHint))
        m_nameLeftOut.hiddenFolders = 0;
    if (m_hintsTaken.contains(kExcludedFilesHint))
        m_nameLeftOut.excludedFiles = 0;
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
// taken up stands there), each file once. Typed in a folder's window with
// its rows first (GitHub #110): a drive's are elsewhere, after the caption;
// the folder's own (not indexed: GitHub #52) go with its rows, before it.
void Launcher::onDriveResults(quint64 id, const SearchResults& results)
{
    if (id != m_requestId)
        return;
    const bool grouped = !m_typingOnly && !typingWords().isEmpty();
    const bool ownFolder = m_hintRunning.startsWith(kFolderHint)
        && pathtext::sameFolder(walkedPath(m_hintRunning), m_typingFolder);
    SearchResults fresh;
    for (SearchResult r : results) {
        if (m_results.indexOf(r.path) < 0) {
            r.elsewhere = grouped && !ownFolder;
            m_leftOutFound.insert(r.path);
            fresh.append(std::move(r));
        }
    }
    if (fresh.isEmpty())
        return;
    const bool hadNoLocal = m_results.count() == 0 || m_webFallbackOnly;
    if (m_webFallbackOnly && m_results.count() == 1 && m_results.at(0)->isWeb()) {
        m_results.removeRange(0, 1);
        m_webFallbackOnly = false;
    }
    m_hintHits += static_cast<int>(fresh.size());
    const int at = grouped && ownFolder ? m_results.elsewhereRow() : -1;
    if (at < 0) {
        m_results.append(fresh);
    } else {
        for (qsizetype i = 0; i < fresh.size(); ++i)
            m_results.insert(at + static_cast<int>(i), fresh[i]);
    }
    if (hadNoLocal)
        emit betterFirstRow();
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
    if (on) {
        m_panelSort = columnOrder(sort());
        emit panelSortChanged();
    }
    m_panel = on;
    followFilterSort(); // the chips count in the panel only
    m_results.setFolderSizesEnabled(on);
    emit panelChanged();
    if (m_appsTabWanted)
        appsTabMoved(); // 程序 is the box's
    refreshStatus();
    if (m_window && m_window->isVisible() && !m_keyword)
        search(); // the rows as the panel (or the box) lists them; the same ones stay where they are
}

void Launcher::togglePanel()
{
    emit panelToggleRequested();
}

void Launcher::setPanelSidebarVisible(bool visible)
{
    if (m_panelSidebarVisible == visible)
        return;
    m_panelSidebarVisible = visible;
    QSettings(Settings::filePath(), QSettings::IniFormat).setValue(u"Panel/SidebarVisible"_s, visible);
    emit panelSidebarVisibleChanged();
}

void Launcher::setPanelFiltersOnTop(bool top)
{
    if (m_panelFiltersOnTop == top)
        return;
    m_panelFiltersOnTop = top;
    QSettings(Settings::filePath(), QSettings::IniFormat).setValue(u"Panel/FiltersOnTop"_s, top);
    emit panelFiltersOnTopChanged();
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

void Launcher::setClosing(bool on)
{
    if (on == m_closing)
        return;
    m_closing = on;
    emit closingChanged();
}

bool Launcher::filtersActive() const
{
    return m_dateChoice != 0 || m_sizeChoice != 0 || !m_folderFilter.isEmpty() || !m_quickFiltersOn.isEmpty();
}

QVariantList Launcher::quickFilters() const
{
    QVariantList filters;
    for (const QuickFilter& f : m_quickFilters)
        filters.append(QVariantMap {{u"name"_s, f.name}, {u"query"_s, f.query}, {u"keyword"_s, f.keyword}, {u"sort"_s, f.sort}});
    return filters;
}

QStringList Launcher::quickConditions() const
{
    QStringList conditions;
    const QList<int>& typed = typedQuick().filters;
    for (qsizetype i = 0; i < m_quickFilters.size(); ++i) {
        if ((m_panel && m_quickFiltersOn.contains(m_quickFilters[i].name)) || typed.contains(int(i)))
            conditions.append(m_quickFilters[i].query);
    }
    return conditions;
}

const QuickFilterQuery& Launcher::typedQuick() const
{
    if (!m_typedQuickRead || m_typedQuickFor != m_query) {
        m_typedQuick = splitQuickFilterQuery(m_query.trimmed(), m_quickFilters);
        m_typedQuickFor = m_query;
        m_typedQuickRead = true;
    }
    return m_typedQuick;
}

// GitHub #392: a quick filter with an order of its own ("大文件", largest
// first) puts the list in that order while it is on: named in the box, or
// its chip in the panel. The first one on that has an order decides. The
// order chosen is kept as it was (chosenSort, the one remembered): with the
// filter gone, it is in force again. Choosing another meanwhile (setSort, a
// column of the panel) sets the filter's aside until another one comes.
void Launcher::followFilterSort()
{
    QString from;
    std::optional<Sort> wanted;
    const QList<int>& typed = typedQuick().filters;
    for (qsizetype i = 0; i < m_quickFilters.size() && !wanted; ++i) {
        const QuickFilter& f = m_quickFilters[i];
        if (isSortName(f.sort) && ((m_panel && m_quickFiltersOn.contains(f.name)) || typed.contains(int(i)))) {
            from = f.sort + u'|' + f.name; // no '|' in a sort's name
            wanted = static_cast<Sort>(sortIndex(f.sort));
        }
    }
    if (from == m_filterSortFrom)
        return; // the same: in force, or set aside
    const std::optional<Sort> was = m_filterSort;
    const Sort before = sort();
    const SortOrder panelBefore = m_panelSort;
    m_filterSortFrom = from;
    m_filterSort = wanted;
    // The panel's columns too: the filter's order, the one before given back
    // after, unless a column was clicked meanwhile.
    if (wanted) {
        if (!was) // the panel's own, or the one it would open with
            m_panelSortBeforeFilter = m_panel ? m_panelSort : columnOrder(m_sort);
        m_panelSort = columnOrder(*wanted);
    } else if (was && m_panelSort == columnOrder(*was)) {
        m_panelSort = m_panelSortBeforeFilter;
    }
    if (sort() != before)
        emit sortChanged();
    if (m_panelSort != panelBefore)
        emit panelSortChanged();
}

void Launcher::setQuickFilters(const QuickFilters& filters)
{
    if (filters == m_quickFilters)
        return;
    const QStringList before = quickConditions();
    const QString words = searchText();
    const SortOrder order = nameOrder();
    const RankBy ranked = rankBy();
    m_quickFilters = filters;
    m_typedQuickRead = false; // a keyword given or taken away: what is typed reads otherwise
    updateQueryPrefix();
    // Renamed or removed: off, as nothing shows it any more.
    m_quickFiltersOn.removeIf([&](const QString& name) {
        return std::ranges::none_of(filters, [&](const QuickFilter& f) { return f.name == name; });
    });
    emit quickFiltersChanged();
    emit filtersChanged();
    followFilterSort(); // its order changed, or it is no more
    const bool reordered = nameOrder() != order || rankBy() != ranked;
    if ((quickConditions() != before || searchText() != words || reordered) && m_window && m_window->isVisible())
        search(); // one that is on was edited: its rows, in place
}

void Launcher::toggleQuickFilter(int index)
{
    if (index < 0 || index >= m_quickFilters.size())
        return;
    const QString name = m_quickFilters[index].name;
    if (!m_quickFiltersOn.removeOne(name))
        m_quickFiltersOn.append(name);
    emit filtersChanged();
    followFilterSort();
    if (m_panel)
        search();
}

void Launcher::editQuickFilters()
{
    // What is typed as the new one's query ("!node_modules\ ext:js"), as
    // Everything's "Add to filters": names only, and only what can be one.
    const QString typed = m_scope == Content || regexTerms() ? QString() : searchText().trimmed();
    emit quickFilterSettingsRequested(Settings::quickFilterProblem(typed).isEmpty() ? typed : QString());
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

FileTime fileTimeOf(const QDateTime& when)
{
    const qint64 secs = when.toSecsSinceEpoch();
    return secs <= 0 ? 0 : secs >= 0xFFFF'FFFFll ? 0xFFFF'FFFFu : static_cast<FileTime>(secs);
}

// The panel's own size boxes: "40 KB", "1.5 GB", or a bare number of MB (GitHub #394).
std::optional<std::uint64_t> sizeBoundOf(const QString& text)
{
    return ws::parseSizeValue(text, std::uint64_t {1} << 20);
}

} // namespace

bool Launcher::readsAsSize(const QString& text) const
{
    return text.trimmed().isEmpty() || sizeBoundOf(text).has_value();
}

bool Launcher::readsAsDate(const QString& text) const
{
    return text.trimmed().isEmpty() || ws::parseDateValue(text).has_value();
}

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
        // "2026" or "2026-3" as well: from its first day, to its last.
        const auto from = ws::parseDateValue(m_dateFrom);
        const auto to = ws::parseDateValue(m_dateTo);
        if (from)
            f.after = fileTimeOf(from->first);
        if (to)
            f.before = fileTimeOf(to->second);
        if (!from && !to)
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
        const auto min = sizeBoundOf(m_sizeMin);
        const auto max = sizeBoundOf(m_sizeMax);
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
    // Only what changes the rows: the box's list is the panel's when nothing
    // narrows or reorders it, so expanding keeps the rows (and the selection).
    QString key;
    if (const QStringList conditions = quickConditions(); !conditions.isEmpty())
        key += u"q:"_s + conditions.join(QChar(0x1f)) + u';'; // in the box too: named by keyword
    if (!m_panel)
        return key;
    if (filtersActive()) {
        key += u"f:%1,%2,%3,%4,%5,%6,%7;"_s.arg(m_dateChoice).arg(m_dateFrom, m_dateTo).arg(m_sizeChoice).arg(m_sizeMin, m_sizeMax, m_folderFilter);
    }
    if (m_panelSort.key != SortKey::Rank)
        key += u"o:%1,%2;"_s.arg(static_cast<int>(m_panelSort.key)).arg(m_panelSort.descending ? 1 : 0);
    if (searchText().isEmpty())
        key += u"browse;"_s;
    return key;
}

QJsonObject Launcher::pluginFilters() const
{
    QJsonObject content {{u"contentEnabled"_s, m_scope == Content || (m_contentEnabled && !contentNeedle().isEmpty())},
        {u"contentWords"_s, m_scope == Content ? searchText() : contentNeedle()},
        {u"excludedExtensions"_s, QJsonArray::fromStringList(m_excludedTypes)},
        {u"excludedFileNames"_s, QJsonArray::fromStringList(m_excludedFileNames)}};
    if (m_scope == Content) {
        const QStringList typeFolders = contentTypeFolders();
        content.insert(u"folder"_s, !m_contentFolder.isEmpty() ? m_contentFolder
                : !m_folderFilter.isEmpty() || typeFolders.size() != 1 ? m_folderFilter : typeFolders.front());
        content.insert(u"contentPick"_s, m_contentPick.folder);
        if (!m_contentTypes.isEmpty()) { // the kinds chosen (GitHub #447), as the panel's filters name them
            QJsonArray kinds;
            for (int type : m_contentTypes)
                kinds.append(m_fileTypes[type].id.isEmpty() ? m_fileTypes[type].name : m_fileTypes[type].id);
            content.insert(u"kinds"_s, kinds);
            content.insert(u"extensions"_s, QJsonArray::fromStringList(contentTypeExtensions()));
        }
    }
    if (!m_panel)
        return content;
    const EntryFilter filter = panelFilter();
    const auto time = [](FileTime value) -> QJsonValue {
        return value == 0 ? QJsonValue(QJsonValue::Null)
                          : QJsonValue(QDateTime::fromSecsSinceEpoch(value, QTimeZone::UTC).toString(Qt::ISODate));
    };
    QJsonArray kinds;
    if (m_fileType >= 0 && m_fileType < m_fileTypes.size()) {
        for (int type : selectedFileTypes())
            kinds.append(m_fileTypes[type].id.isEmpty() ? m_fileTypes[type].name : m_fileTypes[type].id);
    } else if (m_scope == Folders)
        kinds.append(u"folders"_s);
    else if (m_scope == Files)
        kinds.append(u"files"_s);
    else if (m_scope == Content)
        kinds.append(u"content"_s);
    QJsonObject result {{u"modifiedFrom"_s, time(filter.after)}, {u"modifiedTo"_s, time(filter.before)},
        {u"minSize"_s, filter.minSize == 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(qint64(unpackSize(filter.minSize)))},
        {u"maxSize"_s, filter.maxSize == kUnknownSize ? QJsonValue(QJsonValue::Null) : QJsonValue(qint64(unpackSize(filter.maxSize)))},
        {u"kinds"_s, kinds}, {u"extensions"_s, QJsonArray::fromStringList(fileExtensions())},
        {u"folder"_s, m_folderFilter}};
    for (auto it = content.begin(); it != content.end(); ++it)
        result.insert(it.key(), it.value());
    return result;
}

void Launcher::sortShownPanel(const QString& currentPath)
{
    if (!m_panel || m_panelSort.key == SortKey::Rank || m_results.count() < 2)
        return;
    const SearchResult* current = m_results.at(m_currentRow);
    const QString kept = currentPath.isEmpty() && current ? current->path : currentPath;
    SearchResults rows;
    for (int i = 0; i < m_results.count(); ++i)
        rows.push_back(*m_results.at(i));
    std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return resultColumnLess(a, b, m_panelSort); });
    m_results.assign(std::move(rows), ws::parseQuery(searchText(), regexTerms()).highlights);
    if (!kept.isEmpty())
        emit panelRowRestored(m_results.indexOf(kept));
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
    m_quickFiltersOn.clear();
    emit filtersChanged();
    if (m_panel)
        search();
}

void Launcher::sortBy(int key)
{
    const auto sortKey = static_cast<SortKey>(std::clamp(key, 0, 8));
    SortOrder order;
    if (sortKey == m_panelSort.key && sortKey != SortKey::Rank) {
        order = {sortKey, !m_panelSort.descending};
    } else {
        // Names and paths up (a to z); sizes and times down (the largest, the newest first).
        order = {sortKey, sortKey == SortKey::Size || sortKey == SortKey::Modified || sortKey == SortKey::Created};
    }
    if (order == m_panelSort)
        return;
    m_panelSort = order;
    emit panelSortChanged();
    if (m_keyword)
        sortShownPanel();
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
    if (searchText().isEmpty() || (!m_haveResults && !m_contentRunning)) {
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
    if (!m_queryError.isEmpty())
        s = m_queryError;
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
    disarmPower();
    // The keyboard chose another row: that one is meant, unless it changed a
    // moment ago by itself (a number key, an arrow onto a row that just
    // came): then Enter waits a moment as for a row that went. Moving with
    // its own row (followCurrentRow) changes neither.
    if (!m_following) {
        if (rowJustChanged(row))
            m_currentGone.start();
        else
            m_currentGone.invalidate();
    }
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
    QString tab;
    QString tabKeys = u"Tab"_s; // the volume's row: its place is Alt+←→'s (Tab still does the same)
    switch (tabAction(m_currentRow)) {
    case navigation::TabAction::BrowseFolder: tab = tr("进入文件夹"); break;
    case navigation::TabAction::Complete: tab = tr("补全"); break;
    case navigation::TabAction::SwitchScope:
        // In a folder, the way back out (GitHub #361) where Tab only switches
        // the scope, which goes unsaid; not while a name is typed in it (the
        // key takes that away first, as editing).
        if (pathtext::splitTyped(m_query).name.isEmpty() && stepBack()) {
            tabKeys = u"Ctrl+Backspace"_s;
            tab = tr("返回上一级");
        } else if (!m_query.trimmed().isEmpty()) {
            tab = tr("切换范围");
        }
        break;
    case navigation::TabAction::None: break;
    }
    // A keyword took the list: Shift+Tab looks for the words as names (GitHub #411).
    if (tab.isEmpty() && m_keyword) {
        tabKeys = u"Shift+Tab"_s;
        tab = tr("改搜文件名");
    }
    const SearchResult* r = m_results.at(m_currentRow);
    if (!r) {
        // Without a row, a nonblank query can still offer scope switching.
    } else if (r->isPlugin()) {
        const PluginRow& p = *r->plugin;
        if (p.valid) {
            if (!p.setQuery.isEmpty() || !p.searchQuery.isEmpty()) {
                // Enter drills (performPlugin prefers setQuery over actions for
                // a row that has both): the hint must say so, not name action 0.
                enter = !p.searchQuery.isEmpty() ? tr("搜索") : tr("填入搜索框");
                if (p.actions.size() > 1) {
                    secondKeys = p.actions[1].key.isEmpty() ? u"Ctrl+Enter"_s : p.actions[1].key;
                    second = p.actions[1].title;
                }
            } else if (!p.actions.isEmpty()) {
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
    } else if (r->isWindow()) {
        enter = tr("切换到窗口");
    } else if (r->isWeb()) {
        enter = r->words.isEmpty() ? tr("打开网站") : tr("搜索");
    } else if (r->isPlace()) {
        if (ws::isPowerCommand(r->target)) {
            enter = isPowerArmed(r->target) ? tr("确认%1").arg(r->name)
                  : !ws::powerCommandNeedsConfirm(r->target) ? tr("锁定")
                  : tr("执行");
        } else {
            enter = ws::isOwnCommand(r->target) ? tr("打开设置") : tr("打开");
        }
        if (!r->switchId.isEmpty())
            switchKeyHints(*r, enter, secondKeys, second);
        if (steppable(*r)) {
            tabKeys = u"Alt+←→"_s;
            tab = tr("调音量");
        }
    } else if (r->isApp()) {
        enter = adminRemembered(*r) ? tr("以管理员身份运行") : tr("打开"); // GitHub #313: said before Enter is pressed
        if (!r->target.isEmpty()) {
            secondKeys = u"Ctrl+Enter"_s;
            second = r->isPackagedApp() ? tr("打开安装文件夹") : tr("打开位置");
        }
    } else {
        enter = r->isDir ? tr("打开文件夹") : adminRemembered(*r) ? tr("以管理员身份运行") : tr("打开");
        secondKeys = u"Ctrl+Enter"_s;
        // Typed in a folder's window: its direct items are followed there as the
        // keys move; the rest are not (that would navigate the tab), Ctrl+Enter
        // takes the tab to them (App::revealInTypingTab, GitHub #182).
        second = typedInFolder() && !pathtext::sameFolder(QFileInfo(r->path).absolutePath(), m_typingFolder)
            ? tr("在原窗口定位") : tr("打开位置");
    }
    if (r && m_keyword && !m_keyword->suggest.isEmpty() && !r->isPlugin() && fileRow(*r))
        enter = m_keyword->label; // the command's title says what it does with it: 用 VS Code 打开
    else if (const auto files = r && fileRow(*r) ? commandFiles() : std::nullopt)
        enter = tr("用“%1”打开").arg(files->command.name);
    else if (r && r->isCommand() && commandFiles())
        enter = tr("原样传入“%1”").arg(r->words);
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

void Launcher::flash(const QString& message, std::chrono::milliseconds shown)
{
    m_flash = message;
    m_flashTimer.start(shown); // sets the interval: every flash says how long
    emit statusChanged();
}

// An action waiting for a slow server is no longer wanted: its answer is
// ignored when it comes, and its "正在连接 …" goes now.
void Launcher::dropServerWait()
{
    ++m_serverWaitId;
    if (!m_serverWaitFlash.isEmpty() && m_flash == m_serverWaitFlash) {
        m_flashTimer.stop();
        m_flash.clear();
        emit statusChanged();
    }
    m_serverWaitFlash.clear();
}

void Launcher::disarmPower()
{
    if (m_armedPowerCommand.isEmpty())
        return;
    m_armedPowerCommand.clear();
    m_armedPowerTimer.stop();
    if (!m_armedPowerFlash.isEmpty() && m_flash == m_armedPowerFlash) {
        m_flashTimer.stop();
        m_flash.clear();
        emit statusChanged();
    }
    m_armedPowerFlash.clear();
    refreshKeyHints();
}

bool Launcher::isPowerArmed(QStringView command) const noexcept
{
    return !m_armedPowerCommand.isEmpty() && m_armedPowerTimer.isActive() && m_armedPowerCommand == command;
}

// `r`, which an action needed, is not there (any more): out of the history
// (and of the list, when it was to be deleted), and said so.
void Launcher::forgetMissing(const SearchResult& r, Action action)
{
    if (action == Recycle)
        m_results.remove(r.path);
    m_history->remove(r.path);
    emit historyChanged();
    // A favorite stays: it may be back (a drive plugged in again), or be picked anew.
    flash(r.favorite ? tr("“%1” 已不存在；右键可从收藏夹移除").arg(r.name) : tr("“%1” 已不存在").arg(r.name));
}

// "正在连接 …" for the server of `path`, which an action waits for off the
// UI thread: at once, the window never waiting with it (GitHub #116). A
// moment later for a server that answered a moment ago: it answers again
// in milliseconds, and the words would only flicker past. WebClient started
// first (GitHub #114) is said instead, at once.
quint64 Launcher::showServerWait(const QString& path, bool clientStarting)
{
    const quint64 id = m_serverWaitId;
    const QString text = clientStarting ? tr("正在启动 Windows 的 WebDAV 服务…") : tr("正在连接 %1…").arg(serverName(path));
    const auto shown = clientStarting ? kClientStartShown : kServerWaitShown;
    const auto show = [this, id, text, shown] {
        if (m_serverWaitId != id)
            return; // answered meanwhile
        m_serverWaitFlash = text;
        flash(text, shown); // until the answer's flash replaces it
    };
    if (!clientStarting && share::lastHeard(path.toStdWString(), kJustHeard).value_or(false))
        QTimer::singleShot(kQuietWait, this, show);
    else
        show();
    return id;
}

// The files go to the shares' cards in any case (tellShares). Joined, the
// check started by the first asking is answered once, for the newest: Enter
// held on a server gone away starts no new thread at each repeat, each one
// queueing behind the last for Windows (askWindows: one at a time).
void Launcher::checkServersThen(const QStringList& paths, std::function<void(const ServerFound&)> then)
{
    QStringList keys;
    for (const QString& path : paths)
        keys.append(pathtext::pathKey(path));
    keys.sort();
    const QString key = keys.join(u'\n');
    const auto underWay = m_serverChecks.constFind(key);
    const bool joined = underWay != m_serverChecks.cend();
    // Joined, as the check under way was started (with WebClient or not).
    const bool clientStarting = joined ? underWay->clientStarting : clientStarts(paths);
    m_serverChecks.insert(key, {showServerWait(paths.first(), clientStarting), clientStarting, std::move(then)});
    if (joined)
        return;
    ++m_serverChecksStarted;
    std::thread([guard = QPointer(this), paths, key, clientStarting] {
        const ServerFound found = checkServers(paths, clientStarting);
        QMetaObject::invokeMethod(qApp, [guard, key, found] {
            if (!guard)
                return;
            tellShares(guard->m_index->shares(), found);
            const ServerCheck check = guard->m_serverChecks.take(key);
            if (!check.then || guard->m_serverWaitId != check.wait)
                return; // another action since, or the launcher went: not wanted any more
            guard->dropServerWait(); // answered: "正在连接 …" goes either way
            check.then(found);
        });
    }).detach();
}

// A real disk/share file or folder path (not an app, place, web, plugin,
// window, custom command, screenshot row, drive root, or bare UNC share root).
static bool isDeletablePath(const SearchResult& r)
{
    if (r.isApp() || r.isCommand() || r.clipId != 0 || r.name.isEmpty())
        return false;
    QString path = QDir::toNativeSeparators(r.path);
    while (path.size() > 3 && path.endsWith(u'\\'))
        path.chop(1);
    if (path.size() <= 3 || path.lastIndexOf(u'\\') <= 1)
        return false;
    const std::wstring wide = path.toStdWString();
    if (unc::isUnc(wide)) {
        const std::wstring_view root = unc::shareRoot(wide);
        if (root.empty() || wide.size() <= root.size() + 1)
            return false;
    }
    return true;
}

static bool recyclable(const SearchResult& r)
{
    return isDeletablePath(r) && !r.missing && !r.offline;
}

void Launcher::copyFiles(const QStringList& files)
{
    if (m_fileCopier)
        m_fileCopier(files);
    else
        shell::copyFiles(files);
}

void Launcher::recycleFiles(const QStringList& files, std::function<void(bool)> done)
{
    const QStringList prepared = shell::prepareRecyclePaths(files);
    if (prepared.isEmpty()) {
        done(false);
        return;
    }
    ++m_shellOpsInFlight;
    auto finish = [self = QPointer(this), done = std::move(done)](bool ok) mutable {
        QMetaObject::invokeMethod(qApp, [self, done = std::move(done), ok] {
            if (!self)
                return;
            self->m_shellOpsInFlight = std::max(0, self->m_shellOpsInFlight - 1);
            done(ok);
        }, Qt::QueuedConnection);
    };
    if (m_fileRecycler) {
        m_fileRecycler(prepared, std::move(finish));
        return;
    }
    const auto owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    shell::recycle(prepared, owner, std::move(finish));
}

void Launcher::cycleScope(int delta)
{
    // As the tabs show them (文件夹 within 文件): 全部, 文件, 内容, 程序.
    QVarLengthArray<Scope, 4> order {All};
    if (!m_mergeFileScope)
        order.append(Files);
    if (m_contentEnabled)
        order.append(Content);
    if (appsTab())
        order.append(Apps);
    const int scopes = static_cast<int>(order.size());
    if (scopes < 2)
        return;
    const Scope tab = m_scope == Folders || (m_mergeFileScope && m_scope == Files) ? (m_mergeFileScope ? All : Files) : m_scope;
    const int at = std::max(0, static_cast<int>(order.indexOf(tab)));
    chooseScope(order[((at + delta) % scopes + scopes) % scopes]);
}

QVariantList Launcher::fileTypes() const
{
    QVariantList types;
    for (const FileType& type : m_fileTypes)
        types.append(QVariantMap {{u"name"_s, type.name}, {u"extensions"_s, type.extensions.join(u' ')}, {u"keyword"_s, type.keyword},
            {u"folder"_s, type.folder}});
    return types;
}

void Launcher::setFileTypes(const FileTypes& types)
{
    if (types == m_fileTypes)
        return;
    // The kind chosen stays chosen where it went (moved up, renamed: by its
    // id or name); gone, any kind.
    QStringList chosen;
    const auto oldTypes = selectedFileTypes();
    for (int type : oldTypes)
        chosen.append(fileTypeKey(m_fileTypes[type]));
    QStringList contentChosen;
    for (int type : m_contentTypes)
        contentChosen.append(fileTypeKey(m_fileTypes[type]));
    m_fileTypes = types;
    QList<int> contentTypes;
    for (const QString& key : contentChosen) {
        if (const int type = fileTypeIndex(key); type >= 0)
            contentTypes.append(type);
    }
    const bool contentMoved = contentTypes != m_contentTypes;
    m_contentTypes = std::move(contentTypes);
    QList<int> selected;
    for (const QString& key : chosen) {
        const int type = fileTypeIndex(key);
        if (type >= 0)
            selected.append(type);
    }
    if (selected != oldTypes) {
        m_fileType = selected.isEmpty() ? -1 : selected.takeFirst();
        m_extraFileTypes = selected;
        emit fileTypeChanged();
        emit fileChoiceChanged();
    } else if (contentMoved) {
        emit fileTypeChanged();
    }
    emit fileTypesChanged();
    emit folderChipsChanged(); // 内容's strip names its kinds
    updateQueryPrefix();
    emit scopeChanged(); // the placeholder names the kind
    // Its extensions changed: listed again (a refresh when they did not).
    if ((m_fileType >= 0 || (m_scope == Content && (contentMoved || !m_contentTypes.isEmpty()))) && m_window && m_window->isVisible())
        search();
}

QStringList Launcher::fileExtensions() const
{
    QStringList extensions;
    if (m_scope == Files) {
        for (int type : selectedFileTypes())
            extensions.append(m_fileTypes[type].extensions);
    }
    extensions.removeDuplicates();
    return extensions;
}

QStringList Launcher::fileTypeFolders() const
{
    QStringList folders;
    if (m_scope == Files) {
        for (int type : selectedFileTypes()) {
            const QString& folder = m_fileTypes[type].folder;
            // 多选的类型和范围分别合并；有不限范围的类型时使用整个索引。
            if (folder.isEmpty())
                return {};
            folders.append(folder);
        }
    }
    folders.removeDuplicates();
    return folders;
}

QStringList Launcher::contentTypeExtensions() const
{
    QStringList extensions;
    for (int type : m_contentTypes)
        extensions.append(m_fileTypes[type].extensions);
    extensions.removeDuplicates();
    return extensions;
}

QStringList Launcher::contentTypeFolders() const
{
    QStringList folders;
    for (int type : m_contentTypes) {
        const QString& folder = m_fileTypes[type].folder;
        if (folder.isEmpty())
            return {}; // one kind is anywhere: so is the search (as fileTypeFolders)
        folders.append(folder);
    }
    folders.removeDuplicates();
    return folders;
}

void Launcher::setContentTypes(QList<int> types)
{
    types.removeIf([&](int type) { return type < 0 || type >= m_fileTypes.size(); });
    if (types == m_contentTypes)
        return;
    m_contentTypes = std::move(types);
    emit fileTypeChanged();
    emit folderChipsChanged(); // the strip shows them
    emit scopeChanged(); // the placeholder names them
    if (m_scope != Content)
        return; // kept for when 内容 comes back
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints();
    search(); // the rows stay until the new ones come (see setScope)
}

QList<int> Launcher::selectedFileTypes() const
{
    QList<int> types = m_extraFileTypes;
    if (m_fileType >= 0)
        types.prepend(m_fileType);
    return types;
}

QString Launcher::queryError() const
{
    return m_queryError;
}

void Launcher::setGallery(bool gallery)
{
    if (m_gallery == gallery)
        return;
    m_gallery = gallery;
    emit galleryChanged();
}

void Launcher::setThumbnailSize(int size)
{
    size = std::clamp(size, 96, 320);
    if (size == m_thumbnailSize)
        return;
    m_thumbnailSize = size;
    emit galleryChanged();
}

void Launcher::toggleFileType(int type)
{
    if (type < 0 || type >= m_fileTypes.size()) {
        chooseFileType(type);
        return;
    }
    if (m_scope == Content) { // the words in that kind of file too, or no longer (GitHub #447)
        QList<int> types = m_contentTypes;
        if (types.contains(type))
            types.removeAll(type);
        else
            types.append(type);
        setContentTypes(std::move(types));
        return;
    }
    clearQueryPrefix();
    auto types = m_scope == Files ? selectedFileTypes() : QList<int>();
    if (types.contains(type))
        types.removeAll(type);
    else
        types.append(type);
    m_scope = m_mergeFileScope && types.isEmpty() ? All : Files;
    m_foldersChosen = false;
    m_fileType = types.isEmpty() ? -1 : types.takeFirst();
    m_extraFileTypes = types;
    emit fileTypeChanged();
    emit fileChoiceChanged();
    emit scopeChanged();
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints();
    rememberScope(); // a chip of the strip: the user's own (chooseScope)
    search();
}

void Launcher::chooseFileType(int type)
{
    // In 内容 a kind narrows the words' search; 文件夹 (no words in a folder) is 文件夹's scope.
    if (m_scope == Content && type != kFoldersChip) {
        setContentTypes(type >= 0 && type < m_fileTypes.size() ? QList<int> {type} : QList<int> {});
        return;
    }
    const bool hadMultiple = !m_extraFileTypes.isEmpty();
    m_extraFileTypes.clear();
    const bool hadPrefix = clearQueryPrefix();
    const Scope scope = type == kFoldersChip ? Folders : m_mergeFileScope && type == -1 ? All : Files;
    if (type < -1 || type >= m_fileTypes.size())
        type = -1;
    if (scope == m_scope && type == m_fileType && !hadMultiple) {
        if (hadPrefix) {
            // The chip the prefix already showed, clicked: the user's own
            // choice from here on (kept by clearQueryPrefix), its scope too.
            rememberScope();
            search();
        }
        return;
    }
    m_scope = scope;
    if (type != m_fileType || m_foldersChosen != (scope == Folders) || hadMultiple) {
        m_fileType = type;
        m_foldersChosen = scope == Folders;
        emit fileTypeChanged();
        emit fileChoiceChanged();
    }
    emit scopeChanged();
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints();
    rememberScope(); // a chip, or Ctrl+Tab through them: the user's own (chooseScope)
    search(); // the rows stay until the new ones come (see setScope)
}

void Launcher::cycleFileType(int delta)
{
    QList<int> chips {-1, kFoldersChip};
    for (int i = 0; i < m_fileTypes.size(); ++i)
        chips.append(i);
    const int at = std::max(0, static_cast<int>(chips.indexOf(m_scope == Folders ? kFoldersChip : m_scope == All ? -1 : m_fileType)));
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
    // While a prefix is typed the strip shows the prefix's kinds, and those
    // only pass: what is kept between runs is what the strip goes back to
    // once the prefix is gone (m_filePrefix). Kept as shown, a quit with the
    // prefix still typed left the strip on the prefix's kind at the next start.
    if (m_filePrefix) {
        const QStringList& keys = m_filePrefix->typeKeys;
        if (m_filePrefix->folders)
            return u"folders"_s;
        if (keys.size() > 1)
            return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(keys)).toJson(QJsonDocument::Compact));
        return keys.value(0);
    }
    if (!m_foldersChosen && !m_extraFileTypes.isEmpty()) {
        QStringList keys;
        for (int type : selectedFileTypes())
            keys.append(fileTypeKey(m_fileTypes[type]));
        return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(keys)).toJson(QJsonDocument::Compact));
    }
    return m_foldersChosen ? u"folders"_s : m_fileType >= 0 ? fileTypeKey(m_fileTypes[m_fileType]) : QString();
}

void Launcher::setFileChoice(const QString& choice)
{
    const auto array = QJsonDocument::fromJson(choice.toUtf8()).array();
    if (!array.isEmpty()) {
        QList<int> types;
        for (const QJsonValue& value : array) {
            const int type = fileTypeIndex(value.toString());
            if (type >= 0 && !types.contains(type))
                types.append(type);
        }
        m_fileType = types.isEmpty() ? -1 : types.takeFirst();
        m_extraFileTypes = types;
        m_foldersChosen = false;
        emit fileTypeChanged();
        emit fileChoiceChanged();
        if (m_scope == Files || m_scope == Folders) {
            m_scope = Files;
            emit scopeChanged();
            search();
        }
        return;
    }
    const int type = choice == u"folders" || choice.isEmpty() ? -1 : fileTypeIndex(choice);
    if (m_scope == Files || m_scope == Folders) {
        chooseFileType(choice == u"folders" ? kFoldersChip : type);
        return;
    }
    m_foldersChosen = choice == u"folders";
    if (type != m_fileType || !m_extraFileTypes.isEmpty()) {
        m_fileType = type;
        m_extraFileTypes.clear();
        emit fileTypeChanged();
    }
}

namespace {

constexpr QStringView kAllChip = u"all"; // every folder the name typed stands for
constexpr QStringView kPinPrefix = u"pin:"; // a folder chosen: m_contentFolder
constexpr QStringView kPickPrefix = u"pick:"; // one of those the name typed stands for: m_contentPick
constexpr QStringView kMissingChip = u"missing"; // a place not in the index: nothing to choose
constexpr QStringView kTypedChip = u"typed"; // a folder typed, not made out yet
constexpr QStringView kTypePrefix = u"type:"; // a kind of file 内容 is narrowed to (m_contentTypes): taken off when clicked
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
    // The kinds chosen (GitHub #447), and where they are kept when each is
    // bound to a folder (as 文件's typeFolders), unless a folder is chosen.
    request.contentTypes = contentTypeExtensions();
    if (request.contentFolders.isEmpty())
        request.contentFolders = contentTypeFolders();
}

const ContentScope::Place* Launcher::typedPlace() const
{
    if (m_scope != Content)
        return nullptr;
    const ContentQuery typed = splitContentQuery(searchText(), m_regexMode);
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
    // The kinds of file it is narrowed to (GitHub #447), first and on; a
    // click takes one off. `kind` its index in fileTypes (its colour), with
    // its extensions (its icon), as 文件's chips.
    for (const int type : m_contentTypes) {
        const FileType& kind = m_fileTypes[type];
        chips.append(QVariantMap {{u"type"_s, kTypePrefix.toString() + fileTypeKey(kind)}, {u"name"_s, kind.name},
            {u"tip"_s, tr("只搜%1中的文字，点一下取消").arg(kind.name)}, {u"dot"_s, QString()}, {u"kind"_s, type},
            {u"extensions"_s, kind.extensions.join(u' ')}});
    }
    // A folder named among the words: that one, or the ones it stands for.
    const ContentQuery typed = splitContentQuery(searchText(), m_regexMode);
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
        if (place->status == Status::NotFound)
            return kMissingChip.toString();
        if (place->status == Status::Searched) {
            if (!place->chosen.isEmpty())
                return kPickPrefix.toString() + place->chosen;
            return place->count > 1 || place->folders.isEmpty() ? kAllChip.toString() : kPickPrefix.toString() + place->folders.first();
        }
    } else if (!splitContentQuery(searchText(), m_regexMode).folders.empty()) {
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
    if (m_regexMode || !splitContentQuery(searchText(), m_regexMode).folders.empty())
        return {}; // in regex mode "abc/" is an expression: the folders below are the way
    return tr("或在词后加“文件夹名/”");
}

QString Launcher::contentWords() const
{
    if (typedPlace())
        return m_contentScope.words;
    return splitContentQuery(searchText(), m_regexMode).words;
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
    if (!place || place->status != ContentScope::Status::NotFound)
        return {};
    return place->folders.value(0, place->typed);
}

void Launcher::chooseFolderChip(const QString& key)
{
    if (key.startsWith(kTypePrefix)) { // a kind it is narrowed to: off
        QList<int> types = m_contentTypes;
        types.removeAll(fileTypeIndex(key.sliced(kTypePrefix.size())));
        setContentTypes(std::move(types));
        return;
    }
    if (key.isEmpty() && !m_contentTypes.isEmpty()) { // 不限: in all files, of any kind
        m_contentTypes.clear();
        emit fileTypeChanged();
    }
    chooseFolder(key);
}

void Launcher::chooseFolder(const QString& key)
{
    if (key == kAllChip) {
        m_contentPick = {};
    } else if (key.startsWith(kPickPrefix)) {
        const ContentQuery typed = splitContentQuery(searchText(), m_regexMode);
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
        chooseScope(Content); // by a chip of 内容's strip: the user's own
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
        if (key != kMissingChip && key != kTypedChip && !key.startsWith(kTypePrefix)) // the folders, not the kinds
            keys.append(key);
    }
    if (keys.size() < 2)
        return; // no folder to go to
    const auto count = keys.size();
    const qsizetype at = std::max<qsizetype>(0, keys.indexOf(folderChip()));
    chooseFolder(keys[((at + delta) % count + count) % count]);
}

bool Launcher::clearContentWords()
{
    if (m_scope != Content || m_contentFolder.isEmpty() || searchText().isEmpty())
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
    disarmPower();
    emit escaped();
    emit dismissRequested();
}

// What a row shows in the preview: a file or folder of its own (not an app,
// a place or a web page; not one gone or out of reach); a plugin's row, the
// plain file it opens, or what the plugin tells of it (GitHub #202).
std::optional<FilePreview::Item> Launcher::previewItem(const SearchResult* r, bool ask)
{
    // With the words searched for, marked in a text file's lines (GitHub
    // #291): those typed (a plugin's list: after its keyword), and in 内容
    // the words looked for in the files. Not a regular expression's text.
    const auto item = [this](FilePreview::Item item) {
        if (!regexTerms()) {
            item.words = ws::parseQuery(m_keyword ? m_keyword->words : searchText()).highlights;
            const QString needle = m_scope == Content || m_withContent ? contentNeedle() : QString();
            for (const QString& word : needle.split(u' ', Qt::SkipEmptyParts)) {
                if (!item.words.contains(word, Qt::CaseInsensitive))
                    item.words.append(word);
            }
        }
        return item;
    };
    if (r && r->clipId > 0 && !r->target.isEmpty()) // a screenshot: its picture
        return FilePreview::Item {r->target, r->name, false};
    if (r && r->isPlugin()) {
        if (const auto details = pluginDetails(*r, ask)) {
            if (!details->file.isEmpty())
                return item({details->file, r->name, false});
            return item({r->path, r->name, false, details->text, details->message});
        }
        const QString file = plugin::rowFile(*r->plugin);
        if (file.isEmpty())
            return std::nullopt;
        return item({file, file.mid(file.lastIndexOf(u'\\') + 1), false});
    }
    if (!r || r->isApp() || r->missing || r->offline || r->path.isEmpty())
        return std::nullopt;
    return item({r->path, r->name, r->isDir});
}

// The words a plugin's row is about, for its actions and details: those
// after the keyword in a command's own list, none for the rows of an instant
// command whose keyword was typed alone, else the text typed.
QString Launcher::pluginWords(const SearchResult& row) const
{
    if (m_keyword)
        return m_keyword->words;
    return m_merge && m_merge->keywordAlone(row) ? QString() : searchText();
}

QString Launcher::detailsKey(const SearchResult& row) const
{
    const QString words = pluginWords(row); // as an action's
    return row.plugin->pluginId + u'\n' + QString::fromUtf8(QJsonDocument(row.plugin->item).toJson(QJsonDocument::Compact))
        + u'\n' + words;
}

std::optional<Launcher::RowDetails> Launcher::pluginDetails(const SearchResult& row, bool ask)
{
    if (!row.isPlugin() || !plugin::asksDetails(*row.plugin))
        return std::nullopt;
    const QString key = detailsKey(row);
    if (const RowDetails* known = m_details.object(key))
        return *known;
    if (key == m_detailsFailed)
        return RowDetails {{}, {}, false, m_detailsFailedWhy, false};
    if (ask) {
        m_detailsFailed.clear(); // another row: the failed one is asked again when it comes back
        if (m_detailsWant.key != key && m_detailsAsking != key) {
            m_detailsWant = {key, row.plugin->pluginId, row.plugin->item, pluginWords(row)};
            if (m_detailsAsking.isEmpty())
                m_detailsTimer.start();
        }
    }
    return RowDetails {{}, {}, false, tr("正在向插件获取详情…"), true};
}

void Launcher::askDetails()
{
    if (m_detailsWant.key.isEmpty() || !m_detailsAsking.isEmpty())
        return;
    const DetailsWant want = std::exchange(m_detailsWant, {});
    // Off, or waiting for the user's word on more permissions: not asked.
    if (!m_plugins || !m_plugins->catalog().enabled(want.pluginId)) {
        detailsAnswered(want.key, false, {});
        return;
    }
    m_detailsAsking = want.key;
    // Its timeoutMs at most; not running (or a once plugin's): failed at once.
    m_plugins->request(want.pluginId, u"details"_s, {{u"item"_s, want.item}, {u"text"_s, want.text}},
        [self = QPointer<Launcher>(this), key = want.key, dataDir = m_plugins->dataDir(want.pluginId)](bool ok, const QJsonValue& result) {
            if (self)
                self->detailsAnswered(key, ok, ok ? plugin::parseDetails(result, dataDir) : plugin::DetailsAnswer {});
        });
}

void Launcher::detailsAnswered(const QString& key, bool ok, const plugin::DetailsAnswer& answer)
{
    if (m_detailsAsking == key)
        m_detailsAsking.clear();
    if (answer.kind == plugin::DetailsAnswer::Kind::None) {
        m_detailsFailed = key;
        m_detailsFailedWhy = ok ? tr("插件没有给出可显示的详情") : tr("无法获取详情：插件没有回答");
    } else {
        auto* kept = new RowDetails;
        kept->file = answer.file;
        if (answer.kind == plugin::DetailsAnswer::Kind::Text)
            kept->text = std::make_shared<const QString>(answer.text);
        kept->cut = answer.cut;
        m_details.insert(key, kept, static_cast<int>(answer.text.size() / 512 + 1));
    }
    // What shows it (the row may be another by now: then that one's turn).
    syncContentPane();
    followPreview();
    if (!m_detailsWant.key.isEmpty())
        m_detailsTimer.start();
}

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
            const std::optional<FilePreview::Item> shown
                = r && (!r->isApp() || (r->isPlugin() && r->clipId == 0)) ? previewItem(r, false) : std::nullopt;
            if (shown && shown->path.compare(path, Qt::CaseInsensitive) == 0) {
                trigger(row, Open);
                return;
            }
        }
    });
}

void Launcher::togglePreview(int row)
{
    dropServerWait(); // a server still being waited for no longer matters (Space, Alt+P: not through trigger)
    if (!m_preview)
        return;
    const SearchResult* r = m_results.at(row);
    const std::optional<FilePreview::Item> item = previewItem(r);
    m_preview->forgetClosedExternal(); // QuickLook or Seer closed on its own meanwhile
    // The panel's details: their 预览 tab, and back to the tab it came from.
    if (detailsShown()) {
        if (m_contentPane->tab() == ContentPane::PreviewTab) {
            m_contentPane->setTab(m_tabBeforePreview);
        } else {
            m_tabBeforePreview = m_contentPane->tab();
            m_contentPane->setTab(ContentPane::PreviewTab);
        }
        return;
    }
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

void Launcher::previewRow(int row)
{
    if (!m_preview)
        return;
    if (detailsShown()) { // the panel's details: their 预览 tab
        if (m_contentPane->tab() != ContentPane::PreviewTab)
            togglePreview(row);
        return;
    }
    const std::optional<FilePreview::Item> item = previewItem(m_results.at(row));
    if (!item)
        return;
    m_preview->forgetClosedExternal(); // QuickLook or Seer closed on its own meanwhile
    if (m_preview->isOpen() && m_preview->isShowing(item->path))
        return;
    togglePreview(row); // opens it, or (open on another file) shows this one
}

void Launcher::previewHover(int row)
{
    if (m_previewFollowsPointer && m_preview && m_preview->isOpen() && row >= 0 && row < m_results.count())
        m_preview->follow(previewItem(m_results.at(row)));
}

void Launcher::previewPointerMoved()
{
    if (m_previewFollowsPointer && m_preview)
        m_preview->postponeHandler();
}

bool Launcher::copyPreviewSelection()
{
    const QString text = m_preview && m_preview->isOpen() ? m_preview->selectedText()
        : detailsShown() && m_contentPane->tab() == ContentPane::PreviewTab ? m_contentPane->filePreview()->selectedText()
                                                                           : QString();
    if (text.isEmpty())
        return false;
    shell::copyText(text);
    return true;
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
    // In the panel's details their 预览 tab is the preview: the window waits
    // until they go (handOverPreview).
    if (detailsShown()) {
        if (m_previewWanted && std::exchange(m_previewResume, false))
            m_previewHandedOver = true;
        return;
    }
    // Left open last time: back with the launcher, on the first row that
    // has a file to show. Ours only: QuickLook's or Seer's window popping up
    // while one types is in the way (GitHub #131); Alt+P brings it.
    if (m_previewWanted && m_previewResume) {
        if (m_preview->showsElsewhere())
            m_previewResume = false;
        else if (const std::optional<FilePreview::Item> item = previewItem(m_results.at(m_currentRow))) {
            m_previewResume = false;
            m_preview->show(*item);
        }
    }
}

bool Launcher::detailsShown() const
{
    return m_panel && m_scope == Content && m_contentPane && m_contentPane->opened();
}

void Launcher::handOverPreview()
{
    if (!m_preview)
        return;
    if (detailsShown()) {
        // In: the window goes, what it showed is on the 预览 tab now.
        m_preview->forgetClosedExternal();
        if (!m_preview->isOpen())
            return;
        m_preview->close(); // not by the user: still wanted (m_previewWanted)
        m_previewHandedOver = true;
        if (m_contentPane->tab() != ContentPane::PreviewTab) {
            m_tabBeforePreview = m_contentPane->tab();
            m_contentPane->setTab(ContentPane::PreviewTab);
        }
        return;
    }
    // Out: the window again, on the current row, if the tab still showed
    // it (left for another tab, the preview was put away). Ours only, as when
    // the launcher opens (followPreview): QuickLook or Seer popping up is in the way.
    if (!std::exchange(m_previewHandedOver, false) || m_contentPane->tab() != ContentPane::PreviewTab || !m_window
        || !m_window->isVisible() || m_preview->showsElsewhere())
        return;
    if (const std::optional<FilePreview::Item> item = previewItem(m_results.at(m_currentRow)))
        m_preview->show(*item);
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
    dropServerWait(); // a server still being waited for no longer matters: this is asked now
    if (row == m_currentRow && m_currentGone.isValid() && m_currentGone.elapsed() < 800) {
        // Its row went a moment ago (a plugin's list changed by itself): the
        // key was meant for that one, not for what took its place.
        flash(tr("这一行刚刚变了，看清楚再按一次"));
        return;
    }
    if (action == SystemMenu) { // takes the selection itself, from Win顺's menu (Main.qml says where)
        showSystemMenu(row);
        return;
    }
    if (action >= PluginFileAction) { // one request for the whole selection, not one per row
        runFileCommand(row, action - PluginFileAction);
        return;
    }
    if (action == Preview) { // the row it was asked on, not a selection
        togglePreview(row);
        return;
    }
    // A terminal opens for the row it was asked on, not for each of a selection.
    if (action != OpenTerminal && action != OpenTerminalAdmin && m_results.isSelected(row) && m_results.selectedCount() > 1) {
        performMany(m_results.selection(), static_cast<Action>(action));
        return;
    }
    if (const SearchResult* r = m_results.at(row))
        perform(*r, static_cast<Action>(action));
}

void Launcher::clickRow(int row, bool onSwitch)
{
    if (rowJustChanged(row)) { // another row under the pointer a moment ago
        flash(tr("这一行刚刚变了，看清楚再点一次"));
        return;
    }
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
    if (action >= PluginFileAction) {
        if (m_results.selectedCount() > 0)
            runFileCommand(m_results.indexOf(m_results.selection().first().path), action - PluginFileAction);
        return;
    }
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
    dropServerWait(); // a server still being waited for no longer matters
    const SearchResults items = m_results.isSelected(row) && m_results.selectedCount() > 1 ? m_results.selection()
                                                                                         : SearchResults {*at};
    // A share that does not answer would hold the shell up for long: each
    // server asked by its first file, all together (dragAnswers).
    QHash<QString, QString> asker; // serverKey: the file it is asked by
    QStringList asked;
    for (const SearchResult& r : items) {
        if (!r.isApp() && !r.offline && onServer(r.path) && !asker.contains(serverKey(r.path))) {
            asker.insert(serverKey(r.path), r.path);
            asked.append(r.path);
        }
    }
    const QSet<QString> answering = dragAnswers(asked);
    QStringList paths;
    QString away; // the first that did not answer
    for (const SearchResult& r : items) {
        if (r.isApp() || r.offline)
            continue;
        bool answers = true;
        if (onServer(r.path)) {
            answers = answering.contains(asker.value(serverKey(r.path)));
            if (!answers && away.isEmpty())
                away = serverName(r.path);
        } else {
            answers = serverAnswers(r.path); // WSL: its state, without asking
        }
        if (answers)
            paths.append(r.path);
    }
    if (paths.isEmpty() && !away.isEmpty())
        flash(tr("连不上 %1（离线或不在同一网络）").arg(away));
    if (paths.isEmpty() || !m_window)
        return;
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    const DragPicture picture = dragPicture(paths, m_window->devicePixelRatio());
    dragout::drag(paths, hwnd, true, picture.image, picture.hotspot);
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

void Launcher::perform(const SearchResult& result, Action action, bool fileChecked)
{
    const SearchResult r = result; // the model may change underneath us
    m_adminNote.reset(); // set below for the row being run, taken by remember()
    // A path found for a program's words (GitHub #344): Enter hands it over.
    if (action == Open && m_keyword && !m_keyword->suggest.isEmpty() && !r.isPlugin() && fileRow(r) && runWithPath(r))
        return;
    // A file or folder found for a command (commandFiles): Enter hands it over.
    if (const auto files = action == Open && fileRow(r) ? commandFiles() : std::nullopt) {
        const Favorite& command = files->command;
        remember(command.path, m_shownText);
        shell::runCustom(command.command, command.workingDirectory, r.path, reportFailure(command.name),
            {command.silent, command.runAsAdmin});
        emit dismissRequested();
        return;
    }
    if (!fileChecked)
        dropServerWait(); // a server still being waited for no longer matters
    if (r.isWindow()) {
        if (action == Open || action == Alternate) {
            switch (windowsearch::activate(r)) {
            case windowsearch::Activation::Switched:
                rememberWindow(r);
                emit dismissRequested();
                break;
            case windowsearch::Activation::Gone:
                flash(tr("窗口已关闭或内容已改变，请重新搜索"));
                break;
            case windowsearch::Activation::Refused:
                flash(tr("无法切换到此窗口，请从任务栏打开"));
                break;
            }
        } else if (action == CopyItem || action == CopyName || action == CopyPath) {
            shell::copyText(r.name);
            flash(tr("已复制名称"));
        }
        return;
    }
    if (action == EditWebShortcuts) {
        emit webSettingsRequested();
        return;
    }
    if (action == AddFavorite || action == RemoveFavorite || action == FavoriteFolder || action == EditFavorite) {
        performFavorite(r, action);
        return;
    }
    if (action == BrowseFolder) {
        browseFolder(r);
        return;
    }
    if (r.clipId > 0) {
        performScreenshot(r, action);
        return;
    }
    if (action == PinPicture) {
        if (!r.isApp() && !r.isDir && m_pictureVerbs.pin) {
            emit pinRequested(r.path, false);
            emit dismissRequested();
        }
        return;
    }
    if (action == EditPicture) {
        if (!r.isApp() && !r.isDir && m_pictureVerbs.edit && isPinnablePicture(r.path)) {
            emit dismissRequested(); // first: the editing window takes the front
            emit editPictureRequested(r.path);
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
        performPlace(r, action == RunAsUser ? Open : action);
        return;
    }
    // 记住以管理员身份运行 (GitHub #313): Enter runs as it was last run from
    // here. remember() keeps which with the row. Not on the second pass for
    // a file checked meanwhile: `action` is already what it became.
    if (action == RunAsUser) {
        action = Open;
        if (canRememberAdmin(r))
            m_adminNote = false;
    } else if (action == RunAsAdmin && canRememberAdmin(r)) {
        m_adminNote = true;
    } else if (!fileChecked && action == Open && adminRemembered(r)) {
        action = RunAsAdmin;
    }
    if (r.isApp()) {
        performApp(r, action);
        return;
    }
    const bool needsFile
        = action == Open || action == Reveal || action == RunAsAdmin || action == Recycle
        || action == OpenTerminal || action == OpenTerminalAdmin;
    const std::wstring sourcePath = r.path.toStdWString();
    const auto distro = unc::wslDistribution(sourcePath);
    if (needsFile && !distro.empty() && !fileChecked) {
        const QString key = pathtext::pathKey(r.path);
        if (m_wslOpening.contains(key))
            return;
        m_wslOpening.insert(key);
        // The user may just have started WSL elsewhere. Refresh and check the
        // actual file off the UI thread, including a slow first access to 9P.
        flash(tr("正在连接 WSL…"));
        std::thread([guard = QPointer(this), r, action, key] {
            const auto state = wsl::probe(r.path.toStdWString(), true);
            const bool exists = state == wsl::State::Running && QFileInfo::exists(r.path);
            QMetaObject::invokeMethod(qApp, [guard, r, action, key, state, exists] {
                if (!guard)
                    return;
                guard->m_wslOpening.remove(key);
                if (state != wsl::State::Running) {
                    const std::wstring path = r.path.toStdWString();
                    const auto name = unc::wslDistribution(path);
                    guard->flash(state == wsl::State::Stopped
                        ? tr("WSL：请先启动 %1，再重试；也可以在设置的“网络与 WSL”中启动")
                            .arg(QString::fromStdWString(std::wstring(name)))
                        : tr("暂时无法连接这个 WSL 发行版，请在终端检查后重试"));
                } else if (!exists) {
                    guard->flash(tr("“%1” 已不存在，或当前 Linux 用户没有访问权限").arg(r.name));
                } else {
                    guard->perform(r, action, true);
                }
            });
        }).detach();
        return;
    }
    if (needsFile && !fileChecked && onServer(r.path)) {
        // Never asked on the UI thread (GitHub #116): an online server
        // answers in milliseconds, but one gone away holds Windows up for
        // half a minute (even seconds after it last answered), and a NAS just
        // started takes seconds. The probe and Windows at once; the action
        // once they answered.
        checkServersThen({r.path}, [this, r, action](const ServerFound& found) {
            switch (found.value(r.path, share::Reach::Unreachable)) {
            case share::Reach::There:
                perform(r, action, true);
                break;
            case share::Reach::Missing:
                forgetMissing(r, action);
                break;
            case share::Reach::ShareAway: // nothing forgotten: it comes back
            case share::Reach::NoService:
            case share::Reach::Unreachable:
                flash(whyNot({r.path}, found));
                break;
            }
        });
        return;
    }
    if (needsFile && r.offline && !distro.empty())
        m_index->shares().networkChanged(); // WSL answers again: its share is back
    if (action == SearchInFolder) {
        if (!r.isDir)
            return;
        // 内容 there, the box emptied for what to find in it.
        m_contentFolder = r.path;
        m_contentPick = {};
        m_results.setSelection({});
        if (!m_query.isEmpty()) {
            m_query.clear();
            updateQueryPrefix();
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
    if (needsFile && !fileChecked && !QFileInfo::exists(r.path)) {
        forgetMissing(r, action);
        return;
    }
    switch (action) {
    case Open:
    case RunAsAdmin:
        remember(r.path, m_shownText);
        // Opened by typing in Explorer, a folder goes to that tab (GitHub #39).
        if (action == Open && r.where == SearchResult::Where::Line && r.line > 0 && !r.isDir)
            contentopen::open(r.path, r.line, reportFailure(r.name)); // at the matching line, in an editor that can
        else if (action != Open || !r.isDir || !m_folderOpener || !m_folderOpener(r.path))
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
        copyFiles({r.path});
        flash(tr("已复制，可在资源管理器中粘贴"));
        break;
    case ForgetRecent:
        forgetRecent({r.path});
        break;
    case Recycle: {
        if (!isDeletablePath(r))
            break;
        recycleFiles({r.path}, [this, r](bool ok) { onRecycled(r, ok); });
        break;
    }
    case EditWebShortcuts:
    case AddFavorite:
    case RemoveFavorite:
    case OpenTerminal:
    case OpenTerminalAdmin: {
        const QString folder = r.isDir ? r.path : folderOf(r.path);
        const bool asAdmin = action == OpenTerminalAdmin;
        remember(r.path, m_shownText);
        if (!m_terminalOpener || !m_terminalOpener(folder, asAdmin))
            shell::openTerminalIn(m_terminal, m_terminalCommand, folder, reportFailure(r.name), asAdmin);
        emit dismissRequested();
        break;
    }
    case FavoriteFolder:
    case EditFavorite:
    case SystemMenu:
    case Rename:
        break; // not for a file, or done above (SystemMenu: trigger; Rename: Main.qml's box)
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
    case OpenTerminalAdmin:
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
    const bool own = ws::isOwnCommand(place.target); // Win顺's settings, or a plugin's (GitHub #389, #390)
    if (own && (action == Alternate || action == Reveal))
        action = Open; // where else would it go
    switch (action) {
    case Open:
    case RunAsAdmin:
        if (place.target == ws::kRestartElevatedCommand) { // Win顺's own: nothing for the shell
            emit dismissRequested();
            emit elevatedRestartRequested();
            return;
        }
        if (ws::isPowerCommand(place.target)) { // Win顺's own power/session commands (GitHub #428)
            const auto powerAction = power::parseAction(ws::powerCommandAction(place.target).toString());
            if (!powerAction) {
                flash(tr("暂不支持“%1”").arg(place.name));
                return;
            }
            if (ws::powerCommandNeedsConfirm(place.target) && !isPowerArmed(place.target)) {
                m_armedPowerCommand = place.target;
                m_armedPowerFlash = tr("再按一次 Enter 或再点一次确认%1 · Esc 取消").arg(place.name);
                m_armedPowerTimer.start(5s);
                flash(m_armedPowerFlash, 5s);
                refreshKeyHints();
                return;
            }
            disarmPower();
            emit dismissRequested();
            power::execute(*powerAction, [self = QPointer(this), name = place.name](const QString& error) {
                if (!error.isEmpty() && self)
                    emit self->openFailed(u"%1（%2）"_s.arg(name, error));
            });
            return;
        }
        remember(place.path, m_shownText); // the next time the same words put it first
        if (own) {
            if (place.target == ws::kSettingsCommand)
                openSettings();
            else
                emit pluginSettingsRequested(place.target.mid(ws::kPluginSettingsCommand.size()));
            emit dismissRequested();
            return;
        }
        if (place.isCommand())
            shell::runCustom(place.target, place.workingDirectory, place.words, reportFailure(place.name),
                {place.silent, place.runAsAdmin || action == RunAsAdmin});
        else
            shell::run(place.target, reportFailure(place.name));
        emit dismissRequested();
        break;
    case Reveal:
        flash(tr("“%1”没有可以打开的位置").arg(place.name));
        break;
    case CopyItem:
    case CopyPath:
        if (place.target == ws::kRestartElevatedCommand || ws::isPowerCommand(place.target)) {
            flash(tr("“%1”没有可复制的命令").arg(place.name));
            break;
        }
        if (own) { // as a winshun:// link opens it (WinShunUri)
            shell::copyText(u"winshun://"_s + place.target.mid(place.target.indexOf(u':') + 1));
            flash(tr("已复制链接"));
            break;
        }
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
    case OpenTerminalAdmin:
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
        qInfo().noquote() << (dryRun ? u"Web: would open %1%2%3"_s : u"Web: open %1%2%3"_s)
                                 .arg(web.target, web.suggestion ? u" (a suggestion)"_s : QString(),
                                     m_webBrowser.isEmpty() ? QString() : u" in "_s + m_webBrowser);
        if (!dryRun)
            openWebAddress(web.target);
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
    case OpenTerminalAdmin:
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
void Launcher::performMany(const SearchResults& items, Action action, const ServerFound* found)
{
    if (!found)
        dropServerWait(); // a server still being waited for no longer matters
    if (action == Alternate)
        action = Open;
    // Not for copying, as for one row: the files go to the clipboard as they
    // are, and Explorer finds out on pasting.
    if (!found && (action == Open || action == RunAsAdmin || action == Reveal || action == Recycle)) {
        // Files on servers asked about off the UI thread first, each server
        // once (see perform); the action once they answered.
        QStringList network;
        QSet<QString> seen;
        bool elsewhere = false; // some not on a server: what is done with them says so
        for (const SearchResult& r : items) {
            // What the action looks up for it: an app's target where that is
            // shown (its shortcut starts it as it is).
            const QString path = !r.isApp() ? r.path : action == Reveal ? r.target : QString();
            if (!onServer(path))
                elsewhere = true;
            else if (!seen.contains(path)) {
                seen.insert(path);
                network.append(path);
            }
        }
        if (!network.isEmpty()) {
            checkServersThen(network, [this, items, action, network, elsewhere](const ServerFound& found) {
                performMany(items, action, &found);
                // Why nothing was done, when it was all on servers; else what was done says so.
                const bool anyThere
                    = std::any_of(found.cbegin(), found.cend(), [](share::Reach r) { return r == share::Reach::There; });
                if (const QString why = whyNot(network, found); !anyThere && !elsewhere && !why.isEmpty())
                    flash(why);
            });
            return;
        }
    }
    // Files on SMB and WebDAV servers were looked up just now (`found`);
    // WSL's go by its state, unasked.
    const auto exists = [found](const QString& path) {
        if (onServer(path))
            return found && found->value(path, share::Reach::Unreachable) == share::Reach::There;
        return serverAnswers(path) && QFileInfo::exists(path);
    };
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
            if (r.isWindow()) {
                if (!admin && windowsearch::activate(r) == windowsearch::Activation::Switched) {
                    rememberWindow(r);
                    ++started;
                }
                continue;
            }
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
                openWebAddress(r.target);
                ++started;
                continue; // not remembered (see performWeb)
            }
            if (r.isPlace()) {
                if (admin || drawsSwitch(r) || ws::isPowerCommand(r.target) || r.target == ws::kRestartElevatedCommand)
                    continue; // switches and power/elevation actions are never batch-opened with others
                if (r.target == ws::kSettingsCommand) // Win顺's own (GitHub #389, #390): never the shell
                    openSettings();
                else if (r.target.startsWith(ws::kPluginSettingsCommand))
                    emit pluginSettingsRequested(r.target.mid(ws::kPluginSettingsCommand.size()));
                else
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
            if (r.isWindow() || r.isPlace() || place.isEmpty() || !exists(place))
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
            if (!file.isEmpty() && (onServer(file) || exists(file))) // on a server: not asked (see above)
                files.append(file);
        }
        if (files.isEmpty()) {
            flash(tr("选中的项目没有可以复制的文件"));
            return;
        }
        copyFiles(files);
        flash(tr("已复制 %Ln 项，可在资源管理器中粘贴", nullptr, static_cast<int>(files.size())));
        break;
    }
    case CopyPath: {
        QStringList paths;
        for (const SearchResult& r : items) {
            if (r.isWindow())
                paths.append(r.name);
            else if (r.isPlugin())
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
        QSet<QString> seen;
        for (const SearchResult& r : items) {
            if (!isDeletablePath(r))
                continue;
            const QString key = pathtext::pathKey(r.path);
            if (seen.contains(key))
                continue;
            seen.insert(key);
            if (!exists(r.path)) {
                // Gone: out of the list. One on a server is not asked again
                // here (it may hold the window up): out when its server said
                // so; kept when it did not answer, or its share is away.
                if (!onServer(r.path))
                    forgetRecycled(r);
                else if (found && found->value(r.path, share::Reach::Unreachable) == share::Reach::Missing)
                    forgetRow(r);
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
        recycleFiles(paths, [this, files](bool ok) { onRecycledMany(files, ok); });
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
    m_queryRecorded = true;
    m_queryHistory.resetNavigation();
    emit queryHistoryChanged();
    emit historyChanged();
    refreshFrequentApps();
    if (m_window && m_window->isVisible())
        search(); // "最近" rows come or go
    else
        m_shownStale = true;
}

bool Launcher::canClearHistory() const
{
    return m_recordHistory && (!m_history->items().isEmpty() || !m_queryHistory.items().isEmpty());
}

void Launcher::clearHistory()
{
    m_history->clear();
    m_queryHistory.clear();
    m_queryRecorded = true;
    m_historyDirty = true;
    emit queryHistoryChanged();
    emit historyChanged();
    refreshFrequentApps();
    m_results.setSelection({});
    search(); // the list of recent ones empties, "最近" goes from the rest
    flash(tr("已清除搜索和最近使用记录"));
}

void Launcher::remember(const QString& path, const QString& query)
{
    rememberQuery();
    if (m_recordHistory) {
        m_history->record(path, query, QDateTime::currentMSecsSinceEpoch());
        m_historyDirty = true;
    }
    if (m_recordHistory && m_adminNote)
        m_history->setAdmin(path, *m_adminNote);
    m_adminNote.reset();
}

void Launcher::setRememberAdmin(bool on)
{
    if (m_rememberAdmin == on)
        return;
    m_rememberAdmin = on;
    if (m_window && m_window->isVisible())
        search(); // 管理员 comes or goes
    else
        m_shownStale = true;
}

bool Launcher::canRememberAdmin(const SearchResult& r) const
{
    if (!m_rememberAdmin || !m_recordHistory || r.missing || r.offline || r.isPlace() || r.isWeb() || r.isPlugin() || r.isWindow())
        return false;
    return r.isApp() ? r.elevatable : !r.isDir && shell::canRunAsAdministrator(r.path);
}

bool Launcher::adminRemembered(const SearchResult& r) const
{
    return m_rememberAdmin && m_recordHistory && m_history->admin(r.path) && canRememberAdmin(r);
}

// A window switched to that shows a folder (Explorer, Total Commander, Opus:
// filemanager::frontFolder): the folder is what the user went to, remembered
// as one opened from here is, so it is among the recent ones next time
// (GitHub #231). Other windows leave nothing: a program's window is not a
// file to list.
void Launcher::rememberWindow(const SearchResult& window)
{
    if (!m_recordHistory || !window.windowHandle)
        return;
    const std::wstring folder = filemanager::frontFolder(reinterpret_cast<HWND>(window.windowHandle));
    if (folder.empty())
        return;
    const QString path = QDir::toNativeSeparators(QString::fromStdWString(folder));
    if (QFileInfo(path).isDir())
        remember(path, m_shownText);
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
            if (!self)
                return;
            self->m_apps->checkPrograms(); // an app whose program is gone goes from the list (#207)
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

// A file or folder of the user's that is there now: not an app, a place, a
// web page, a plugin's or the clipboard's row, nor a drive or a share itself.
static bool renamable(const SearchResult& r)
{
    return recyclable(r);
}

bool Launcher::beginRename(int row)
{
    m_renaming.reset();
    const SearchResult* r = m_results.at(row);
    if (!r || !renamable(*r) || m_results.selectedCount() > 1)
        return false;
    m_renaming = *r;
    return true;
}

int Launcher::renameStem() const
{
    return m_renaming ? static_cast<int>(renamecheck::stemLength(m_renaming->name, m_renaming->isDir)) : 0;
}

bool Launcher::finishRename(const QString& typed)
{
    if (!m_renaming)
        return true;
    using renamecheck::Problem;
    const SearchResult was = *m_renaming;
    const QString name = renamecheck::cleaned(typed);
    switch (renamecheck::problem(was.name, typed)) {
    case Problem::None: break;
    case Problem::Same:
        m_renaming.reset();
        return true; // nothing to do
    case Problem::Empty:
        flash(tr("名字不能是空的"));
        return false;
    case Problem::Character:
        flash(tr("名字里不能有 \\ / : * ? \" < > |"));
        return false;
    case Problem::Reserved:
        flash(tr("“%1”是 Windows 保留的名字，换一个").arg(name));
        return false;
    case Problem::Ending:
        flash(tr("名字不能以句点结尾"));
        return false;
    case Problem::Long:
        flash(tr("名字太长了"));
        return false;
    }
    const QString folder = was.path.left(was.path.lastIndexOf(u'\\') + 1);
    // Another one of that name there (not this one in another case).
    if (name.compare(was.name, Qt::CaseInsensitive) != 0 && QFileInfo::exists(folder + name)) {
        flash(tr("这里已经有“%1”了，换一个名字").arg(name));
        return false;
    }
    m_renaming.reset();
    const auto owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    ++m_shellOpsInFlight;
    shell::rename(was.path, name, owner, [self = QPointer(this), was, name](bool ok) {
        QMetaObject::invokeMethod(qApp, [self, was, name, ok] {
            if (self) {
                self->m_shellOpsInFlight = std::max(0, self->m_shellOpsInFlight - 1);
                self->onRenamed(was, name, ok);
            }
        }, Qt::QueuedConnection);
    });
    return true;
}

// The row takes the new name where it is (the index hears of it from the
// disk a moment later); a recent or pinned entry of the old path goes with it.
void Launcher::onRenamed(const SearchResult& was, const QString& newName, bool ok)
{
    const QString path = was.path.left(was.path.lastIndexOf(u'\\') + 1) + newName;
    if (!ok || !QFileInfo::exists(path)) { // cancelled, in use, access denied…
        flash(tr("没有重命名“%1”").arg(was.name));
        return;
    }
    for (int row = 0; row < m_results.count(); ++row) {
        const SearchResult* r = m_results.at(row);
        if (r->path.compare(was.path, Qt::CaseInsensitive) != 0)
            continue;
        SearchResult renamed = *r;
        renamed.name = newName;
        renamed.path = path;
        m_results.update(row, renamed);
        break;
    }
    flash(tr("已重命名为“%1”").arg(newName));
}

bool Launcher::forgetRecycled(const SearchResult& result)
{
    if (QFileInfo::exists(result.path))
        return false;
    forgetRow(result);
    return true;
}

void Launcher::forgetRow(const SearchResult& result)
{
    m_history->remove(result.path);
    emit historyChanged();
    // Content search can list multiple line matches for the same file path.
    for (int rowIndex = m_results.indexOf(result.path); rowIndex >= 0; rowIndex = m_results.indexOf(result.path)) {
        const SearchResult* row = m_results.at(rowIndex);
        const bool contentRow = row && row->line > 0;
        if (m_results.remove(result.path) && m_scope != Content) {
            if (contentRow)
                --m_contentHits;
            else if (m_totalMatches > 0)
                --m_totalMatches;
        }
    }
}

QVariantList Launcher::menuItems(int row) const
{
    m_menuFileCommands.clear(); // this menu's, filled by fileCommandItems
    const SearchResult* r = m_results.at(row);
    if (!r)
        return {};
    const auto entry = [](Action action, const QString& text, const QString& shortcut, const QString& glyph,
                           bool danger = false) {
        QVariantMap map {
            {u"action"_s, static_cast<int>(action)},
            {u"text"_s, text},
            {u"shortcut"_s, shortcut},
            {u"glyph"_s, glyph},
        };
        if (danger)
            map.insert(u"danger"_s, true);
        return map;
    };
    const QVariantMap separator {{u"separator"_s, true}};
    // The plugins' `files` commands (用局域网发送… and the like) for the row's
    // file or folder, or the whole selection: a group of their own.
    const auto fileCommandItems = [&](QVariantList& into) {
        QStringList paths;
        QList<bool> folders;
        if (!m_plugins || !fileCommandPaths(row, &paths, &folders))
            return;
        const QString locale = uiLocale();
        const std::vector<plugin::Catalog::FileCommand> commands = m_plugins->catalog().fileCommands(paths, folders);
        for (std::size_t i = 0; i < commands.size() && i < 20; ++i) {
            const plugin::Catalog::FileCommand& c = commands[i];
            QString text = c.trigger->title.pick(locale);
            if (text.isEmpty())
                text = c.command->title.pick(locale);
            if (text.isEmpty())
                text = c.entry->manifest->name.pick(locale);
            // A glyph of the command's (or the plugin's) icon, else the puzzle.
            const QString icon = !c.command->icon.isEmpty() ? c.command->icon : c.entry->manifest->icon;
            bool ok = false;
            const uint code = icon.startsWith(u"glyph:U+", Qt::CaseInsensitive) ? icon.mid(8).toUInt(&ok, 16) : 0;
            // A named Phosphor icon stays a spec: the tray menu resolves it
            // when it draws the row (see phosphor::resolve).
            // A name Phosphor does not have is the puzzle, as a glyph not there is.
            const QString named = icon.toLower();
            const QString glyph = (named.startsWith(u"ph:"_s) || named.startsWith(u"ph-fill:"_s)) && phosphor::resolve(named)
                ? named
                : QString(QChar(static_cast<char16_t>(ok && code >= 0xE000 && code <= 0xF8FF ? code : 0xEA86)));
            if (i == 0)
                into.append(separator);
            into.append(entry(static_cast<Action>(PluginFileAction + static_cast<int>(m_menuFileCommands.size())), text, QString(),
                glyph));
            m_menuFileCommands.push_back({c.entry->id, c.command->id, paths});
        }
    };

    if (m_results.isSelected(row) && m_results.selectedCount() > 1) { // for the whole selection
        const SearchResults selection = m_results.selection();
        const auto any = [&](auto test) { return std::any_of(selection.cbegin(), selection.cend(), test); };
        QVariantList items;
        items.append(entry(Open, tr("打开 %Ln 项", nullptr, static_cast<int>(selection.size())), u"Enter"_s, u"\uE8E5"_s));
        if (any([](const SearchResult& r) { return !r.isApp() || (!r.isWindow() && !r.isPlace() && !r.target.isEmpty()); }))
            items.append(entry(Reveal, tr("打开所在位置"), u"Ctrl+Enter"_s, u"\uE8DA"_s));
        fileCommandItems(items);
        items.append(separator);
        if (any([](const SearchResult& r) { return !r.isApp() || r.hasCopyableTarget(); }))
            items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
        items.append(entry(CopyPath, any([](const SearchResult& r) { return r.isWindow(); })
            ? tr("复制路径或窗口标题") : tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
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
        QSet<QString> deletablePaths;
        for (const SearchResult& s : selection) {
            if (recyclable(s))
                deletablePaths.insert(pathtext::pathKey(s.path));
        }
        if (!deletablePaths.isEmpty()) {
            items.append(separator);
            items.append(entry(Recycle, tr("移到回收站（%Ln 项）", nullptr, static_cast<int>(deletablePaths.size())),
                QString(), u"\uE74D"_s, true)); // Delete
        }
        if (any([](const SearchResult& r) { return !r.isApp() && !r.missing && !r.offline; })) {
            items.append(separator);
            items.append(entry(SystemMenu, tr("更多选项"), m_defaultSystemMenu ? tr("右键") : tr("Shift+右键"), u"\uE712"_s)); // More
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
    if (r->isWindow()) {
        items.append(entry(Open, tr("切换到窗口"), u"Enter"_s, u"\uE8A7"_s));
        items.append(entry(CopyName, tr("复制窗口标题"), u"Ctrl+C"_s, u"\uE8AC"_s));
        return items;
    }
    if (r->clipId > 0) { // a screenshot of the history
        items.append(entry(Open, tr("复制并粘贴"), u"Enter"_s, u"\uE77F"_s)); // Paste
        if (m_pictureVerbs.pin)
            items.append(entry(PinPicture, tr("贴到屏幕"), u"Ctrl+Enter"_s, u"\uE718"_s)); // Pin
        if (m_pictureVerbs.edit)
            items.append(entry(EditPicture, tr("编辑"), u"Shift+Enter"_s, u"\uE70F"_s)); // Edit
        items.append(entry(Preview, tr("预览"), m_previewHotkey, u"\uE890"_s)); // View
        items.append(separator);
        items.append(entry(CopyItem, tr("复制图片"), u"Ctrl+C"_s, u"\uE8C8"_s));
        if (r->plugin && !r->plugin->copy.isEmpty())
            items.append(entry(CopyName, tr("复制识别出的文字"), QString(), u"\uE8AC"_s));
        items.append(separator);
        items.append(entry(RemoveScreenshot, tr("从历史删除"), tr("Delete"), u"\uE74D"_s, true)); // Delete
        return items;
    }
    if (r->isPlugin()) {
        // Its actions as the plugin named them (Enter, Ctrl+Enter, Shift+Enter
        // in that order), else what its default does; then copying its text.
        const PluginRow& p = *r->plugin;
        static constexpr QStringView kKeys[] {u"Enter", u"Ctrl+Enter", u"Shift+Enter"};
        if (p.valid) {
            // A row that drills (setQuery/searchQuery) keeps that as its Enter
            // even when it also carries actions: the menu names it first.
            if (!p.setQuery.isEmpty() || !p.searchQuery.isEmpty()) {
                const QString text = !p.searchQuery.isEmpty() ? tr("搜索") : tr("填入搜索框");
                items.append(entry(Open, text, u"Enter"_s, u"\uE8A7"_s));
            }
            const bool drills = !p.setQuery.isEmpty() || !p.searchQuery.isEmpty(); // Enter is that row's, not its first action's
            for (int i = 0; i < p.actions.size(); ++i) {
                const QString key = !p.actions[i].key.isEmpty() ? p.actions[i].key
                    : i < 3 && !(drills && i == 0)              ? kKeys[i].toString()
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
    if (r->isPlace() && ws::isOwnCommand(r->target)) { // Win顺's settings, a plugin's: kept like any place (GitHub #389)
        items.append(entry(Open, tr("打开设置"), u"Enter"_s, u"\uE713"_s)); // Settings
        items.append(separator);
        // winshun://settings/plugin/<id>: for a desktop shortcut or a script (GitHub #390)
        items.append(entry(CopyPath, tr("复制链接"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        favoriteItems();
        if (r->recent) {
            items.append(separator);
            items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
        }
        return items;
    }
    if (r->isPlace() && ws::isPowerCommand(r->target)) { // Win顺's own power/session actions (GitHub #428)
        const QString label = isPowerArmed(r->target) ? tr("确认%1").arg(r->name)
                            : !ws::powerCommandNeedsConfirm(r->target) ? tr("锁定")
                            : tr("执行");
        items.append(entry(Open, label, u"Enter"_s, u"\uE7E8"_s)); // Power
        items.append(separator);
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
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
        // Enter runs it as administrator (GitHub #313): said so, with the way back.
        const bool admin = adminRemembered(*r);
        items.append(entry(Open, admin ? tr("以管理员身份运行") : tr("打开"), u"Enter"_s, admin ? u"\uE7EF"_s : u"\uE8A7"_s)); // Admin, OpenInNewWindow
        if (admin)
            items.append(entry(RunAsUser, tr("以普通权限运行"), QString(), u"\uE8A7"_s));
        else if (r->elevatable && !r->missing && !r->offline)
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
    const bool admin = adminRemembered(*r); // as an app's above
    items.append(entry(Open, r->isDir ? tr("打开文件夹") : admin ? tr("以管理员身份运行") : tr("打开"), u"Enter"_s,
        r->isDir ? u"\uE838"_s : admin ? u"\uE7EF"_s : u"\uE8E5"_s));
    items.append(entry(Reveal, tr("打开所在位置"), u"Ctrl+Enter"_s, u"\uE8DA"_s));
    if (navigation::canBrowseFolder(*r))
        items.append(entry(BrowseFolder, tr("进入此文件夹搜索"), QString(), u"\uE721"_s));
    if (!r->missing && !r->offline)
        items.append(entry(Preview, tr("预览"), m_previewHotkey, u"\uE890"_s)); // View
    // A picture file: on the screen, or in the editing window (as the clipboard history's).
    const bool picture = !r->isDir && !r->missing && !r->offline && isPinnablePicture(r->path);
    if (picture && m_pictureVerbs.pin)
        items.append(entry(PinPicture, tr("贴到屏幕"), QString(), u"\uE718"_s)); // Pin
    if (picture && m_pictureVerbs.edit)
        items.append(entry(EditPicture, tr("编辑"), QString(), u"\uE70F"_s)); // Edit
    items.append(entry(OpenTerminal, tr("在终端中打开"), QString(), u"\uE756"_s)); // CommandPrompt
    if (!r->missing && !r->offline)
        items.append(entry(OpenTerminalAdmin, tr("以管理员身份在终端中打开"), QString(), u"\uE7EF"_s)); // Admin shield
    if (r->isDir && m_contentEnabled && !r->path.startsWith(u"\\\\"_s)) // shares: contents are not read there
        items.append(entry(SearchInFolder, tr("在此文件夹中搜内容"), QString(), u"\uE721"_s)); // Search
    if (admin)
        items.append(entry(RunAsUser, tr("以普通权限运行"), QString(), u"\uE8E5"_s));
    else if (!r->isDir && !r->missing && !r->offline && shell::canRunAsAdministrator(r->path))
        items.append(entry(RunAsAdmin, tr("以管理员身份运行"), u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
    fileCommandItems(items);
    if (renamable(*r) && m_results.selectedCount() <= 1)
        items.append(entry(Rename, tr("重命名"), u"F2"_s, u"\uE70F"_s)); // Edit: Phosphor's pencil
    items.append(separator);
    items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
    items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
    items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
    favoriteItems();
    if (r->recent) {
        items.append(separator);
        items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
    }
    if (recyclable(*r)) {
        items.append(separator);
        items.append(entry(Recycle, tr("移到回收站"), QString(), u"\uE74D"_s, true)); // Delete
    }
    if (!r->missing && !r->offline) { // the shell's menu for it, for whom Shift+right-click is news
        items.append(separator);
        items.append(entry(SystemMenu, tr("更多选项"), m_defaultSystemMenu ? tr("右键") : tr("Shift+右键"), u"\uE712"_s)); // More
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
    dropServerWait(); // a server still being waited for no longer matters (nor opens under the menu)
    // The selection when the row is in it (its files; apps and the like
    // have no shell menu), else the row alone, as Win顺's menu has it.
    const SearchResults items = m_results.isSelected(row) && m_results.selectedCount() > 1 ? m_results.selection()
                                                                                         : SearchResults {*at};
    QStringList paths;
    QStringList network; // asked off the UI thread, below: a share that does not answer would hold the shell up for long
    for (const SearchResult& r : items) {
        if (r.isApp() || r.missing || r.offline)
            continue;
        if (onServer(r.path))
            network.append(r.path);
        else if (!serverAnswers(r.path)) // WSL: its state, without asking
            continue;
        paths.append(r.path);
    }
    if (paths.isEmpty())
        return;
    if (const qsizetype here = paths.indexOf(at->path); here > 0)
        paths.move(here, 0); // the row asked on first: the one the menu falls back to
    const std::optional<QPointF> pos = globalPos.canConvert<QPointF>() ? std::optional(globalPos.toPointF()) : std::nullopt;
    m_systemMenuAsked.start();
    if (!network.isEmpty()) {
        // Each server asked once (see perform); the menu once they
        // answered, for the files found there.
        checkServersThen(network, [this, paths, network, pos, name = at->name](const ServerFound& found) {
            QStringList there;
            for (const QString& path : paths) {
                if (!onServer(path) || found.value(path, share::Reach::Unreachable) == share::Reach::There)
                    there.append(path);
            }
            if (!there.isEmpty())
                runSystemMenu(there, pos); // from the event loop already
            else if (const QString why = whyNot(network, found); !why.isEmpty())
                flash(why);
            else
                flash(paths.size() == 1 ? tr("“%1” 已不存在").arg(name) : tr("选中的项目都已不存在"));
        });
        return;
    }
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
            if (c.singleFallback && !paths.isEmpty()) {
                remember(paths.first(), m_shownText);
            } else {
                for (const QString& path : paths)
                    remember(path, m_shownText);
            }
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
    // What Win顺's own menu says for the like of it; when the shell fell back to
    // the clicked item alone (multi-select across folders), tell the user honestly.
    if (choice.singleFallback && paths.size() > 1) {
        if (choice.verb == L"copy")
            flash(tr("系统菜单仅支持同目录多选，已复制当前项；跨目录批量操作请用 Win顺 右键菜单"), std::chrono::seconds(4));
        else if (choice.verb == L"cut")
            flash(tr("系统菜单仅支持同目录多选，已剪切当前项"), std::chrono::seconds(4));
        else if (choice.verb == L"copyaspath")
            flash(tr("系统菜单仅支持同目录多选，已复制当前项路径；跨目录请用 Win顺 右键菜单"), std::chrono::seconds(4));
        else if (choice.verb == L"delete")
            flash(tr("系统菜单仅支持同目录多选，已仅操作当前项；跨目录批量删除请用 Win顺 右键菜单"), std::chrono::seconds(4));
    } else if (choice.verb == L"copy") {
        flash(paths.size() == 1 ? tr("已复制，可在资源管理器中粘贴")
                                : tr("已复制 %Ln 项，可在资源管理器中粘贴", nullptr, static_cast<int>(paths.size())));
    } else if (choice.verb == L"cut") {
        flash(tr("已剪切，可在资源管理器中粘贴"));
    } else if (choice.verb == L"copyaspath") {
        flash(paths.size() == 1 ? tr("已复制路径") : tr("已复制 %Ln 个路径", nullptr, static_cast<int>(paths.size())));
    }
    forgetGone(choice.singleFallback && !paths.isEmpty() ? QStringList {paths.first()} : paths, 0);
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
    if (r && ((r->isPlugin() && !r->plugin->autocomplete.isEmpty()) || r->suggestion || completesWebKeyword(*r)))
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

// A site's own row, found by a part of its keyword or by its name ("b" or
// "百度" for baidu, GitHub #303): Tab puts its keyword and a space in the
// box, the words to search for typed on from there. Not for a page that
// takes no words, nor once the box holds just that.
bool Launcher::completesWebKeyword(const SearchResult& r) const
{
    if (!r.isWeb() || r.suggestion || !r.words.isEmpty() || r.keyword.isEmpty() || m_keyword)
        return false;
    const auto web = std::ranges::find_if(m_web, [&](const WebShortcut& w) { return w.keyword.compare(r.keyword, Qt::CaseInsensitive) == 0; });
    if (web == m_web.end() || !web->searches())
        return false;
    return !(m_query.endsWith(u' ') && m_query.trimmed().compare(r.keyword, Qt::CaseInsensitive) == 0);
}

navigation::TabAction Launcher::tabAction(int row) const
{
    if (m_scope == Apps)
        return navigation::TabAction::SwitchScope;
    const SearchResult* r = m_results.at(row);
    // The folder the box lists already (the first row under "D:\Work\"):
    // going into it changes nothing, Tab is the scopes' there (GitHub #361).
    const pathtext::TypedPath typed = pathtext::splitTyped(m_query);
    if (r && typed.name.isEmpty() && pathtext::sameFolder(typed.folder, r->path))
        r = nullptr;
    return navigation::tabAction(static_cast<ws::Scope>(m_scope), bool(m_keyword), m_tabEntersFavoriteFolder, r,
        completionRow(row) != nullptr, r && stepsInto(*r));
}

// GitHub #361: a folder the user chose in this list (chooseRow), or the one
// that completes the path being typed ("D:\Wo" on D:\Work, as a shell
// completes it). Not where the words are no names or no path: a plugin's
// keyword, regex mode, typed in a folder's window (whose folder would go).
bool Launcher::stepsInto(const SearchResult& r) const
{
    if (m_keyword || regexTerms() || typedInFolder() || !navigation::canBrowseFolder(r))
        return false;
    if (!m_chosenPath.isEmpty() && pathtext::samePath(r.path, m_chosenPath))
        return true;
    // A moment after typing, the rows on screen are the last query's: none
    // of them completes what is typed now (autocomplete waits for this one's).
    return !rowsComing() && navigation::completesPath(pathtext::splitTyped(m_query), r.path);
}

bool Launcher::completingPath() const
{
    if (m_keyword || regexTerms() || typedInFolder() || m_scope == Content || m_scope == Apps)
        return false;
    const pathtext::TypedPath typed = pathtext::splitTyped(m_query);
    return !typed.folder.isEmpty() && !typed.name.isEmpty() && !typed.name.front().isSpace();
}

void Launcher::chooseRow(int row)
{
    const SearchResult* r = m_results.at(row);
    const QString path = r ? r->path : QString();
    if (path == m_chosenPath)
        return;
    m_chosenPath = path;
    refreshKeyHints();
}

void Launcher::browseFolder(const SearchResult& folder)
{
    if (!navigation::canBrowseFolder(folder))
        return;
    const QString query = navigation::browseQuery(folder.path);
    // Where Ctrl+Backspace comes back to (stepOut, GitHub #361): these words,
    // in the scope under them (a prefix such as ":folder" sets its own again).
    const FolderStep step {m_query, m_filePrefix ? m_filePrefix->scope : m_scope, folder.path};
    const bool moved = query != m_query;
    if (m_scope != All)
        setScope(All);
    setQuery(query);
    if (moved)
        m_folderSteps.append(step);
    emit caretRequested(static_cast<int>(m_query.size()), false);
}

std::optional<Launcher::StepBack> Launcher::stepBack() const
{
    if (m_keyword || regexTerms() || typedInFolder() || m_scope == Content || m_scope == Apps)
        return std::nullopt;
    const pathtext::TypedPath typed = pathtext::splitTyped(m_query);
    if (typed.folder.isEmpty())
        return std::nullopt;
    if (typed.name.isEmpty() && !m_folderSteps.isEmpty() && pathtext::sameFolder(m_folderSteps.last().folder, typed.folder)) {
        const FolderStep& step = m_folderSteps.last();
        return StepBack {step.from, step.folder, step.scope != m_scope ? std::optional(step.scope) : std::nullopt, true};
    }
    const std::optional<navigation::Up> up = navigation::upFrom(typed);
    if (!up)
        return std::nullopt;
    return StepBack {up->query, up->row, std::nullopt, false};
}

bool Launcher::stepOut()
{
    const std::optional<StepBack> out = stepBack();
    if (!out)
        return false;
    if (out->back)
        m_folderSteps.removeLast();
    if (out->scope)
        setScope(*out->scope); // as it was, not remembered as chosen (chooseScope, GitHub #263)
    returnTo(out->query, out->row);
    return true;
}

void Launcher::returnTo(const QString& query, const QString& row)
{
    const QScopedValueRollback back(m_returnRow, row);
    setQuery(query);
    emit caretRequested(static_cast<int>(m_query.size()), false);
}

bool Launcher::autocomplete(int row)
{
    // GitHub #361: Tab right after a part of a name was typed in a path, its
    // rows not here yet: it waits for them (showRows), neither taking a
    // folder of the last query's rows for the completion nor the scopes.
    if (rowsComing() && completingPath()) {
        m_tabHeld = true;
        return true;
    }
    const navigation::TabAction action = tabAction(row);
    if (action == navigation::TabAction::BrowseFolder) {
        browseFolder(*m_results.at(row));
        return true;
    }
    if (action != navigation::TabAction::Complete)
        return false;
    const SearchResult* r = completionRow(row);
    // A site's suggestion: into the box after its keyword, to type on from.
    setQuery(r->suggestion ? r->keyword + u' ' + r->words : r->isWeb() ? r->keyword + u' ' : r->plugin->autocomplete);
    return true;
}

void Launcher::leaveKeyword()
{
    if (m_keyword)
        setQuery({});
}

bool Launcher::searchKeywordAsWords()
{
    if (!m_keyword)
        return false;
    // Only the first word as typed is a keyword (Catalog::match): quoted, it
    // is a word of the name, as any quoted word is.
    const QString words = m_keyword->words.trimmed();
    setQuery(u'"' + m_keyword->keyword + u'"' + (words.isEmpty() ? u" "_s : u' ' + words));
    emit caretRequested(static_cast<int>(m_query.size()), false);
    return true;
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
    return m_keyword && isShotHistory(m_keyword->pluginId, m_keyword->commandId);
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
        || searchText().compare(m_shotKeyword, Qt::CaseInsensitive) != 0)
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
        if (!m_pictureVerbs.pin)
            return;
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
    if (!m_plugins || !m_merge || (m_scope != All && !m_panel) || text.trimmed().isEmpty())
        return std::nullopt;
    for (const plugin::Match& m : m_plugins->catalog().match(text)) {
        // An instant command's keyword alone leaves the list the search's:
        // its rows lead it (plugin::Merge, GitHub #242).
        if (m.kind != plugin::Match::Kind::Keyword || m.alone)
            continue;
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        const QString locale = uiLocale();
        const QString icon = !m.command->icon.isEmpty() ? m.command->icon : m.entry->manifest->icon;
        return KeywordMode {m.entry->id, m.command->id, m.keyword, m.text,
            m.command->title.pick(locale).isEmpty() ? m.entry->manifest->name.pick(locale) : m.command->title.pick(locale),
            m.command->hint.pick(locale), FileIconProvider::pluginIconUrl(m.entry->id, icon, dark),
            m.command->run.kind == plugin::Run::Kind::Program ? m.command->run.suggest : QString(), m.command->history};
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
    if (!m_queryError.isEmpty()) {
        ++m_pluginQueryId;
        if (m_plugins)
            m_plugins->cancelAll();
        if (m_merge)
            m_merge->begin(m_pluginQueryId, text, {}, {});
        if (was)
            emit pluginChanged();
        return;
    }
    if (keyword && isShotHistory(keyword->pluginId, keyword->commandId)) {
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
    const auto scope = keyword && m_panel ? ws::Scope::All : static_cast<ws::Scope>(m_scope);
    const std::uint64_t id = ++m_pluginQueryId;
    m_keyword = std::move(keyword);
    // 内容 narrowed to kinds of file: the providers' rows too (GitHub #447), as the scan's (setContentPlace).
    const bool kinds = m_scope == Content && !m_keyword;
    m_merge->setContentKinds(kinds ? contentTypeExtensions() : QStringList(),
        kinds && m_contentFolder.isEmpty() && m_folderFilter.isEmpty() ? contentTypeFolders() : QStringList());
    // The keywords offered as rows are 全部's (matchKeyword): in 文件 and 文件夹 they could not be run.
    m_merge->begin(id, text, m_keyword ? m_keyword->pluginId : QString(), m_keyword ? m_keyword->commandId : QString(),
        m_scope == Content, m_scope == All || m_panel);
    m_pluginTimer.start();
    m_plugins->query(text, scope, id, pluginFilters()); // zero-code ones answer within
    if (!m_pluginPending.isEmpty())
        m_pluginWait.start();
    if (was || m_keyword)
        emit pluginChanged();
    if (was != m_keyword.has_value())
        refreshKeyHints(); // Shift+Tab's, before any row comes
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
    const SearchResult* current = m_results.at(m_currentRow);
    const QString currentPath = current ? current->path : QString();
    // The keyword's list as it stands, when this changes it by itself (a
    // rerun, a later batch: not the first rows for what was typed).
    const bool update = m_keyword && m_keyword->pluginId == pluginId && !m_replacePending && !m_panel;
    QStringList before;
    if (update) {
        before.reserve(m_results.count());
        for (int i = 0; i < m_results.count(); ++i)
            before.append(m_results.at(i)->path);
    }
    plugin::Items filtered = items;
    const QStringList typeFolders = fileTypeFolders();
    if (!m_keyword && !typeFolders.isEmpty()) {
        const QStringList extensions = fileExtensions();
        filtered.removeIf([&](const plugin::Item& item) {
            if (item.open.kind != plugin::Open::Kind::Path || !pathtext::inChosenFolders(typeFolders, item.open.target))
                return true;
            return !extensions.contains(QFileInfo(item.open.target).suffix(), Qt::CaseInsensitive);
        });
    }
    if (m_panel) {
        const EntryFilter filter = panelFilter();
        // The quick filters, for a row that opens a file: by its path, as a
        // file's own row. The rest (a note, a web page) are in no folder.
        ws::ParsedQuery gate;
        gate.conditions = ws::parseConditions(quickConditions());
        const ws::NameMatcher quick(gate);
        filtered.removeIf([&](const plugin::Item& item) {
            if (!gate.conditions.empty() && item.open.kind == plugin::Open::Kind::Path
                && quick.matchPath(QDir::toNativeSeparators(item.open.target), false, item.modified,
                       item.size >= 0 ? packSize(static_cast<std::uint64_t>(item.size)) : kUnknownSize)
                    < 0)
                return true;
            if (item.modified != 0 && filter.byTime()) {
                EntryFilter time = filter;
                time.minSize = 0;
                time.maxSize = kUnknownSize;
                if (!time.fits(item.modified, kUnknownSize, false))
                    return true;
            }
            if (item.size >= 0 && filter.bySize()) {
                const FileSize size = packSize(item.size);
                if (size < filter.minSize || size > filter.maxSize)
                    return true;
            }
            if (!m_folderFilter.isEmpty() && item.open.kind == plugin::Open::Kind::Path) {
                const QString folder = QDir::toNativeSeparators(m_folderFilter);
                const QString path = QDir::toNativeSeparators(item.open.target);
                if (path.compare(folder, Qt::CaseInsensitive) != 0
                    && !path.startsWith(folder.endsWith(u'\\') ? folder : folder + u'\\', Qt::CaseInsensitive))
                    return true;
            }
            const QStringList extensions = fileExtensions();
            const QString extension = QFileInfo(item.title).suffix().toLower();
            return !extensions.isEmpty() && !extension.isEmpty() && !extensions.contains(extension, Qt::CaseInsensitive);
        });
    }
    m_merge->results(queryId, pluginId, commandId, filtered, partial, replace);
    // 内容: the providers' rows do not wait for the native scan (with no
    // content index it reads file after file); they take the place of the
    // last query's rows, and its matches go in above them as they come.
    if (m_merge->contentOnly() && !m_merge->namesReady() && m_merge->answerHeld()) {
        if (m_replacePending)
            showRows({}, {}); // namesShown
        else
            m_merge->namesShown();
    }
    if (m_merge->takeBetterFirstRow())
        emit betterFirstRow();
    sortShownPanel(currentPath);
    if (update)
        followCurrentRow(before, currentPath);
    refreshStatus();
}

// A keyword's list changed by itself (onPluginResults): rows come and go
// above the keyboard's (a request arrives, a device shows up), so the row
// the keyboard is on would be another under the same Enter. The keyboard
// follows its row, the first row's too (GitHub #113: a request coming in on
// top never takes an Enter meant for the row below; the user moves onto it).
// A row that went, or a row clicked that changed, does nothing for a moment
// (trigger, clickRow). Only right after the list opened from that plugin's
// notification (openedFromToast) does the first row stay first, once: the
// list opens as it was when hidden, and the request the notification was
// about comes in on top a moment later. Enter waits a moment there too.
void Launcher::followCurrentRow(const QStringList& before, const QString& currentPath)
{
    const int count = m_results.count();
    if (!m_rowClock.isValid())
        m_rowClock.start();
    m_rowChangedAt.resize(count);
    QStringList after;
    after.reserve(count);
    for (int i = 0; i < count; ++i) {
        after.append(m_results.at(i)->path);
        if (i >= before.size() || after[i] != before[i])
            m_rowChangedAt[i] = m_rowClock.elapsed() + 1;
    }
    const bool standIn = m_currentGone.isValid() && m_currentGone.elapsed() < 800;
    const bool openedForIt = m_toastOpened.isValid() && m_toastOpened.elapsed() < kToastOpenMs && m_keyword
                             && m_keyword->pluginId == m_toastPlugin;
    const navigation::RowFollow follow = navigation::followRow(before, after, m_currentRow, currentPath, standIn, openedForIt);
    if (follow.kind == navigation::RowFollow::Kind::Stay)
        return;
    const QScopedValueRollback following(m_following, true);
    if (follow.kind == navigation::RowFollow::Kind::Wait) {
        // Past the end of a shorter list it goes to the last row, still
        // waiting (the list would move it there later, as if chosen).
        if (m_currentRow == 0 && openedForIt)
            m_toastOpened.invalidate(); // once: what the notification was about came
        m_currentGone.start();
        if (m_currentRow != 0)
            emit currentRowMoved(m_currentRow < count ? -1 : count - 1); // -1: only closes a menu opened on it
        return;
    }
    emit currentRowMoved(follow.row);
}

void Launcher::openedFromToast(const QString& pluginId)
{
    m_toastPlugin = pluginId;
    m_toastOpened.start();
}

bool Launcher::rowJustChanged(int row) const
{
    if (row < 0 || row >= m_rowChangedAt.size() || m_rowChangedAt[row] == 0)
        return false;
    return m_rowClock.elapsed() + 1 - m_rowChangedAt[row] < 800;
}

void Launcher::forgetRowChanges()
{
    m_rowChangedAt.clear();
    m_currentGone.invalidate();
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
        case PluginRow::OpenKind::Path: {
            // A program with arguments or a working folder is started as
            // such; anything else opens as the shell would open it. Win顺
            // carries this out, so a plugin without `processes` may not have
            // it start a program (pluginOpenAllowed); the launcher stays open
            // to show the notice.
            plugin::Open open;
            open.kind = plugin::Open::Kind::Path;
            open.target = p.openTarget;
            open.args = p.openArgs;
            open.cwd = p.openCwd;
            if (!pluginOpenAllowed(p.pluginId, open))
                return;
            if (p.openArgs.isEmpty() && p.openCwd.isEmpty())
                shell::open(p.openTarget, false, reportFailure(result.name));
            else if (!shell::startUnelevated(p.openTarget.toStdWString(), p.openArgs.toStdWString(), p.openCwd.toStdWString()).valid())
                emit openFailed(result.name);
            emit dismissRequested();
            return;
        }
        case PluginRow::OpenKind::Url: {
            // "winshun:settings": Win顺's own settings at the plugin's card
            // (a row saying what to set up); never handed to the shell.
            if (p.openTarget.compare(u"winshun:settings", Qt::CaseInsensitive) == 0) {
                emit pluginSettingsRequested(p.pluginId);
                emit dismissRequested();
                return;
            }
            plugin::Open open;
            open.kind = plugin::Open::Kind::Url;
            open.target = p.openTarget;
            if (!pluginOpenAllowed(p.pluginId, open))
                return;
            shell::openUrl(p.openTarget);
            emit dismissRequested();
            return;
        }
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
    m_plugins->action(++m_pluginRequest, p.pluginId, p.commandId, actionId, p.item, pluginWords(result));
}

void Launcher::onPluginActionDone(std::uint64_t requestId, const QString& pluginId, const QList<ws::plugin::Effect>& effects, bool ok, const QString& error)
{
    (void)requestId;
    if (!ok) {
        flash(error.isEmpty() ? tr("插件没有完成这个操作") : error);
        return;
    }
    applyEffects(pluginId, effects);
}

bool Launcher::fileCommandPaths(int row, QStringList* paths, QList<bool>* folders) const
{
    const SearchResult* at = m_results.at(row);
    if (!at)
        return false;
    const SearchResults items = m_results.isSelected(row) && m_results.selectedCount() > 1 ? m_results.selection()
                                                                                         : SearchResults {*at};
    for (const SearchResult& r : items) {
        if (r.isApp() || r.clipId > 0 || r.missing || r.offline || r.path.isEmpty())
            return false;
        paths->append(QDir::toNativeSeparators(r.path));
        folders->append(r.isDir);
    }
    return !paths->isEmpty();
}

// A plugin's `files` command from a file's menu (menuItems): one action
// request with every path, its effects as an action's (onPluginActionDone).
// The plugin and command the menu named, for the files it was for, if they
// still take them (GitHub #113: by its place alone, the files could go to
// another plugin once the plugins changed under the menu).
void Launcher::runFileCommand(int row, int index)
{
    dropServerWait(); // a server still being waited for no longer matters
    if (index < 0 || static_cast<std::size_t>(index) >= m_menuFileCommands.size())
        return;
    const MenuFileCommand chosen = m_menuFileCommands[static_cast<std::size_t>(index)];
    QStringList paths;
    QList<bool> folders;
    if (!m_plugins || !fileCommandPaths(row, &paths, &folders))
        return;
    if (paths != chosen.paths || !plugin::Catalog::hasFileCommand(m_plugins->catalog().fileCommands(paths, folders), chosen.pluginId, chosen.commandId)) {
        flash(tr("插件或选中的文件刚刚变了，再打开一次菜单"));
        return;
    }
    m_plugins->action(++m_pluginRequest, chosen.pluginId, chosen.commandId, u"files"_s, QJsonObject {}, QString(), paths);
}

void Launcher::runPluginCommand(const QString& pluginId, const QString& commandId, const QString& actionId,
    const QString& text, const QStringList& files)
{
    if (!m_plugins)
        return;
    setScope(All);
    clearFilters();
    setQuery({});
    m_plugins->action(++m_pluginRequest, pluginId, commandId, actionId, QJsonObject {}, text, files);
}

// Win顺 carries out a plugin's declarative open (an item's default action, or
// an `open` effect): to a program or a non-web link only when the plugin has
// `processes` (OpenPolicy). Blocked: don't run it, say so plainly, log the
// first block of each plugin.
bool Launcher::pluginOpenAllowed(const QString& pluginId, const plugin::Open& open)
{
    // Off, or a newer version waiting for the user's OK on more permissions
    // (Catalog::setConfirmed): its rows still on show open nothing, as its
    // manifest's permissions are not the user's word yet.
    if (m_plugins && !m_plugins->catalog().enabled(pluginId)) {
        flash(m_plugins->catalog().awaitingConsent(pluginId).isEmpty()
                ? tr("“%1”已停用").arg(pluginName(pluginId))
                : tr("“%1”的新版本要更多权限，到设置里同意后才能用").arg(pluginName(pluginId)));
        return false;
    }
    const plugin::Entry* e = m_plugins ? m_plugins->catalog().find(pluginId) : nullptr;
    const bool hasProcesses = e && e->manifest && e->manifest->permissions.has(plugin::Permission::Processes);
    const plugin::OpenRuling ruling = plugin::ruleOpen(open, hasProcesses, plugin::systemProbe());
    if (ruling.allowed())
        return true;
    const QString shown = ruling.target.size() > 60 ? ruling.target.left(57) + QStringLiteral("…") : ruling.target;
    flash(tr("“%1”没有“启动程序”权限，没能打开 %2").arg(pluginName(pluginId), shown));
    if (!m_openBlockedLogged.contains(pluginId)) {
        m_openBlockedLogged.insert(pluginId);
        qWarning().noquote() << u"[plugin] "_s + pluginId + u" 没有 processes 权限，拦下 open："_s + ruling.target;
    }
    return false;
}

// What a plugin asked for after an action, in its order: hide (the default),
// keep, setQuery, notify, copy, open, replace (its items as the list), reveal.
// `pluginId`: whose action it was (onPluginActionDone), for the open policy.
void Launcher::applyEffects(const QString& pluginId, const QList<ws::plugin::Effect>& effects)
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
                const QString card = !pluginId.isEmpty() ? pluginId : (m_keyword ? m_keyword->pluginId : QString());
                if (!card.isEmpty())
                    emit pluginSettingsRequested(card); // its own card (see performPlugin)
            } else if (pluginOpenAllowed(pluginId, e.open)) {
                if (e.open.kind == plugin::Open::Kind::Url)
                    shell::openUrl(e.open.target);
                else if (e.open.args.isEmpty() && e.open.cwd.isEmpty())
                    shell::open(e.open.target, false);
                else
                    shell::startUnelevated(e.open.target.toStdWString(), e.open.args.toStdWString(), e.open.cwd.toStdWString());
            }
            break;
        case plugin::Effect::Kind::Replace:
            if (m_merge && m_keyword) {
                // As a rerun's rows (onPluginResults): the keyboard stays on its row.
                const SearchResult* current = m_results.at(m_currentRow);
                const QString currentPath = current ? current->path : QString();
                QStringList before;
                for (int i = 0; i < m_results.count(); ++i)
                    before.append(m_results.at(i)->path);
                m_merge->replace(m_keyword->pluginId, m_keyword->commandId, e.items);
                if (!m_panel)
                    followCurrentRow(before, currentPath);
            }
            break;
        case plugin::Effect::Kind::Reveal: {
            // Only selects (as show_in_folder): no program starts, no processes
            // permission. Gone by now: its folder, if that is still there (#194).
            if ((m_plugins && !m_plugins->catalog().enabled(pluginId)) || !plugin::plainLocalPath(e.open.target))
                break;
            const QFileInfo item(e.open.target);
            if (item.exists())
                shell::reveal(QDir::toNativeSeparators(item.absoluteFilePath()));
            else if (QFileInfo(item.absolutePath()).isDir())
                shell::open(QDir::toNativeSeparators(item.absolutePath()), false);
            break;
        }
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
