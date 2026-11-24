#include "FolderMenu.h"

#include "DiagnosticLog.h"
#include "AppCatalog.h"
#include "FileIconProvider.h"
#include "Favorites.h"
#include "History.h"
#include "RecentFolders.h"
#include "SystemCatalog.h"
#include "platform/FileDialog.h"
#include "platform/FileManagers.h"
#include "platform/Integrity.h"
#include "platform/KeyRouter.h"
#include "platform/Power.h"
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QClipboard>
#include <QCollator>
#include <QCursor>
#include <QThreadPool>
#include <QGuiApplication>
#include <QJsonObject>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>
#include <QWindow>

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <optional>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr int kMaxRecent = 20; // bounded shell history, with room beyond the ten direct rows
constexpr int kMaxJournal = 24; // different stories of the double click a run (journal())

// The DPI awareness a window was made with, for the journal.
const char* awarenessOf(HWND window)
{
    const DPI_AWARENESS_CONTEXT context = ::GetWindowDpiAwarenessContext(window);
    if (::AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return "per-monitor-v2";
    if (::AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
        return "per-monitor";
    if (::AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
        return "system";
    if (::AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED))
        return "gdi-scaled";
    if (::AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_UNAWARE))
        return "unaware";
    return "unknown";
}

// Listary's own process (not its service): it brings up its menu on the
// same double click (docs/pitfalls.md). Asked at most every few seconds.
bool listaryRuns()
{
    static std::atomic<qint64> askedAt {0};
    static std::atomic<bool> runs {false};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - askedAt.load() < 5000)
        return runs.load();
    bool found = false;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry {sizeof entry};
        for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !found; more = ::Process32NextW(snapshot, &entry))
            found = ::_wcsicmp(entry.szExeFile, L"Listary.exe") == 0;
        ::CloseHandle(snapshot);
    }
    runs = found;
    askedAt = now;
    return found;
}

// A File Explorer window Clover holds as a tab, with Clover's own "双击文件夹
// 空白处跳到上层目录" on (its default): Clover goes up on that same double
// click, so the menu would come for a folder no longer shown. The switch is
// kept in config.ini beside clover.exe ([Seting] IsDoubleClickToNavigateUp).
bool cloverGoesUp(HWND frame)
{
    const HWND top = ::GetAncestor(frame, GA_ROOT);
    wchar_t name[64] {};
    if (!top || top == frame || !::GetClassNameW(top, name, 64) || std::wcsncmp(name, L"Clover_WidgetWin", 16) != 0)
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(top, &pid);
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return true;
    wchar_t path[MAX_PATH * 2] {};
    DWORD size = MAX_PATH * 2;
    const bool named = ::QueryFullProcessImageNameW(process, 0, path, &size);
    ::CloseHandle(process);
    if (!named)
        return true;
    std::wstring ini(path, size);
    ini.resize(ini.find_last_of(L'\\') + 1);
    ini += L"config.ini";
    return ::GetPrivateProfileIntW(L"Seting", L"IsDoubleClickToNavigateUp", 1, ini.c_str()) != 0;
}

// 360文件夹's own "双击文件夹空白区域跳转上层目录", on until unticked:
// HKCU\Software\360FileBrowser\Setting dbclick_to_parent (written at once).
bool hostGoesUp()
{
    wchar_t text[16] {};
    DWORD type = 0;
    DWORD size = sizeof text - sizeof(wchar_t);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\360FileBrowser\\Setting", L"dbclick_to_parent",
                       RRF_RT_REG_DWORD | RRF_RT_REG_SZ, &type, text, &size) != ERROR_SUCCESS)
        return true;
    if (type == REG_DWORD)
        return *reinterpret_cast<const DWORD*>(text) != 0;
    return std::wcstol(text, nullptr, 10) != 0;
}

// "D:\Projects\报告" -> "报告"; "D:\" -> "D:\".
QString nameOf(const QString& folder)
{
    const qsizetype slash = folder.lastIndexOf(u'\\');
    return slash < 0 || slash == folder.size() - 1 ? folder : folder.mid(slash + 1);
}

const AppInfo* findApp(const AppList& apps, const QString& path)
{
    const auto found = std::ranges::find(apps, appIdOf(path), &AppInfo::id);
    return found == apps.end() ? nullptr : &*found;
}

const PlaceInfo* findPlace(const PlaceList& places, const QString& path)
{
    const auto found = std::ranges::find(places, placeKeyOf(path), &PlaceInfo::key);
    return found == places.end() ? nullptr : &*found;
}

bool samePath(const QString& a, const QString& b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

// The screen a physical point is on, as Qt knows it; the primary when none is.
QScreen* screenAt(POINT pt)
{
    const HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    QScreen* screen = QGuiApplication::primaryScreen();
    for (QScreen* candidate : QGuiApplication::screens()) {
        const auto* native = candidate->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            screen = candidate;
    }
    return screen;
}

// The file manager whose window the menu is over, for the surfaces that are one's.
std::optional<filemanager::Kind> managerOf(folderview::Surface surface)
{
    switch (surface) {
    case folderview::Surface::Opus:
        return filemanager::Kind::DirectoryOpus;
    case folderview::Surface::TotalCommander:
        return filemanager::Kind::TotalCommander;
    case folderview::Surface::XYplorer:
        return filemanager::Kind::XYplorer;
    case folderview::Surface::Pulse:
        return filemanager::Kind::Pulse;
    case folderview::Surface::OneCommander:
        return filemanager::Kind::OneCommander;
    default:
        return std::nullopt;
    }
}

const char* nameOf(folderview::Surface surface)
{
    switch (surface) {
    case folderview::Surface::Explorer:
        return "File Explorer";
    case folderview::Surface::Desktop:
        return "the desktop";
    case folderview::Surface::Opus:
        return "Directory Opus";
    case folderview::Surface::TotalCommander:
        return "Total Commander";
    case folderview::Surface::XYplorer:
        return "XYplorer";
    case folderview::Surface::Pulse:
        return "Pulse";
    case folderview::Surface::OneCommander:
        return "OneCommander";
    case folderview::Surface::Dialog:
        return "a file dialog";
    case folderview::Surface::Other:
        return "another window";
    case folderview::Surface::None:
        break;
    }
    return "nothing";
}

} // namespace

struct FolderMenu::Found {
    QString folder; // the tab's
    QStringList recent;
    QStringList windows;
};

FolderMenu::FolderMenu(History* history, Favorites* favorites, QObject* parent)
    : QObject(parent)
    , m_history(history)
    , m_favorites(favorites)
{
}

FolderMenu::~FolderMenu()
{
    m_router.reset();
    m_draining.clear();
}

