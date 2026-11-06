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
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonObject>
#include <QScreen>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>
#include <QWindow>

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
    if (m_router)
        m_router->drain();
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

void FolderMenu::setInlineCounts(int recent, int windows)
{
    m_recentInline = std::clamp(recent, 0, 10);
    m_windowsInline = std::clamp(windows, 0, 10);
}

QVariantList FolderMenu::groupEntries(const QString& title, const QString& glyph, const QVariantList& children, int inlineCount)
{
    inlineCount = std::clamp(inlineCount, 0, 10);
    QVariantMap group {{u"text"_s, title}, {u"glyph"_s, glyph}, {u"children"_s, children},
        {u"detail"_s, QString::number(children.size())}, {u"disabled"_s, children.isEmpty()}};
    if (inlineCount == 0 || children.isEmpty())
        return {group};
    QVariantList rows {QVariantMap {{u"text"_s, title}, {u"heading"_s, true}, {u"disabled"_s, true}}};
    const qsizetype count = std::min<qsizetype>(inlineCount, children.size());
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
            const QString folder = hit.surface == folderview::Surface::Explorer
                ? QString::fromStdWString(filemanager::explorerFolder(hit.tab))
                : QString();
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
    HWND root = foreground ? ::GetAncestor(foreground, GA_ROOT) : nullptr;
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
            folder = QString::fromStdWString(filemanager::explorerFolder(hit.tab));
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
    const bool over = under && ::GetAncestor(under, GA_ROOT) == root;
    GUITHREADINFO info {sizeof info};
    HWND focused = nullptr;
    if (::GetGUIThreadInfo(::GetWindowThreadProcessId(root, nullptr), &info) && info.hwndFocus
        && ::GetAncestor(info.hwndFocus, GA_ROOT) == root)
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
    const HWND front = foreground ? ::GetAncestor(foreground, GA_ROOT) : nullptr;
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
    m_entries.clear();
    m_actions.clear();
    const auto action = [this](Kind kind, const QString& path, const QString& text, const QString& detail,
                            const QString& glyph, const QString& icon = QString(), const QString& tooltip = QString()) {
        const int index = static_cast<int>(m_actions.size());
        m_actions.push_back({kind, path, text});
        return QVariantMap {{u"text"_s, text}, {u"detail"_s, detail}, {u"glyph"_s, glyph},
            {u"icon"_s, icon}, {u"tooltip"_s, tooltip}, {u"action"_s, index}};
    };
    const auto group = [this](const QString& text, const QString& glyph, const QVariantList& children) {
        m_entries.append(groupEntries(text, glyph, children));
    };
    const auto separator = [this] {
        if (!m_entries.isEmpty() && !m_entries.last().toMap().value(u"separator"_s).toBool()) {
            m_entries.append(QVariantMap {{u"separator"_s, true}});
        }
    };
    if (m_listFavorites) {
        QVariantList favorites;
        const auto apps = m_apps ? m_apps->apps() : nullptr;
        const auto places = m_places ? m_places->places() : nullptr;
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        for (const Favorite& f : m_favorites ? m_favorites->items() : FavoriteList()) {
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
                icon = FileIconProvider::commandIconUrl(f.command, f.workingDirectory.isEmpty() ? m_folder : f.workingDirectory,
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
                                                           : FileIconProvider::placeIconUrl(place->icon);
                    tooltip = place->command;
                }
                break;
            }
            favorites.append(action(Kind::Favorite, f.path, f.name.isEmpty() ? nameOf(f.path) : f.name, f.keyword,
                glyph, icon, tooltip));
        }
        group(tr("收藏夹"), u"\uE734"_s, favorites);
    }
    const auto addList = [&](const QString& text, const QStringList& folders, qsizetype max, const QString& glyph, int inlineCount) {
        QVariantList children;
        QStringList listed;
        if (!m_folder.isEmpty())
            listed.append(m_folder);
        for (const QString& folder : folders) {
            if (children.size() >= max || listed.contains(folder, Qt::CaseInsensitive))
                continue;
            children.append(action(Kind::Folder, folder, nameOf(folder), {}, u"\uE8B7"_s,
                FileIconProvider::iconUrl(folder, true), folder));
            listed.append(folder);
        }
        m_entries.append(groupEntries(text, glyph, children, inlineCount));
    };
    if (m_listRecent && m_recordHistory)
        addList(tr("最近的文件夹"), found.recent, kMaxRecent, u"\uE81C"_s, m_recentInline);
    if (m_listWindows)
        addList(tr("已打开的文件夹"), found.windows, found.windows.size(), u"\uE8A7"_s, m_windowsInline);
    if (!m_folder.isEmpty()) {
        separator();
        if (m_itemSearch)
            m_entries.append(action(Kind::Search, m_folder, tr("在“%1”里搜索").arg(nameOf(m_folder)), {}, u"\uE721"_s));
        if (m_itemCopyPath) // plain text, no quotes (#137); not offered where there is no folder path
            m_entries.append(action(Kind::CopyPath, m_folder, tr("复制路径"), {}, u"\uE8C8"_s));
        if (m_itemCmd)
            m_entries.append(action(Kind::Terminal, m_folder, tr("在此打开命令提示符"), {}, u"\uE756"_s));
        if (m_itemPowerShell)
            m_entries.append(action(Kind::PowerShell, m_folder, tr("在此打开 PowerShell"), {}, u"\uE756"_s));
        if (m_itemFavorite) {
            if (m_favorites && m_favorites->contains(m_folder))
                m_entries.append(action(Kind::RemoveFavorite, m_folder, tr("从收藏夹移除这个文件夹"), {}, u"\uE8D9"_s));
            else
                m_entries.append(action(Kind::AddFavorite, m_folder, tr("收藏这个文件夹"), {}, u"\uE734"_s));
        }
    }
    if (m_itemSettings) {
        separator();
        m_entries.append(action(Kind::Settings, {}, tr("Win顺 设置…"), {}, u"\uE713"_s));
    } else if (!m_entries.isEmpty() && m_entries.last().toMap().value(u"separator"_s).toBool()) {
        m_entries.removeLast(); // no action left to stand under it
    }
    emit entriesChanged();

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
    callbacks.clickedAway = [this, post] { post([this] { dismiss(); }); };
    callbacks.foregroundChanged = [this, post](HWND window) {
        post([this, window] {
            // File Explorer's own going to another folder says so too: by
            // what is in front now, not by the event.
            const HWND foreground = ::GetForegroundWindow();
            if (window != m_hit.root && (!foreground || ::GetAncestor(foreground, GA_ROOT) != m_hit.root))
                dismiss();
        });
    };
    callbacks.drained = [this, run] {
        QMetaObject::invokeMethod(this, [this, run] {
            if (m_drainingRun == run)
                m_draining.reset();
        }, Qt::QueuedConnection);
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
    m_draining = std::move(m_router);
    m_drainingRun = m_routerRun;
    QTimer::singleShot(2s, this, [this, run = m_drainingRun] {
        if (m_drainingRun == run)
            m_draining.reset(); // a key held that long is the program's again
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
    const QString title = entry.title;
    const auto done = [guard = QPointer(this), title](bool ok) {
        if (!ok) {
            QMetaObject::invokeMethod(qApp, [guard, title] {
                if (guard)
                    emit guard->notice(tr("无法打开“%1”，请检查命令和工作目录").arg(title));
            }, Qt::QueuedConnection);
        }
    };
    dismiss();
    switch (entry.kind) {
    case Kind::Folder:
        goTo(entry.path);
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
            if (const PlaceInfo* place = places ? findPlace(*places, favorite.path) : nullptr)
                shell::run(place->command);
            else
                emit notice(tr("这一项暂不可用，请稍后重试"));
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
    if (m_hit.surface != folderview::Surface::Explorer) {
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

} // namespace ws
