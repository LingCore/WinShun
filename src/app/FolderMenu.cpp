#include "FolderMenu.h"

#include "DiagnosticLog.h"
#include "Favorites.h"
#include "History.h"
#include "RecentFolders.h"
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
#include <QGuiApplication>
#include <QJsonObject>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>
#include <QWindow>

#include <shellscalingapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <cmath>
#include <cwchar>
#include <optional>
#include <utility>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr int kMaxFavorites = 12;
constexpr int kMaxRecent = 5;
constexpr int kMaxWindows = 5;
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

// Where it is: its parent folder, shortened from the left.
QString detailOf(const QString& folder)
{
    const qsizetype slash = folder.lastIndexOf(u'\\');
    if (slash < 0 || slash == folder.size() - 1)
        return {};
    QString parent = folder.left(slash == 2 ? 3 : slash);
    if (parent.size() > 40)
        parent = u"…"_s + parent.right(39);
    return parent;
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

void FolderMenu::doubleClicked(HWND window, POINT pt)
{
    // On the listener thread: class names only, nothing that waits.
    const folderview::Hit hit = folderview::classify(window, pt, true);
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
        if (trace->msaa.empty())
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
    if (!front || (front != hit.root && !(m_dialogOf && m_dialogOf(front) == hit.root))) {
        if (hit.view) // a double click's (the hotkey's have no view)
            m_worker.post([this, hit, front = QString::fromStdWString(folderview::classChain(front))] {
                journal(u"not-in-front"_s, hit, {}, nullptr, {{u"front"_s, front}});
            });
        return;
    }
    m_hit = hit;
    m_folder = found.folder;
    m_entries.clear();
    m_actions.clear();
    const auto add = [this](Kind kind, const QString& path, const QString& text, const QString& detail,
                         const QString& glyph) {
        m_entries.append(QVariantMap {{u"text"_s, text}, {u"detail"_s, detail}, {u"glyph"_s, glyph}});
        m_actions.push_back({kind, path});
    };
    const auto separator = [this] {
        if (!m_entries.isEmpty() && !m_entries.last().toMap().value(u"separator"_s).toBool()) {
            m_entries.append(QVariantMap {{u"separator"_s, true}});
            m_actions.push_back({});
        }
    };
    QStringList listed;
    if (!m_folder.isEmpty())
        listed.append(m_folder); // where it is already
    const bool listFavorites = m_listFavorites;
    int favorites = 0;
    for (const Favorite& f : listFavorites && m_favorites ? m_favorites->items() : FavoriteList()) {
        if (f.kind != Favorite::Kind::Folder || favorites >= kMaxFavorites || listed.contains(f.path, Qt::CaseInsensitive))
            continue;
        add(Kind::Folder, f.path, f.name.isEmpty() ? nameOf(f.path) : f.name, f.keyword.isEmpty() ? detailOf(f.path) : f.keyword,
            u"\uE734"_s); // FavoriteStar
        listed.append(f.path);
        ++favorites;
    }
    if (listFavorites && favorites == 0) {
        m_entries.append(QVariantMap {{u"text"_s, tr("还没有收藏的文件夹")}, {u"detail"_s, QString()},
            {u"glyph"_s, u"\uE734"_s}, {u"disabled"_s, true}});
        m_actions.push_back({});
    }
    const auto addList = [&](const QStringList& folders, int max, const QString& glyph) {
        bool first = true;
        int count = 0;
        for (const QString& folder : folders) {
            if (count >= max || listed.contains(folder, Qt::CaseInsensitive))
                continue;
            if (std::exchange(first, false))
                separator();
            add(Kind::Folder, folder, nameOf(folder), detailOf(folder), glyph);
            listed.append(folder);
            ++count;
        }
    };
    addList(found.recent, kMaxRecent, u"\uE81C"_s); // History
    addList(found.windows, kMaxWindows, u"\uE8A7"_s); // OpenInNewWindow
    if (m_entries.isEmpty() && m_folder.isEmpty()) { // only groups with nothing in them: not a blank menu
        m_entries.append(QVariantMap {{u"text"_s, tr("\u6CA1\u6709\u53EF\u5217\u51FA\u7684\u6587\u4EF6\u5939")}, {u"detail"_s, QString()},
            {u"glyph"_s, u"\uE81C"_s}, {u"disabled"_s, true}});
        m_actions.push_back({});
    }
    if (!m_folder.isEmpty()) {
        separator();
        add(Kind::Search, m_folder, tr("在“%1”里搜索").arg(nameOf(m_folder)), QString(), u"\uE721"_s); // Search
        if (m_favorites && m_favorites->contains(m_folder))
            add(Kind::RemoveFavorite, m_folder, tr("从收藏夹移除这个文件夹"), QString(), u"\uE8D9"_s); // Unfavorite
        else
            add(Kind::AddFavorite, m_folder, tr("收藏这个文件夹"), QString(), u"\uE734"_s);
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
        Qt::NoModifier, KeyRouter::Policy::Menu);
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
    if (entry.path.isEmpty())
        return;
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