void FolderMenu::setWindow(QWindow* window)
{
    m_window = window;
}

void FolderMenu::setRecordHistory(bool on)
{
    m_recordHistory = on;
    if (!on || !m_history)
        return;
    // The recent folders gathered now, in the background: the first menu comes with them.
    m_worker.post([this, history = m_history->items()] {
        if (!m_recentKnown) {
            m_recent = recentfolders::recent(history, kMaxRecent + 1);
            m_recentKnown = true;
        }
    });
}

void FolderMenu::setOptions(bool yieldToListary, bool explorer, bool desktop)
{
    m_yieldToListary = yieldToListary;
    m_explorer = explorer;
    m_desktop = desktop;
}

void FolderMenu::setGroups(bool favorites, bool recent, bool windows)
{
    m_listFavorites = favorites;
    m_listRecent = recent;
    m_listWindows = windows;
}

void FolderMenu::setFavoriteKinds(FavoriteKinds kinds)
{
    m_favoriteKinds = kinds;
}

void FolderMenu::setAppsExpanded(bool expanded)
{
    if (m_appsExpanded == expanded)
        return;
    m_appsExpanded = expanded;
    if (m_open && m_hasFavorites)
        rebuildEntries();
}

namespace {
constexpr int clampInline(int count) noexcept
{
    return count == FolderMenu::kInlineAll ? FolderMenu::kInlineAll : std::clamp(count, 0, FolderMenu::kInlineMax);
}

bool includesKind(const FolderMenu::FavoriteKinds& kinds, Favorite::Kind kind) noexcept
{
    switch (kind) {
    case Favorite::Kind::Folder:
        return kinds.folders;
    case Favorite::Kind::File:
        return kinds.files;
    case Favorite::Kind::App:
        return kinds.apps;
    case Favorite::Kind::Place:
        return kinds.places;
    case Favorite::Kind::Command:
        return kinds.commands;
    }
    return false;
}

QVariantMap appsToggleRow(qsizetype count, bool expanded)
{
    return QVariantMap {
        {u"text"_s, FolderMenu::tr("应用")},
        {u"detail"_s, QString::number(count)},
        {u"toggle"_s, u"apps"_s},
        {u"expanded"_s, expanded},
    };
}

QVariantList combineFavoriteRows(const QVariantList& primary, const QVariantList& apps, bool appsExpanded, bool leadingSeparator)
{
    QVariantList rows = primary;
    if (!apps.isEmpty()) {
        if (leadingSeparator && !primary.isEmpty())
            rows.append(QVariantMap {{u"separator"_s, true}});
        rows.append(appsToggleRow(apps.size(), appsExpanded));
        if (appsExpanded)
            rows.append(apps);
    }
    return rows;
}
} // namespace

void FolderMenu::setInlineCounts(int favorites, int recent, int windows)
{
    m_favoritesInline = clampInline(favorites);
    m_recentInline = clampInline(recent);
    m_windowsInline = clampInline(windows);
}

QVariantList FolderMenu::groupEntries(const QString& title, const QString& glyph, const QVariantList& children, int inlineCount)
{
    inlineCount = clampInline(inlineCount);
    QVariantMap group {{u"text"_s, title}, {u"glyph"_s, glyph}, {u"children"_s, children},
        {u"detail"_s, QString::number(children.size())}, {u"disabled"_s, children.isEmpty()}};
    if (inlineCount == 0 || children.isEmpty())
        return {group};
    QVariantList rows {QVariantMap {{u"text"_s, title}, {u"detail"_s, QString::number(children.size())},
        {u"heading"_s, true}, {u"disabled"_s, true}}};
    const qsizetype count = inlineCount == kInlineAll
        ? children.size()
        : std::min<qsizetype>(inlineCount, children.size());
    rows.append(children.first(count));
    if (count < children.size()) {
        group[u"text"_s] = tr("更多…");
        group[u"pageTitle"_s] = title;
        group[u"children"_s] = children.sliced(count);
        group[u"detail"_s] = QString::number(children.size() - count);
        rows.append(group);
    }
    return rows;
}

QVariantList FolderMenu::favoriteGroupEntries(const QString& title, const QString& glyph, const QVariantList& primary,
    const QVariantList& apps, bool appsExpanded, int inlineCount)
{
    inlineCount = clampInline(inlineCount);
    if (apps.isEmpty()) {
        QVariantList rows = groupEntries(title, glyph, primary, inlineCount);
        if (!rows.isEmpty()) {
            QVariantMap first = rows.first().toMap();
            if (first.contains(u"children"_s)) {
                first.insert(u"group"_s, u"favorites"_s);
                rows[0] = first;
            }
        }
        return rows;
    }
    const qsizetype total = primary.size() + apps.size();
    if (inlineCount == 0) {
        return {QVariantMap {{u"text"_s, title}, {u"glyph"_s, glyph}, {u"group"_s, u"favorites"_s},
            {u"children"_s, combineFavoriteRows(primary, apps, appsExpanded, true)},
            {u"detail"_s, QString::number(total)}, {u"disabled"_s, false}}};
    }
    QVariantList rows {QVariantMap {{u"text"_s, title}, {u"detail"_s, QString::number(total)},
        {u"heading"_s, true}, {u"disabled"_s, true}}};
    if (inlineCount == kInlineAll) {
        rows.append(combineFavoriteRows(primary, apps, appsExpanded, false));
        return rows;
    }
    if (primary.size() >= inlineCount) {
        rows.append(primary.first(inlineCount));
        const QVariantList restPrimary = primary.sliced(inlineCount);
        rows.append(QVariantMap {{u"text"_s, tr("更多…")}, {u"pageTitle"_s, title}, {u"glyph"_s, glyph},
            {u"group"_s, u"favorites-more"_s},
            {u"children"_s, combineFavoriteRows(restPrimary, apps, appsExpanded, true)},
            {u"detail"_s, QString::number(restPrimary.size() + apps.size())}, {u"disabled"_s, false}});
        return rows;
    }
    rows.append(primary);
    rows.append(appsToggleRow(apps.size(), appsExpanded));
    if (appsExpanded) {
        const qsizetype directApps = std::min(inlineCount - primary.size(), apps.size());
        rows.append(apps.first(directApps));
        if (directApps < apps.size()) {
            rows.append(QVariantMap {{u"text"_s, tr("更多…")}, {u"pageTitle"_s, tr("应用")}, {u"glyph"_s, glyph},
                {u"group"_s, u"favorites-more"_s},
                {u"children"_s, apps.sliced(directApps)},
                {u"detail"_s, QString::number(apps.size() - directApps)}, {u"disabled"_s, false}});
        }
    }
    return rows;
}

QVariantList FolderMenu::assembleGroups(const QList<QVariantList>& groups)
{
    QVariantList out;
    bool prevInline = false;
    for (const QVariantList& rows : groups) {
        if (rows.isEmpty())
            continue;
        const bool isInline = rows.first().toMap().value(u"heading"_s).toBool();
        if (!out.isEmpty() && (prevInline || isInline))
            out.append(QVariantMap {{u"separator"_s, true}});
        out.append(rows);
        prevInline = isInline;
    }
    return out;
}

void FolderMenu::setItems(bool search, bool copyPath, bool cmd, bool powerShell, bool favorite, bool settings)
{
    m_itemSearch = search;
    m_itemCopyPath = copyPath;
    m_itemCmd = cmd;
    m_itemPowerShell = powerShell;
    m_itemFavorite = favorite;
    m_itemSettings = settings;
}

void FolderMenu::doubleClicked(HWND window, POINT pt)
{
    // On the listener thread: class names only, nothing that waits.
    const folderview::Hit hit = folderview::classifyMenu(window, pt);
    if (hit.surface == folderview::Surface::None) {
        // A folder view's window all the same, of a kind not known (another
        // shell's, a file dialog's): for the journal (GitHub #45).
        if (folderview::inShellView(window))
            m_worker.post([this, chain = folderview::classChain(window)] { journal(u"classify"_s, {}, chain, nullptr); });
        return;
    }
    if ((hit.surface == folderview::Surface::Explorer && !m_explorer) || (hit.surface == folderview::Surface::Desktop && !m_desktop)) {
        // Its own switch in the settings is off (the journal: the diagnostics
        // have no settings file).
        m_worker.post([this, hit] { journal(u"switched-off"_s, hit, {}, nullptr); });
        return;
    }
    QMetaObject::invokeMethod(this, [this, hit] {
        if (m_blocked && m_blocked()) {
            m_worker.post([this, hit] { journal(u"blocked"_s, hit, {}, nullptr); });
            return;
        }
        const int ask = ++m_asks;
        const bool yield = m_yieldToListary;
        const bool recordHistory = m_recordHistory;
        const QStringList history = recordHistory && m_history ? m_history->items() : QStringList();
        m_worker.post([this, hit, ask, yield, recordHistory, history] {
            if (ask != m_asks)
                return;
            if (yield && listaryRuns()) {
                journal(u"listary"_s, hit, {}, nullptr);
                return;
            }
            if (hit.surface == folderview::Surface::Explorer && (hit.hosted ? hostGoesUp() : cloverGoesUp(hit.root))) {
                // The tab goes up itself on this double click (its own switch).
                journal(u"goes-up"_s, hit, {}, nullptr);
                return;
            }
            QElapsedTimer clock;
            clock.start();
            folderview::BlankTrace trace;
            if (!folderview::isBlank(hit, &trace)) {
                journal(u"not-blank"_s, hit, {}, &trace, {{u"ms"_s, clock.elapsed()}});
                return;
            }
            const qint64 blankMs = clock.elapsed();
            // Before asking Explorer for the folder: should that never come
            // back, the journal still says the double click got this far.
            // (A story not told yet costs one accessibility walk, once.)
            journal(u"blank"_s, hit, {}, &trace, {{u"ms"_s, blankMs}});
            clock.restart();
            const QString folder = hit.surface == folderview::Surface::Explorer ? explorerFolder(hit) : QString();
            qInfo().noquote() << u"Folder menu: blank %1 (%2), folder %3 ms"_s.arg(blankMs)
                                     .arg(QString::fromLatin1(trace.by))
                                     .arg(clock.elapsed());
            gatherHere(hit, ask, folder, recordHistory, history);
        });
    }, Qt::QueuedConnection);
}

// The double click's way in the diagnostics journal (GitHub #45): where it
// stopped (classify: a folder view's window of a kind not known; switched-off;
// blocked; listary; not-blank; not-in-front) or that it got through (blank), told by
// window classes, accessibility roles and UI Automation class names, never
// a file's or folder's name. Each different story once a run, at most
// kMaxJournal; only a story not told yet costs the accessibility walk added
// for comparison (one more round to Explorer). On the worker.
void FolderMenu::journal(const QString& step, const folderview::Hit& hit, const std::wstring& chain,
    folderview::BlankTrace* trace, const QJsonObject& more)
{
    if (m_journaled.size() >= kMaxJournal)
        return;
    const QString classes = QString::fromStdWString(hit.view ? folderview::classChain(hit.view) : chain);
    QString story = step + u'|' + classes;
    if (trace)
        story += u'|' + QString::fromLatin1(trace->by) + u'|' + QString::fromStdWString(trace->uiaClass);
    if (m_journaled.contains(story))
        return;
    m_journaled.insert(story);
    QJsonObject details = more;
    details[u"step"_s] = step;
    details[u"surface"_s] = hit.surface == folderview::Surface::Explorer ? u"explorer"_s
        : hit.surface == folderview::Surface::Desktop                    ? u"desktop"_s
                                                                         : u"none"_s;
    details[u"classes"_s] = classes;
    if (hit.hosted)
        details[u"hosted"_s] = true; // 360文件夹's
    if (trace) {
        if (trace->msaa.empty() && std::string_view(trace->by) != "desktop-background")
            trace->msaa = folderview::accessibleTrace(hit); // what the walk alone would have said
        details[u"by"_s] = QString::fromLatin1(trace->by);
        details[u"uia"_s] = QString::fromStdWString(trace->uiaClass);
        details[u"uiaType"_s] = static_cast<qint64>(trace->uiaControlType);
        details[u"uiaResult"_s] = u"0x%1"_s.arg(static_cast<quint32>(trace->uiaResult), 8, 16, u'0');
        details[u"uiaOtherProcess"_s] = trace->uiaOtherProcess;
        details[u"msaa"_s] = QString::fromStdWString(trace->msaa);
    }
    if (hit.view && ::IsWindow(hit.view)) {
        details[u"dpi"_s] = static_cast<int>(::GetDpiForWindow(hit.view));
        details[u"awareness"_s] = QString::fromLatin1(awarenessOf(hit.view));
        DWORD process = 0;
        DWORD desktop = 0;
        ::GetWindowThreadProcessId(hit.view, &process);
        ::GetWindowThreadProcessId(::GetShellWindow(), &desktop);
        details[u"desktopProcess"_s] = process == desktop; // false: folder windows in a process of their own
        details[u"higherIntegrity"_s] = integrity::higherThanOurs(hit.root);
    }
    diagnostics::record(u"folder-menu-double-click"_s, details);
}

void FolderMenu::requested(const QString& folder)
{
    if (m_blocked && m_blocked())
        return;
    const int ask = ++m_asks;
    const bool recordHistory = m_recordHistory;
    const QStringList history = recordHistory && m_history ? m_history->items() : QStringList();
    m_worker.post([this, ask, folder, recordHistory, history] {
        if (ask != m_asks)
            return;
        // Where the pointer is: Opus ran WinShunSearch.exe on the double
        // click a moment ago, and the lister under it is the one. (Asked
        // here: WindowFromPoint may wait on the window it finds.)
        folderview::Hit hit;
        ::GetCursorPos(&hit.pt);
        hit.view = ::WindowFromPoint(hit.pt);
        hit.root = hit.view ? ::GetAncestor(hit.view, GA_ROOT) : nullptr;
        if (!hit.root || filemanager::kindOf(hit.root) != filemanager::Kind::DirectoryOpus) {
            qWarning() << "Folder menu: asked for by Directory Opus, but no lister under the pointer";
            return;
        }
        hit.surface = folderview::Surface::Opus;
        hit.tab = hit.root;
        gatherHere(hit, ask, folder, recordHistory, history);
    });
}

void FolderMenu::hotkeyPressed()
{
    using Action = menuhotkey::Toggle::Action;
    switch (m_toggle.pressed(m_open, m_asks)) {
    case Action::Close:
        qInfo() << "Folder menu: the hotkey closes it";
        dismiss();
        return;
    case Action::Drop:
        ++m_asks; // the answer on its way is dropped
        qInfo() << "Folder menu: the hotkey again while asking; dropped";
        return;
    case Action::Open:
        break;
    }
    if (m_blocked && m_blocked())
        return;
    const HWND foreground = ::GetForegroundWindow();
    HWND root = foreground ? folderview::frameOf(foreground) : nullptr;
    if (!root)
        return;
    // The search bar beside a file dialog has the keyboard: the menu is for
    // that dialog, which gets the front back first (ours to give). With a
    // window of ours in front the keys could not be taken anyway: the
    // process has keyboard Raw Input, and Windows then skips its hook (see
    // KeyListener, docs/pitfalls.md).
    if (const HWND dialog = m_dialogOf ? m_dialogOf(root) : nullptr) {
        ::SetForegroundWindow(dialog);
        root = dialog;
    }
    folderview::Hit hit = folderview::classifyWindow(root);
    if (hit.surface == folderview::Surface::None) {
        hit.root = root;
        hit.tab = root;
        const std::optional<filemanager::Kind> manager = filemanager::kindOf(root);
        if (manager == filemanager::Kind::DirectoryOpus)
            hit.surface = folderview::Surface::Opus;
        else if (manager == filemanager::Kind::TotalCommander)
            hit.surface = folderview::Surface::TotalCommander;
        else if (manager == filemanager::Kind::XYplorer)
            hit.surface = folderview::Surface::XYplorer;
        else if (manager == filemanager::Kind::Pulse)
            hit.surface = folderview::Surface::Pulse;
        else if (manager == filemanager::Kind::OneCommander)
            hit.surface = folderview::Surface::OneCommander;
        else if (filedialog::kind(root) != filedialog::Kind::None)
            hit.surface = folderview::Surface::Dialog;
        else
            hit.surface = folderview::Surface::Other;
    }
    hit.pt = hotkeyAnchor(root);
    const int ask = ++m_asks;
    m_toggle.asked(ask);
    const bool recordHistory = m_recordHistory;
    const QStringList history = recordHistory && m_history ? m_history->items() : QStringList();
    qInfo().noquote() << u"Folder menu: hotkey over %1 at %2,%3"_s.arg(QLatin1StringView(nameOf(hit.surface)))
                             .arg(hit.pt.x).arg(hit.pt.y);
    m_worker.post([this, hit, ask, recordHistory, history] {
        if (ask != m_asks)
            return;
        QElapsedTimer clock;
        clock.start();
        // The folder shown there, if the window has one: it is "here" (在此搜索, the favorite).
        QString folder;
        switch (hit.surface) {
        case folderview::Surface::Explorer:
            folder = explorerFolder(hit);
            break;
        case folderview::Surface::Opus:
        case folderview::Surface::TotalCommander:
        case folderview::Surface::XYplorer:
        case folderview::Surface::Pulse: // may wait for it to save what it shows (filemanager::openFolders)
        case folderview::Surface::OneCommander:
            folder = QString::fromStdWString(filemanager::frontFolder(hit.root));
            break;
        case folderview::Surface::Dialog:
            folder = QString::fromStdWString(filedialog::currentFolder(hit.root));
            break;
        case folderview::Surface::Desktop:
        case folderview::Surface::Other:
        case folderview::Surface::None:
            break;
        }
        qInfo().noquote() << u"Folder menu: hotkey, folder \"%1\" %2 ms"_s.arg(folder).arg(clock.elapsed());
        gatherHere(hit, ask, folder, recordHistory, history);
    });
}

// Under the pointer when it is over `root` (and nothing covers it there),
// as a context menu; else a little inside what has the keyboard focus in
// that window (the folder view, a text box), where a keyboard-opened context
// menu goes. Physical pixels; the inset in the monitor's own scale.
POINT FolderMenu::hotkeyAnchor(HWND root) const
{
    POINT cursor {};
    ::GetCursorPos(&cursor);
    // Asked on the GUI thread: only windows of this thread are sent anything by it.
    const HWND under = ::WindowFromPoint(cursor);
    const bool over = under && folderview::frameOf(under) == root;
    GUITHREADINFO info {sizeof info};
    HWND focused = nullptr;
    if (::GetGUIThreadInfo(::GetWindowThreadProcessId(root, nullptr), &info) && info.hwndFocus
        && folderview::frameOf(info.hwndFocus) == root)
        focused = info.hwndFocus;
    POINT topLeft {};
    RECT rect {};
    if (focused && ::GetWindowRect(focused, &rect)) {
        topLeft = {rect.left, rect.top};
    } else {
        ::ClientToScreen(root, &topLeft); // the window's client area
    }
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(::GetDpiForMonitor(::MonitorFromPoint(topLeft, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        dpiX = 96;
    const int inset = static_cast<int>(std::lround(16.0 * dpiX / 96.0)); // 16 logical px
    const menuhotkey::Point at = menuhotkey::anchor(over, {cursor.x, cursor.y}, {topLeft.x, topLeft.y}, inset);
    return {at.x, at.y};
}

// On the worker: the folder a File Explorer view shows, a hosted one's
// (360文件夹) as told by what it shows.
QString FolderMenu::explorerFolder(const folderview::Hit& hit) const
{
    if (hit.hosted)
        return m_hostedFolder ? m_hostedFolder(hit.root, hit.tab) : QString();
    return QString::fromStdWString(filemanager::explorerFolder(hit.tab));
}

// On the worker: the rest of what the menu shows, then the menu.
void FolderMenu::gatherHere(const folderview::Hit& hit, int ask, const QString& folder, bool recordHistory,
    const QStringList& history)
{
    QElapsedTimer clock;
    clock.start();
    Found found;
    found.folder = folder;
    // A group switched off is not gathered at all (GitHub #51).
    if (m_listWindows) {
        for (const filemanager::Folder& f : filemanager::openFolders()) {
            const QString path = QString::fromStdWString(f.path);
            if (!samePath(path, found.folder) && !found.windows.contains(path, Qt::CaseInsensitive))
                found.windows.append(path);
        }
    }
    const qint64 windowsMs = clock.elapsed();
    const bool recent = recordHistory && m_listRecent;
    if (recent && !m_recentKnown) {
        m_recent = recentfolders::recent(history, kMaxRecent + 1);
        m_recentKnown = true;
    }
    if (recent)
        found.recent = m_recent;
    qInfo().noquote() << u"Folder menu: windows %1, ready %2 ms"_s.arg(windowsMs).arg(clock.elapsed());
    QMetaObject::invokeMethod(this, [this, hit, ask, found = std::move(found)] {
        m_toggle.answered(ask); // the hotkey's, maybe; shown or not
        if (ask == m_asks)
            show(hit, found);
    }, Qt::QueuedConnection);
    if (recent) // for the next time, now that this one is up
        m_recent = recentfolders::recent(history, kMaxRecent + 1);
}

void FolderMenu::show(const folderview::Hit& hit, const Found& found)
{
    if (!m_window || (m_blocked && m_blocked()))
        return;
    // Still where it was double-clicked: the menu goes with that window. (A
    // dialog given the front back from its bar may not have it yet.)
    const HWND foreground = ::GetForegroundWindow();
    const HWND front = foreground ? folderview::frameOf(foreground) : nullptr;
    const bool desktop = hit.surface == folderview::Surface::Desktop && folderview::sameDesktop(front, hit.root);
    if (!front || (front != hit.root && !desktop && !(m_dialogOf && m_dialogOf(front) == hit.root))) {
        if (hit.view) // a double click's (the hotkey's have no view)
            m_worker.post([this, hit, front = QString::fromStdWString(folderview::classChain(front))] {
                journal(u"not-in-front"_s, hit, {}, nullptr, {{u"front"_s, front}});
            });
        return;
    }
    m_hit = hit;
    m_folder = found.folder;
    if (m_folder.isEmpty() && hit.surface == folderview::Surface::Desktop)
        m_folder = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    populate(m_folder, found.recent, found.windows);

    // Where the second press was (or the hotkey's anchor): its top-left
    // corner there, as a context menu's. In Qt's pixels on the screen it is
    // on (the same origin in both).
    QScreen* screen = screenAt(hit.pt);
    if (!screen)
        return;
    const QPoint origin = screen->geometry().topLeft();
    const qreal scale = screen->devicePixelRatio();
    const QPointF pos(origin.x() + (hit.pt.x - origin.x()) / scale, origin.y() + (hit.pt.y - origin.y()) / scale);
    emit popupRequested(pos);
    if (!m_window->isVisible())
        return;
    m_open = true;
    emit openChanged(true);
    startKeys(hit.root);
}

void FolderMenu::previewMenu(const QString& folder, const QStringList& recent, const QStringList& windows)
{
    m_hit = {};
    m_folder = folder;
    populate(m_folder, recent, windows);
}

void FolderMenu::populate(const QString& folder, const QStringList& recent, const QStringList& windows)
{
    m_entries.clear();
    m_actions.clear();
    m_hasFavorites = false;
    m_favoritePrimary.clear();
    m_favoriteApps.clear();
    m_afterFavoriteGroups.clear();
    m_folderActionEntries.clear();
    m_settingsEntries.clear();
    ++m_shown;
    const auto action = [this](Kind kind, const QString& path, const QString& text, const QString& detail,
                            const QString& glyph, const QString& icon = QString(), const QString& tooltip = QString()) {
        const int index = static_cast<int>(m_actions.size());
        m_actions.push_back({kind, path, text});
        QVariantMap row {{u"text"_s, text}, {u"detail"_s, detail}, {u"glyph"_s, glyph},
            {u"icon"_s, icon}, {u"tooltip"_s, tooltip}, {u"action"_s, index}};
        if (kind == Kind::Folder)
            row.insert(u"folder"_s, path); // its row opens the folders inside it (#170)
        return row;
    };
    if (m_listFavorites) {
        m_hasFavorites = true;
        const auto apps = m_apps ? m_apps->apps() : nullptr;
        const auto places = m_places ? m_places->places() : nullptr;
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        for (const Favorite& f : m_favorites ? m_favorites->items() : FavoriteList()) {
            if (!includesKind(m_favoriteKinds, f.kind))
                continue;
            QString icon;
            QString glyph = u"\uE734"_s;
            QString tooltip = f.path;
            switch (f.kind) {
            case Favorite::Kind::Folder:
            case Favorite::Kind::File:
                icon = FileIconProvider::iconUrl(f.path, f.kind == Favorite::Kind::Folder);
                break;
            case Favorite::Kind::Command:
                glyph = u"\uE756"_s;
                tooltip = f.command;
                icon = FileIconProvider::commandIconUrl(f.command, f.workingDirectory.isEmpty() ? folder : f.workingDirectory,
                    f.icon, dark);
                break;
            case Favorite::Kind::App: {
                const AppInfo* app = apps ? findApp(*apps, f.path) : nullptr;
                const bool packaged = app && (app->kind == AppKind::Store || app->kind == AppKind::System || app->kind == AppKind::Package);
                icon = FileIconProvider::appIconUrl(appIdOf(f.path), packaged, dark);
                break;
            }
            case Favorite::Kind::Place:
                glyph = u"\uE713"_s;
                if (const PlaceInfo* place = places ? findPlace(*places, f.path) : nullptr) {
                    icon = place->icon.startsWith(u"app:") ? FileIconProvider::appIconUrl(place->icon.mid(4), true, dark)
                                                           : FileIconProvider::placeIconUrl(place->icon, dark);
                    tooltip = place->command;
                }
                break;
            }
            QVariantMap row = action(Kind::Favorite, f.path, f.name.isEmpty() ? nameOf(f.path) : f.name, f.keyword,
                glyph, icon, tooltip);
            if (f.kind == Favorite::Kind::Folder)
                row.insert(u"folder"_s, f.path);
            if (f.kind == Favorite::Kind::App)
                m_favoriteApps.append(row);
            else
                m_favoritePrimary.append(row);
        }
    }
    const auto addList = [&](const QString& text, const QStringList& folders, qsizetype max, const QString& glyph, int inlineCount,
                             bool onlyWithSome) {
        QVariantList children;
        QStringList listed;
        if (!folder.isEmpty())
            listed.append(folder);
        for (const QString& item : folders) {
            if (children.size() >= max || listed.contains(item, Qt::CaseInsensitive))
                continue;
            children.append(action(Kind::Folder, item, nameOf(item), {}, u"\uE8B7"_s,
                FileIconProvider::iconUrl(item, true), item));
            listed.append(item);
        }
        if (children.isEmpty() && onlyWithSome)
            return;
        m_afterFavoriteGroups.append(groupEntries(text, glyph, children, inlineCount));
    };
    if (m_listRecent && m_recordHistory)
        addList(tr("最近的文件夹"), recent, kMaxRecent, u"\uE81C"_s, m_recentInline, false);
    // The folder it is in is not among them: with no other open, no "0" that
    // reads as if none were found (GitHub #393).
    if (m_listWindows)
        addList(tr("已打开的文件夹"), windows, windows.size(), u"\uE8A7"_s, m_windowsInline, true);
    if (!folder.isEmpty()) {
        if (m_itemSearch)
            m_folderActionEntries.append(action(Kind::Search, folder, tr("在“%1”里搜索").arg(nameOf(folder)), {}, u"\uE721"_s));
        if (m_itemCopyPath) // plain text, no quotes (#137); not offered where there is no folder path
            m_folderActionEntries.append(action(Kind::CopyPath, folder, tr("复制路径"), {}, u"\uE8C8"_s));
        if (m_itemCmd)
            m_folderActionEntries.append(action(Kind::Terminal, folder, tr("在此打开命令提示符"), {}, u"\uE756"_s));
        if (m_itemPowerShell)
            m_folderActionEntries.append(action(Kind::PowerShell, folder, tr("在此打开 PowerShell"), {}, u"\uE756"_s));
        if (m_itemFavorite) {
            if (m_favorites && m_favorites->contains(folder))
                m_folderActionEntries.append(action(Kind::RemoveFavorite, folder, tr("从收藏夹移除这个文件夹"), {}, u"\uE8D9"_s));
            else
                m_folderActionEntries.append(action(Kind::AddFavorite, folder, tr("收藏这个文件夹"), {}, u"\uE734"_s));
        }
    }
    if (m_itemSettings)
        m_settingsEntries.append(action(Kind::Settings, {}, tr("Win顺 设置…"), {}, u"\uE713"_s));
    rebuildEntries();
}

void FolderMenu::rebuildEntries()
{
    m_entries.clear();
    const auto appendSection = [this](const QVariantList& section) {
        if (section.isEmpty())
            return;
        if (!m_entries.isEmpty())
            m_entries.append(QVariantMap {{u"separator"_s, true}});
        m_entries.append(section);
    };
    QList<QVariantList> groups;
    if (m_hasFavorites)
        groups.append(favoriteGroupEntries(tr("收藏夹"), u"\uE734"_s, m_favoritePrimary, m_favoriteApps, m_appsExpanded, m_favoritesInline));
    groups.append(m_afterFavoriteGroups);
    m_entries = assembleGroups(groups);
    appendSection(m_folderActionEntries);
    appendSection(m_settingsEntries);
    emit entriesChanged();
}

void FolderMenu::toggleSection(const QString& id)
{
    if (id != u"apps")
        return;
    m_appsExpanded = !m_appsExpanded;
    emit appsExpandedRequested(m_appsExpanded);
    if (m_hasFavorites)
        rebuildEntries();
}

void FolderMenu::startKeys(HWND target)
{
    stopKeys();
    const std::uint64_t run = ++m_routerRun;
    const auto post = [this, run](auto call) {
        QMetaObject::invokeMethod(this, [this, run, call = std::move(call)] {
            if (m_router && m_routerRun == run && m_open)
                call();
        }, Qt::QueuedConnection);
    };
    KeyRouter::Callbacks callbacks;
    callbacks.key = [this, post](const KeyRouter::Key& key) {
        if (key.press)
            post([this, key] { emit keyPressed(key.key, key.text); });
    };
    callbacks.clickedAway = [this, post](HWND) { post([this] { dismiss(); }); };
    callbacks.foregroundChanged = [this, post](HWND window) {
        post([this, window] {
            // File Explorer's own going to another folder says so too: by
            // what is in front now, not by the event.
            const HWND foreground = ::GetForegroundWindow();
            if (window != m_hit.root && (!foreground || folderview::frameOf(foreground) != m_hit.root))
                dismiss();
        });
    };
    callbacks.drained = [this] {
        QMetaObject::invokeMethod(this, [this] { reapDrainingKeys(); }, Qt::QueuedConnection);
    };
    auto router = std::make_unique<KeyRouter>(target, reinterpret_cast<HWND>(m_window->winId()), std::move(callbacks),
        Qt::NoModifier, 0, KeyRouter::Policy::Menu);
    if (!router->isActive()) {
        qWarning() << "Folder menu: no keyboard hook; the mouse only";
        return;
    }
    m_router = std::move(router);
}

void FolderMenu::stopKeys()
{
    if (!m_router)
        return;
    // No key is taken any more; the releases of those it took still are its
    // own, for a moment.
    m_router->drain();
    m_draining.push_back(std::move(m_router));
    reapDrainingKeys();
}

void FolderMenu::reapDrainingKeys()
{
    std::erase_if(m_draining, [](const std::unique_ptr<KeyRouter>& router) {
        return !router || router->isFinished();
    });
}

void FolderMenu::dismiss()
{
    stopKeys();
    if (m_open.exchange(false))
        emit openChanged(false);
    emit hideRequested();
}

void FolderMenu::trigger(int index)
{
    if (index < 0 || index >= static_cast<int>(m_actions.size()))
        return;
    const Entry entry = m_actions[static_cast<size_t>(index)];
    if (entry.path.isEmpty() && entry.kind != Kind::Settings)
        return;
    const QString failed = entry.kind == Kind::File ? tr("无法打开“%1”").arg(entry.title)
                                                    : tr("无法打开“%1”，请检查命令和工作目录").arg(entry.title);
    const auto done = [guard = QPointer(this), failed](bool ok) {
        if (!ok) {
            QMetaObject::invokeMethod(qApp, [guard, failed] {
                if (guard)
                    emit guard->notice(failed);
            }, Qt::QueuedConnection);
        }
    };
    dismiss();
    switch (entry.kind) {
    case Kind::Folder:
        goTo(entry.path);
        break;
    case Kind::File:
        if (m_recordHistory && m_history)
            m_history->record(entry.path, QDateTime::currentMSecsSinceEpoch());
        if (m_opener)
            m_opener(entry.path);
        else
            shell::open(entry.path, false, done);
        break;
    case Kind::Search:
        emit searchRequested(entry.path);
        break;
    case Kind::AddFavorite:
        if (m_favorites)
            m_favorites->add({Favorite::Kind::Folder, entry.path, nameOf(entry.path), {}});
        break;
    case Kind::RemoveFavorite:
        if (m_favorites)
            m_favorites->remove(entry.path);
        break;
    case Kind::Terminal:
        shell::openTerminal(entry.path, done);
        break;
    case Kind::PowerShell:
        shell::openPowerShell(entry.path, done);
        break;
    case Kind::Favorite: {
        const auto favoriteIndex = m_favorites ? indexOfFavorite(m_favorites->items(), entry.path) : -1;
        if (favoriteIndex < 0) {
            emit notice(tr("这一项已移除，请重新打开菜单"));
            break;
        }
        const Favorite favorite = m_favorites->items()[favoriteIndex];
        switch (favorite.kind) {
        case Favorite::Kind::Folder:
            goTo(favorite.path);
            break;
        case Favorite::Kind::File:
            shell::open(favorite.path);
            break;
        case Favorite::Kind::App:
            shell::launchApp(favorite.path);
            break;
        case Favorite::Kind::Place: {
            const auto places = m_places ? m_places->places() : nullptr;
            const PlaceInfo* place = places ? findPlace(*places, favorite.path) : nullptr;
            if (!place)
                emit notice(tr("这一项暂不可用，请稍后重试"));
            else if (place->command == kSettingsCommand) // Win顺's own (GitHub #389)
                emit settingsRequested();
            else if (place->command.startsWith(kPluginSettingsCommand))
                emit pluginSettingsRequested(place->command.mid(kPluginSettingsCommand.size()));
            else if (ws::isPowerCommand(place->command)) {
                const auto action = power::parseAction(ws::powerCommandAction(place->command).toString());
                if (action && !ws::powerCommandNeedsConfirm(place->command)) {
                    power::execute(*action, [this, name = place->name](const QString& error) {
                        if (!error.isEmpty())
                            emit notice(tr("没能执行“%1”：%2").arg(name, error));
                    });
                } else {
                    emit notice(tr("请在搜索框中搜索“%1”并二次确认执行").arg(place->name));
                }
            } else
                shell::run(place->command);
            break;
        }
        case Favorite::Kind::Command:
            shell::runCustom(favorite.command, favorite.workingDirectory.isEmpty() ? m_folder : favorite.workingDirectory,
                m_folder, done, {favorite.silent, favorite.runAsAdmin});
            break;
        }
        break;
    }
    case Kind::CopyPath:
        if (const QString text = QDir::toNativeSeparators(entry.path); m_clipboardWriter)
            m_clipboardWriter(text);
        else
            QGuiApplication::clipboard()->setText(text);
        break;
    case Kind::Settings:
        emit settingsRequested();
        break;
    }
}

void FolderMenu::goTo(const QString& folder)
{
    if (m_recordHistory && m_history)
        m_history->record(folder, QDateTime::currentMSecsSinceEpoch());
    if (const std::optional<filemanager::Kind> manager = managerOf(m_hit.surface)) {
        // The window it came from (the latest one used) and its tab in front.
        m_worker.post([kind = *manager, path = QDir::toNativeSeparators(folder)] {
            if (!filemanager::goToInFront(kind, path.toStdWString()))
                shell::open(path);
        });
        return;
    }
    if (m_hit.surface == folderview::Surface::Dialog) {
        // The dialog in front goes there, as Ctrl+G takes it: through the
        // app (in order with its other moves), or by the menu itself.
        const QString path = QDir::toNativeSeparators(folder);
        if (m_dialogGo && m_dialogGo(m_hit.root, path))
            return;
        m_worker.post([dialog = m_hit.root, path] {
            filedialog::waitForFront(dialog);
            if (!filedialog::goTo(dialog, path.toStdWString()))
                qWarning() << "Folder menu: the file dialog did not go to the folder";
        });
        return;
    }
    if (m_hit.surface != folderview::Surface::Explorer || m_hit.hosted) {
        // A hosted view (360文件夹) cannot be sent elsewhere: opened as File
        // Explorer would, it comes in a new tab of that program's.
        if (m_opener)
            m_opener(folder);
        else
            shell::open(folder); // the desktop, any other window: in the file manager chosen
        return;
    }
    // The same tab goes there; one the shell's list lacks (it can miss
    // some): a new window.
    m_worker.post([tab = m_hit.tab, path = QDir::toNativeSeparators(folder)] {
        if (!filemanager::goTo(tab, path.toStdWString())) {
            qWarning() << "Folder menu: tab not found; a new window instead";
            shell::open(path);
        }
    });
}

FolderMenu::Listing FolderMenu::listing(const QString& folder, bool files, qsizetype max)
{
    Listing found;
    std::wstring pattern = QDir::toNativeSeparators(folder).toStdWString();
    if (pattern.empty())
        return found;
    if (pattern.back() != L'\\')
        pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW data;
    const HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
        files ? FindExSearchNameMatch : FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return found;
    do {
        // Hidden ones stay out, as in File Explorer's own default view.
        if (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
            continue;
        const bool isFolder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const std::wstring_view name = data.cFileName;
        if ((isFolder || files) && name != L"." && name != L"..")
            (isFolder ? found.folders : found.files).append(QString::fromWCharArray(name.data(), static_cast<qsizetype>(name.size())));
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    QCollator order; // the user's own order, numbers by value: 2 before 10
    order.setNumericMode(true);
    order.setCaseSensitivity(Qt::CaseInsensitive);
    // The first `max` in order, the rest left out unsorted: a folder of many
    // thousand files is not all sorted for the 500 shown.
    const auto firstInOrder = [&order, max](QStringList& names) {
        const qsizetype kept = std::min(names.size(), max);
        std::ranges::partial_sort(names, names.begin() + kept,
            [&order](const QString& a, const QString& b) { return order.compare(a, b) < 0; });
        const qsizetype left = names.size() - kept;
        names.resize(kept);
        return left;
    };
    firstInOrder(found.folders);
    found.moreFiles = firstInOrder(found.files);
    return found;
}

void FolderMenu::listFolder(const QString& folder)
{
    if (folder.isEmpty() || !m_open)
        return;
    const bool files = m_listFiles && m_hit.surface != folderview::Surface::Dialog;
    // Not on the menu's own worker: a share that does not answer would keep
    // the next menu waiting behind it.
    QThreadPool::globalInstance()->start([guard = QPointer(this), folder, files, shown = m_shown] {
        Listing found = listing(folder, files);
        QMetaObject::invokeMethod(qApp, [guard, folder, files, shown, found = std::move(found)] {
            FolderMenu* const menu = guard.data();
            if (!menu || !menu->m_open || menu->m_shown != shown)
                return;
            const QDir parent(folder);
            QVariantList rows;
            rows.reserve(found.folders.size() + found.files.size() + 1);
            const auto add = [&](Kind kind, const QString& path, const QString& text, QVariantMap row) {
                const int index = static_cast<int>(menu->m_actions.size());
                menu->m_actions.push_back({kind, path, text});
                row.insert(u"text"_s, text);
                row.insert(u"action"_s, index);
                rows.append(row);
            };
            for (const QString& name : found.folders) {
                const QString path = QDir::toNativeSeparators(parent.filePath(name));
                add(Kind::Folder, path, name, {{u"glyph"_s, QString(QChar(0xE8B7))},
                    {u"icon"_s, FileIconProvider::iconUrl(path, true)}, {u"tooltip"_s, path}, {u"folder"_s, path}});
            }
            for (const QString& name : found.files) {
                const QString path = QDir::toNativeSeparators(parent.filePath(name));
                add(Kind::File, path, name, {{u"glyph"_s, QString(QChar(0xE8A5))},
                    {u"icon"_s, FileIconProvider::iconUrl(path, false)}, {u"tooltip"_s, path}});
            }
            if (found.moreFiles > 0) // the rest where they all are
                add(Kind::Folder, QDir::toNativeSeparators(folder), tr("还有 %n 个文件，打开文件夹查看", nullptr, static_cast<int>(found.moreFiles)),
                    {{u"glyph"_s, QString(QChar(0xE712))}});
            else if (files && rows.isEmpty())
                rows.append(QVariantMap {{u"text"_s, tr("空文件夹")}, {u"disabled"_s, true}});
            emit menu->folderListed(folder, rows);
        }, Qt::QueuedConnection);
    });
}

QPoint FolderMenu::cursorPos() const
{
    return m_cursorSource ? m_cursorSource() : QCursor::pos();
}

QRectF FolderMenu::screenArea(QPointF globalPos) const
{
    QScreen* screen = QGuiApplication::screenAt(globalPos.toPoint());
    return screen ? QRectF(screen->availableGeometry()) : QRectF();
}

void FolderMenu::prepareMenuWindow(QWindow* menu) const
{
    if (!menu)
        return;
    // Shown without taking the focus from the window it is for (see the class).
    menu->setProperty("_q_showWithoutActivating", true);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    win::styleFramelessWindow(menu);
    win::setDarkFrame(menu, dark);
}

void FolderMenu::showColumn(QWindow* column)
{
    auto* window = qobject_cast<QQuickWindow*>(column);
    if (!window)
        return;
    if (!window->isVisible()) {
        prepareMenuWindow(window);
        // Cloaked (DWM does not show it) until a frame with what it shows
        // now; a frame that does not come is waited for a moment only.
        win::setCloaked(window, true);
        const std::uint64_t show = ++m_columnShows;
        window->setProperty("_ws_columnShow", QVariant::fromValue(show));
        const auto reveal = [window = QPointer(window), show] {
            if (window && window->isVisible() && window->property("_ws_columnShow").toULongLong() == show)
                win::setCloaked(window, false);
        };
        connect(window, &QQuickWindow::frameSwapped, this, reveal,
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection));
        QTimer::singleShot(250ms, this, reveal);
        window->show();
    }
    window->raise(); // over a column before it it may overlap
}

void FolderMenu::hideColumn(QWindow* column) const
{
    if (!column || !column->isVisible())
        return;
    win::setCloaked(column, true); // gone with the next composition, no fade
    column->hide();
}

namespace {

// The edge Windows draws around a menu window, in the screen's pixels, inside
// the window: DWM's own on Windows 11 (2 px at 150 %), WindowEdge's on
// Windows 10 (Theme.edge).
int frameBorder(HWND window, qreal dpr)
{
    constexpr DWORD kVisibleFrameBorderThickness = 37; // DWMWA_VISIBLE_FRAME_BORDER_THICKNESS, Windows 11
    UINT thickness = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(window, kVisibleFrameBorderThickness, &thickness, sizeof thickness))
        && thickness > 0)
        return static_cast<int>(thickness);
    return std::max(1, qRound(dpr));
}

} // namespace

void FolderMenu::placeColumn(QWindow* column, QWindow* before, QPointF at, int side, qreal rowOffset, bool level,
    QRectF area) const
{
    if (!column)
        return;
    const bool native = before && QGuiApplication::platformName() == u"windows"_s;
    if (!native || !column->handle()) {
        column->setPosition(at.toPoint()); // made there; elsewhere, Qt's pixels are all there is
        if (!native)
            return;
    }
    const auto self = reinterpret_cast<HWND>(column->winId());
    const auto previous = reinterpret_cast<HWND>(before->winId());
    RECT beside {}, own {};
    if (!::GetWindowRect(previous, &beside) || !::GetWindowRect(self, &own)) {
        column->setPosition(at.toPoint());
        return;
    }
    // Qt's pixels to the screen's as Qt maps them: the screen's corner is
    // the same in both.
    const QScreen* screen = before->screen();
    const QPointF origin = screen ? QPointF(screen->geometry().topLeft()) : QPointF();
    const qreal factor = screen ? screen->devicePixelRatio() : before->devicePixelRatio();
    const auto onScreen = [&](QPointF p) {
        return QPoint(qRound(origin.x() + (p.x() - origin.x()) * factor), qRound(origin.y() + (p.y() - origin.y()) * factor));
    };
    const int width = own.right - own.left;
    const int height = own.bottom - own.top;
    QPoint to = onScreen(at);
    const int border = frameBorder(previous, before->devicePixelRatio());
    if (side > 0)
        to.setX(beside.right - border);
    else if (side < 0)
        to.setX(beside.left - width + border);
    if (level)
        to.setY(beside.top + qRound(rowOffset * before->devicePixelRatio()));
    if (area.width() > 0 && area.height() > 0) { // a pixel either way stays inside the work area
        const QPoint low = onScreen(area.topLeft());
        const QPoint high = onScreen(area.bottomRight());
        to.setX(std::max(low.x(), std::min(to.x(), high.x() - width)));
        to.setY(std::max(low.y(), std::min(to.y(), high.y() - height)));
    }
    if (to != QPoint(own.left, own.top)) {
        ::SetWindowPos(self, nullptr, to.x(), to.y(), 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
}

} // namespace ws
