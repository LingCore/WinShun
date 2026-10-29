#include "DialogBar.h"

#include "PathText.h"
#include "Query.h"
#include "RecentFolders.h"
#include "SearchEngine.h"
#include "platform/Integrity.h"
#include "platform/DialogOwner.h"
#include "platform/WindowEffects.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QScreen>
#include <QStyleHints>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

using recentfolders::LocalDrives;

constexpr int kGap = 4; // logical pixels between the dialog and the bar
constexpr int kMinWidth = 480;
constexpr int kSideWidth = 480; // beside the dialog
constexpr int kMinDialogHeight = 400; // made shorter down to this (logical pixels), then moved up
constexpr int kMinDialogWidth = 640; // made narrower down to this, then moved aside
constexpr int kMaxExplorer = 5; // suggestions of each kind
constexpr int kMaxRecent = 10;
constexpr int kMaxListed = 200; // of a folder whose path is typed

int minimumPanelHeight()
{
    return DialogBar::metrics().barHeight + DialogBar::kListChrome + 2 * DialogBar::metrics().barRow;
}

// The tag of a folder open in a file manager. The others by their own names.
QString managerName(filemanager::Kind kind)
{
    switch (kind) {
    case filemanager::Kind::TotalCommander:
        return u"Total Commander"_s;
    case filemanager::Kind::DirectoryOpus:
        return u"Directory Opus"_s;
    case filemanager::Kind::XYplorer:
        return u"XYplorer"_s;
    case filemanager::Kind::Pulse:
        return u"Pulse"_s;
    case filemanager::Kind::OneCommander:
        return u"OneCommander"_s;
    case filemanager::Kind::Custom: // cannot be asked: never among them
    case filemanager::Kind::Explorer:
        break;
    }
    return DialogBar::tr("资源管理器");
}

QScreen* screenOf(HMONITOR monitor)
{
    for (QScreen* screen : QGuiApplication::screens()) {
        const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            return screen;
    }
    return QGuiApplication::primaryScreen();
}

// A window's visible frame and the work area of its screen, in physical pixels.
struct Placing {
    RECT frame;
    RECT work;
    QScreen* screen;
};

std::optional<Placing> placingOf(HWND hwnd)
{
    RECT frame {};
    if (FAILED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof frame)))
        ::GetWindowRect(hwnd, &frame);
    const HMONITOR monitor = ::MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(monitor, &info))
        return std::nullopt;
    return Placing {frame, info.rcWork, screenOf(monitor)};
}

// The bar and at least two rows fit on that side of the dialog as it is.
// Beside it, the bar goes up as far as the list needs.
bool fitsBy(const Placing& at, DialogBar::Place side)
{
    const qreal scale = at.screen->devicePixelRatio();
    switch (side) {
    case DialogBar::Place::Left:
        return at.frame.left - qRound((kGap + kSideWidth) * scale) >= at.work.left;
    case DialogBar::Place::Right:
        return at.frame.right + qRound((kGap + kSideWidth) * scale) <= at.work.right;
    default:
        return at.frame.bottom + qRound((kGap + minimumPanelHeight()) * scale) <= at.work.bottom;
    }
}

// Where the bar goes now: the side set if it fits there (below: while the
// bar itself fits, the list opening upward), else where it is if that still
// fits (no going back and forth as the dialog moves), else the first that
// fits of below, right and left; below if none does.
DialogBar::Place sideFor(const Placing& at, DialogBar::Place set, DialogBar::Place current)
{
    const qreal scale = at.screen->devicePixelRatio();
    return dialogbar::chooseSide(at.work.bottom - at.frame.bottom, at.frame.left - at.work.left,
        at.work.right - at.frame.right, qRound((kGap + minimumPanelHeight()) * scale),
        qRound(kGap * scale) + qRound(DialogBar::metrics().barHeight * scale), // as place() puts it
        qRound((kGap + kSideWidth) * scale), qRound(24 * scale), set, current);
}

// Along one axis: the frame from `first` to `last`, the work area from
// `start` to `end`, and `room` wanted between `last` and `end`. Shorter
// first, `first` staying, down to `minimum`; then moved toward `start`. The
// new `first` and `last`. (Negated, for room before `first`.)
std::pair<int, int> roomAlong(int first, int last, int start, int end, int room, int minimum, bool resizable)
{
    int length = last - first;
    if (resizable)
        length = std::min({length, std::max(end - room - first, minimum), end - start - room});
    const int moved = std::max(start, std::min(first, end - room - length));
    return {moved, moved + length};
}

// "C:\Users\me\file.txt" -> "C:\Users\me"; "C:\file.txt" -> "C:\".
QString parentOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'\\');
    return slash < 0 ? QString() : path.left(slash == 2 ? 3 : slash);
}

QString nameOf(const QString& path)
{
    const QString trimmed = path.endsWith(u'\\') && path.size() > 3 ? path.chopped(1) : path;
    const qsizetype slash = trimmed.lastIndexOf(u'\\');
    return slash < 0 || slash == trimmed.size() - 1 ? trimmed : trimmed.mid(slash + 1);
}

QString withSlash(const QString& folder)
{
    return folder.endsWith(u'\\') ? folder : folder + u'\\';
}

DWORD attributesOf(const QString& path)
{
    return ::GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()));
}

// The path on the clipboard: of the (first) file or folder copied in File
// Explorer, or one copied as text if the text is short (a path, not a book).
// Nothing while another program holds the clipboard: no waiting for it.
QString clipboardPath()
{
    const bool files = ::IsClipboardFormatAvailable(CF_HDROP);
    if ((!files && !::IsClipboardFormatAvailable(CF_UNICODETEXT)) || !::OpenClipboard(nullptr))
        return {};
    QString path;
    if (files) {
        if (const auto drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP))) {
            std::wstring name(::DragQueryFileW(drop, 0, nullptr, 0) + 1, L'\0');
            name.resize(::DragQueryFileW(drop, 0, name.data(), static_cast<UINT>(name.size())));
            path = pathtext::pathFromText(QString::fromStdWString(name));
        }
    } else if (const HANDLE data = ::GetClipboardData(CF_UNICODETEXT); data && ::GlobalSize(data) <= 64 * 1024) {
        if (const auto* chars = static_cast<const wchar_t*>(::GlobalLock(data))) {
            path = pathtext::pathFromText(QString::fromWCharArray(
                chars, static_cast<qsizetype>(::wcsnlen(chars, ::GlobalSize(data) / sizeof(wchar_t)))));
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();
    return path;
}

// The program file of a window's process, "notepad.exe".
QString programOf(HWND hwnd)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const bool ok = ::QueryFullProcessImageNameW(process, 0, path.data(), &size);
    ::CloseHandle(process);
    if (!ok)
        return {};
    path.resize(size);
    return QString::fromStdWString(path.substr(path.find_last_of(L'\\') + 1));
}

} // namespace

DialogBar::DialogBar(SearchEngine* engine, History* history, Favorites* favorites, Mover mover, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_history(history)
    , m_favorites(favorites)
    , m_mover(std::move(mover))
{
    s_instance = this;
    if (m_favorites) {
        connect(m_favorites, &Favorites::changed, this, [this] {
            if (!m_window || !m_window->isVisible())
                return;
            if (m_query.trimmed().isEmpty())
                lookAround(); // which are there, then the list
            else
                relist(); // the favorites first
        });
    }
    connect(m_engine, &SearchEngine::resultsReady, this,
        [this](quint64 id, const SearchResults& results, qint64, qint64) { onResults(id, results); });
    m_listHold.setSingleShot(true);
    m_listHold.setInterval(60ms);
    connect(&m_listHold, &QTimer::timeout, this, [this] { // not clicked into
        if (!m_listHeld || m_clickSeen)
            return;
        m_listHeld = false;
        if (m_window && m_window->isVisible())
            place();
    });
    m_moveTimeout.setSingleShot(true);
    m_moveTimeout.setInterval(300ms); // it is busy, or keeps to a size of its own
    connect(&m_moveTimeout, &QTimer::timeout, this, [this] {
        m_movingTo.reset();
        if (m_inFront)
            follow();
    });
}

DialogBar::~DialogBar()
{
    if (s_instance == this)
        s_instance = nullptr;
}

void DialogBar::setWindow(QQuickWindow* window)
{
    m_window = window;
    m_visibility.setWindow(window, [this](bool shown) {
        if (shown)
            showLogo();
        else
            m_logo.hide();
    });
    // The list opens while the bar has the focus: what is offered, as of now.
    connect(window, &QWindow::activeChanged, this, [this] {
        if (!m_window)
            return;
        // Clicked into: the list waits for the button to come up. Opening
        // above the box, it moves the box down before the window has moved
        // up, and the click would land on a row. The click comes just after
        // the window is active; without one, the list opens a moment later.
        m_listHeld = m_window->isActive();
        m_clickSeen = false;
        if (m_listHeld)
            m_listHold.start();
        else
            m_listHold.stop();
        if (m_window->isActive() && m_query.trimmed().isEmpty()) {
            showSuggestions();
            lookAround();
        }
        if (m_window->isVisible())
            place();
    });
    window->installEventFilter(this); // the button coming up
}

void DialogBar::setExcludedApps(const QStringList& apps)
{
    m_excludedApps = apps;
    if (m_inFront)
        follow();
}

void DialogBar::setPlace(const QString& name)
{
    const Place place = name == u"below" ? Place::Below
        : name == u"left"                ? Place::Left
        : name == u"right"               ? Place::Right
                                         : Place::Auto;
    if (place == m_place)
        return;
    m_place = place;
    if (m_window && m_window->isVisible())
        this->place();
}

bool DialogBar::hidden() const
{
    return m_dialog
        && (m_dialog == m_hiddenUnder
            || (!m_dialogApp.isEmpty() && m_excludedApps.contains(m_dialogApp, Qt::CaseInsensitive)));
}

bool DialogBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && m_listHeld) {
        if (event->type() == QEvent::MouseButtonPress) {
            m_clickSeen = true;
            m_listHold.stop();
        } else if (event->type() == QEvent::MouseButtonRelease) {
            m_listHeld = false;
            QMetaObject::invokeMethod(this, [this] { // once the click is through
                if (m_window && m_window->isVisible())
                    place();
            }, Qt::QueuedConnection);
        }
    }
    return QObject::eventFilter(watched, event);
}

bool DialogBar::isShown() const
{
    return m_window && m_window->isVisible() && m_dialog;
}

void DialogBar::setDialog(HWND dialog)
{
    m_inFront = dialog != nullptr;
    m_dragged = false; // let go of while not in front: no word of it comes
    if (!dialog) {
        hideWindow(); // the same dialog coming back finds the bar as it was
        return;
    }
    if (dialog != m_dialog) {
        m_dialog = dialog;
        m_dialogApp = programOf(dialog);
        m_side = m_place == Place::Auto ? Place::Below : m_place;
        m_movingTo.reset();
        m_moveTimeout.stop();
        const filedialog::Kind kind = filedialog::kind(dialog);
        const bool elevated = integrity::higherThanOurs(dialog);
        const bool copyOnly = elevated && !m_elevatedNavAvailable;
        if (kind != m_kind || copyOnly != m_copyOnly || elevated != m_elevated) {
            m_kind = kind;
            m_copyOnly = copyOnly;
            m_elevated = elevated;
            emit dialogKindChanged();
        }
        m_pathCopied = false;
        emit copyStateChanged();
        emit originChanged(); // that of another dialog, if any
        m_extensions.clear();
        m_requestId = 0;
        m_listPending = false;
        m_held.reset();
        if (!m_query.isEmpty()) {
            m_query.clear();
            emit queryChanged();
        }
        m_results.clear();
        m_tags.clear();
        m_nothingFound = false;
        emit resultsReplaced();
    }
    if (!hidden())
        lookAround(); // Explorer may show another folder by now
    updateAsking();
    follow();
}

void DialogBar::dialogMoved()
{
    if (m_inFront)
        follow();
}

void DialogBar::dialogRestacked()
{
    // Its thread put windows in another Z-order, which it does often (a click
    // into it, a menu): a whole follow() only when the bar is no longer right
    // above it, showing (the dialog came up over it, #95), or no longer in the
    // band followDialog gives it (topmost only by an elevated or topmost
    // dialog: the dialog may have left that band). Moves, shows and hides
    // come as events of their own.
    const auto topmost = [](HWND hwnd) { return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; };
    const HWND bar = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    if (m_inFront && bar && m_visibility.isRevealed() && !m_dragged && !m_movingTo
        && ::GetWindow(m_dialog, GW_HWNDPREV) == bar && topmost(bar) == (m_elevated || topmost(m_dialog)))
        return;
    dialogMoved();
}

void DialogBar::dialogDragged(bool dragged)
{
    m_dragged = dragged;
    if (dragged)
        hideWindow();
    else if (m_inFront)
        follow(); // where it was let go
}

void DialogBar::setOrigin(HWND dialog, const QString& from)
{
    m_originDialog = dialog;
    m_origin = from;
    emit originChanged();
}

void DialogBar::follow()
{
    if (!m_window || !m_dialog)
        return;
    // A dialog comes to the front before it shows; it may be minimised.
    if (m_dragged || hidden() || !::IsWindow(m_dialog) || !::IsWindowVisible(m_dialog) || ::IsIconic(m_dialog)) {
        hideWindow();
        return;
    }
    makeRoom();
    if (m_restoring == m_dialog && ::IsZoomed(m_dialog) && m_moveTimeout.isActive()) {
        hideWindow(); // till it is restored
        return;
    }
    if (m_movingTo) { // the bar shows where it goes once the dialog is there
        const std::optional<Placing> at = placingOf(m_dialog);
        if (at && !::EqualRect(&at->frame, &*m_movingTo)) {
            hideWindow();
            return;
        }
        m_movingTo.reset();
        m_moveTimeout.stop();
    }
    place();
    showWindow();
}

// Once for each dialog: should the bar and two rows not fit by it on its
// screen (Place), it gets smaller there, its far edge staying: shorter for
// below, down to kMinDialogHeight; narrower for a side, down to
// kMinDialogWidth. Then it moves for the rest. Windows keeps a dialog's size
// for each program, so the next one comes up fitting. Maximised, it stays,
// unless it has no button to restore it with: Windows keeps that for the
// program too (once one was snapped to the top of the screen, say), and each
// of its dialogs comes up so, no edge to be dragged and the bar over its
// title. Restored first, then made room by. What the user does with it after
// stays.
void DialogBar::makeRoom()
{
    if (m_elevated) // never resize or move an elevated dialog's window across UIPI
        return;
    std::erase_if(m_roomMade, [](HWND dialog) { return !::IsWindow(dialog); });
    if (std::ranges::find(m_roomMade, m_dialog) != m_roomMade.end())
        return;
    if (::IsZoomed(m_dialog)) {
        if (m_restoring == m_dialog && m_moveTimeout.isActive())
            return; // on its way
        if (m_restoring == m_dialog || (::GetWindowLongPtrW(m_dialog, GWL_STYLE) & WS_MAXIMIZEBOX)) {
            m_roomMade.push_back(m_dialog); // the user's to restore, or it would not be
            return;
        }
        // Its size as restored is not known before: the bar shows once it is.
        m_restoring = m_dialog;
        m_moveTimeout.start();
        ::ShowWindowAsync(m_dialog, SW_SHOWNOACTIVATE);
        return;
    }
    m_roomMade.push_back(m_dialog);
    const std::optional<Placing> at = placingOf(m_dialog);
    RECT window {};
    if (!at || !::GetWindowRect(m_dialog, &window))
        return;
    if (m_place == Place::Auto
        && (fitsBy(*at, Place::Below) || fitsBy(*at, Place::Right) || fitsBy(*at, Place::Left)))
        return;
    const Place side = m_place == Place::Auto ? Place::Below : m_place;
    if (fitsBy(*at, side))
        return;
    const RECT& frame = at->frame;
    const RECT& work = at->work;
    const qreal scale = at->screen->devicePixelRatio();
    const auto physical = [scale](int logical) { return qRound(logical * scale); };
    // An XP-style one may not lay itself out again.
    const bool resizable = ::GetWindowLongPtrW(m_dialog, GWL_STYLE) & WS_THICKFRAME;
    // A logical pixel more than fitsBy() wants: a dialog unaware of DPI
    // comes out a physical pixel off what it is given.
    const int below = physical(kGap + minimumPanelHeight() + 1);
    const int beside = physical(kGap + kSideWidth + 1);
    RECT to = frame;
    if (side == Place::Below) {
        std::tie(to.top, to.bottom) = roomAlong(frame.top, frame.bottom, work.top, work.bottom, below,
            physical(kMinDialogHeight), resizable);
    } else if (side == Place::Right) {
        std::tie(to.left, to.right) = roomAlong(frame.left, frame.right, work.left, work.right, beside,
            physical(kMinDialogWidth), resizable);
    } else {
        const auto [right, left] = roomAlong(-frame.right, -frame.left, -work.right, -work.left, beside,
            physical(kMinDialogWidth), resizable);
        to.left = -left;
        to.right = -right;
    }
    if (::EqualRect(&to, &frame))
        return;
    m_movingTo = to;
    m_moveTimeout.start();
    // The window's rect has borders the frame has not: each edge goes as far.
    // Not waiting for the dialog's thread: it moves, the bar follows.
    const int left = window.left + to.left - frame.left;
    const int top = window.top + to.top - frame.top;
    ::SetWindowPos(m_dialog, nullptr, left, top, window.right + to.right - frame.right - left,
        window.bottom + to.bottom - frame.bottom - top,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void DialogBar::toggleFocus()
{
    if (!isShown())
        return;
    if (m_window->isActive()) {
        back();
        return;
    }
    win::bringToFront(m_window);
    m_window->requestActivate();
}

void DialogBar::back()
{
    if (m_dialog && ::IsWindow(m_dialog))
        ::SetForegroundWindow(m_dialog); // ours is in front, so it may hand that on
}

QString DialogBar::explorerName() const
{
    return m_explorer.isEmpty() ? QString() : nameOf(m_explorer.constFirst());
}

QString DialogBar::explorerSource() const
{
    return m_explorerKinds.isEmpty() ? QString() : managerName(m_explorerKinds.constFirst());
}

QString DialogBar::originName() const
{
    return m_originDialog == m_dialog && !m_origin.isEmpty() ? nameOf(m_origin) : QString();
}

void DialogBar::setQuery(const QString& query)
{
    if (m_pathCopied && !query.isEmpty()) {
        m_pathCopied = false;
        emit copyStateChanged();
    }
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    m_held.reset(); // typed on: a key held for the rows before is not for these
    relist();
}

void DialogBar::setResultOptions(bool showModified, bool thumbnails, const QString& numberKeys)
{
    m_results.setThumbnails(thumbnails);
    const QString keys = numberKeys == u"off" ? QString() : numberKeys;
    if (showModified == m_showModified && keys == m_numberKeys)
        return;
    m_showModified = showModified;
    m_numberKeys = keys;
    emit resultOptionsChanged();
}

void DialogBar::setNumberKeysLeft(unsigned digits)
{
    if (digits == m_numberKeysLeft)
        return;
    m_numberKeysLeft = digits;
    emit resultOptionsChanged();
}

void DialogBar::pressNumber(int number)
{
    if (number < 1 || number > 9 || m_numberKeys.isEmpty() || holdUntilShown(number, false))
        return;
    emit numberPressed(number);
}

bool DialogBar::holdUntilShown(int number, bool open)
{
    if (!m_listPending)
        return false;
    m_held = {number, open};
    return true;
}

void DialogBar::relist()
{
    ++m_listings; // one on its way is for what was typed before
    m_listPending = true;
    const pathtext::TypedPath typed = pathtext::splitTyped(m_query);
    if (typed.folder != m_browsed) {
        m_browsed = typed.folder;
        emit browsedFolderChanged();
    }
    if (m_query.trimmed().isEmpty())
        showSuggestions();
    else if (!typed.folder.isEmpty())
        browse(typed);
    else
        search();
}

void DialogBar::search()
{
    SearchEngine::Request request;
    request.text = m_query;
    // A folder picker: folders alone (each its own best, not those of the
    // best matches of all); where to save: folders first.
    request.scope = foldersOnly() ? Scope::Folders : Scope::Files;
    request.kindOrder = m_kind == filedialog::Kind::Save ? KindOrder::FoldersFirst : KindOrder::FilesFirst;
    request.excludedTypes = m_excludedTypes;
    request.excludedFileNames = m_excludedFileNames;
    if (m_recordHistory)
        request.history = m_history->items();
    if (m_favorites)
        request.favorites = m_favorites->items();
    m_requestId = m_engine->submit(std::move(request));
}

bool DialogBar::ofFileType(const SearchResult& r) const
{
    return std::ranges::any_of(m_extensions, [&](const QString& extension) {
        return r.name.size() > extension.size() && r.name[r.name.size() - extension.size() - 1] == u'.'
            && r.name.endsWith(extension, Qt::CaseInsensitive);
    });
}

void DialogBar::onResults(quint64 id, const SearchResults& results)
{
    if (id == 0 || id != m_requestId)
        return; // the launcher's, or superseded
    SearchResults rows;
    for (const SearchResult& r : results) {
        if (!r.isApp() && !r.missing && (r.isDir || !foldersOnly()))
            rows.append(r);
    }
    // Files of the type the Open dialog shows first; where to save (a Save
    // dialog): folders first, files after (to save over); the favorites
    // before all (the engine puts the first few first; here all of them).
    if (!m_extensions.isEmpty())
        std::ranges::stable_partition(rows, [this](const SearchResult& r) { return r.isDir || ofFileType(r); });
    if (m_kind == filedialog::Kind::Save)
        std::ranges::stable_partition(rows, &SearchResult::isDir);
    std::ranges::stable_partition(rows, &SearchResult::favorite);
    QStringList tags;
    for (const SearchResult& r : std::as_const(rows))
        tags.append(r.favorite ? tr("收藏") : QString());
    setRows(std::move(rows), std::move(tags), parseQuery(m_query).highlights);
}

void DialogBar::browse(const pathtext::TypedPath& typed)
{
    m_requestId = 0; // the search's results are for what was typed before
    const int listing = m_listings;
    const bool foldersOnly = this->foldersOnly();
    m_lister.post([this, listing, typed, foldersOnly] {
        if (listing != m_listings)
            return; // typed on
        SearchResults rows = pathtext::listTyped(typed, foldersOnly, kMaxListed);
        QMetaObject::invokeMethod(this, [this, listing, typed, rows = std::move(rows)]() mutable {
            if (listing != m_listings)
                return;
            // Folders first (as Explorer lists them), then files of the Open dialog's type.
            if (!m_extensions.isEmpty())
                std::ranges::stable_partition(rows, [this](const SearchResult& r) { return r.isDir || ofFileType(r); });
            QStringList tags(rows.size());
            if (!rows.isEmpty() && typed.name.isEmpty() && rows.constFirst().path == typed.folder)
                tags.first() = tr("输入的路径");
            setRows(std::move(rows), std::move(tags), parseQuery(typed.name).highlights);
        }, Qt::QueuedConnection);
    });
}

void DialogBar::setRows(SearchResults rows, QStringList tags, const QStringList& highlights, bool refresh)
{
    m_listPending = false;
    // Then a key pressed for these rows: once the view has them (resultsReplaced).
    const auto deliverHeld = qScopeGuard([this] {
        if (const auto held = std::exchange(m_held, std::nullopt))
            emit heldKey(held->first, held->second);
    });
    // Where the dialog is: going there changes nothing.
    const QString here = m_dialog ? QString::fromStdWString(filedialog::currentFolder(m_dialog)) : QString();
    if (!here.isEmpty()) {
        for (qsizetype i = 0; i < rows.size(); ++i) {
            if (rows[i].isDir && pathtext::sameFolder(rows[i].path, here))
                tags[i] = tr("当前位置");
        }
    }
    // A look around that found nothing new: the selection stays.
    bool same = refresh && !m_nothingFound && tags == m_tags && rows.size() == m_results.count();
    for (qsizetype i = 0; same && i < rows.size(); ++i) {
        const SearchResult* shown = m_results.at(static_cast<int>(i));
        same = shown && shown->path == rows[i].path && shown->isDir == rows[i].isDir;
    }
    if (same)
        return;
    m_nothingFound = rows.isEmpty();
    m_tags = std::move(tags);
    m_results.assign(std::move(rows), highlights);
    emit resultsReplaced();
    if (m_window && m_window->isVisible())
        place();
}

void DialogBar::showSuggestions(bool refresh)
{
    m_requestId = 0;
    SearchResults rows;
    QStringList tags;
    QStringList seen;
    const auto add = [&](const QString& path, bool isDir, const QString& tag, bool recent = false, const QString& name = {}) {
        if (seen.contains(path, Qt::CaseInsensitive))
            return;
        seen.append(path);
        SearchResult row;
        row.path = path;
        row.name = name.isEmpty() ? nameOf(path) : name;
        row.isDir = isDir;
        row.recent = recent;
        rows.append(row);
        tags.append(tag);
    };
    for (qsizetype i = 0; i < std::min<qsizetype>(m_explorer.size(), kMaxExplorer); ++i)
        add(m_explorer[i], true, managerName(m_explorerKinds.value(i)));
    if (!m_clipboard.isEmpty())
        add(m_clipboard, m_clipboardIsDir, tr("剪贴板"));
    for (const SearchResult& f : std::as_const(m_favoritesThere)) {
        if (!foldersOnly() || f.isDir)
            add(f.path, f.isDir, tr("收藏"), false, f.name); // as its user named it
    }
    for (const QString& folder : std::as_const(m_recent))
        add(folder, true, tr("最近"), true);
    setRows(std::move(rows), std::move(tags), {}, refresh);
}

void DialogBar::lookAround()
{
    const int lookup = ++m_lookups;
    struct Found {
        QStringList explorer;
        QList<filemanager::Kind> explorerKinds;
        QStringList recent;
        SearchResults favorites;
        QString clipboard;
        bool clipboardIsDir = false;
        QStringList extensions;
    };
    const HWND dialog = m_dialog;
    const bool foldersOnly = this->foldersOnly();
    const bool recordHistory = m_recordHistory;
    QStringList history = m_recordHistory ? m_history->items() : QStringList();
    FavoriteList favorites = m_favorites ? m_favorites->items() : FavoriteList();
    m_worker.post([this, lookup, dialog, foldersOnly, recordHistory, history = std::move(history),
                      favorites = std::move(favorites)] {
        if (lookup != m_lookups)
            return; // another is on its way
        Found found;
        for (const filemanager::Folder& folder : filemanager::openFolders()) {
            found.explorer.append(QString::fromStdWString(folder.path));
            found.explorerKinds.append(folder.shownIn);
        }
        const LocalDrives drives;
        if (recordHistory)
            found.recent = recentfolders::recent(history, kMaxRecent);
        for (const Favorite& f : favorites) { // one on a network drive or share is kept unasked
            if (!f.isFileOrFolder() || (foldersOnly && f.kind != Favorite::Kind::Folder))
                continue;
            const bool isDir = f.kind == Favorite::Kind::Folder;
            if (drives.contain(f.path)) {
                const DWORD attributes = attributesOf(f.path);
                if (attributes == INVALID_FILE_ATTRIBUTES || bool(attributes & FILE_ATTRIBUTE_DIRECTORY) != isDir)
                    continue;
            }
            SearchResult r;
            r.path = f.path;
            r.name = f.name;
            r.isDir = isDir;
            r.favorite = true;
            found.favorites.append(r);
        }
        const QString path = clipboardPath();
        const DWORD attributes = drives.contain(path) ? attributesOf(path) : INVALID_FILE_ATTRIBUTES;
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            found.clipboardIsDir = foldersOnly || (attributes & FILE_ATTRIBUTE_DIRECTORY);
            found.clipboard = (attributes & FILE_ATTRIBUTE_DIRECTORY) || !foldersOnly ? path : parentOf(path);
        }
        if (dialog)
            found.extensions = pathtext::filterExtensions(QString::fromStdWString(filedialog::fileType(dialog)));

        QMetaObject::invokeMethod(this, [this, lookup, dialog, found = std::move(found)] {
            if (lookup != m_lookups)
                return;
            const bool changed = found.explorer != m_explorer || found.explorerKinds != m_explorerKinds;
            m_explorer = found.explorer;
            m_explorerKinds = found.explorerKinds;
            m_recent = found.recent;
            m_favoritesThere = found.favorites;
            m_clipboard = found.clipboard;
            m_clipboardIsDir = found.clipboardIsDir;
            if (dialog == m_dialog)
                m_extensions = found.extensions;
            if (changed) {
                emit explorerChanged();
                updateAsking();
            }
            if (m_query.trimmed().isEmpty() && m_window && m_window->isActive())
                showSuggestions(true);
        }, Qt::QueuedConnection);
    });
}

void DialogBar::choose(int row, bool open)
{
    const SearchResult* item = m_results.at(row);
    if (item && !item->isApp())
        goTo(item->path, !item->isDir, open && canOpen());
}

void DialogBar::chooseExplorer()
{
    if (!m_explorer.isEmpty())
        goTo(m_explorer.constFirst(), false);
}

void DialogBar::setAskAutoJump(bool on)
{
    if (on == m_askAutoJump)
        return;
    m_askAutoJump = on;
    updateAsking();
}

void DialogBar::setElevatedNavAvailable(bool on)
{
    if (on == m_elevatedNavAvailable)
        return;
    m_elevatedNavAvailable = on;
    // An elevated dialog is copy-only only when the service cannot drive it.
    const bool copyOnly = m_elevated && !on;
    const bool copyOnlyChanged = copyOnly != m_copyOnly;
    m_copyOnly = copyOnly;
    emit dialogKindChanged(); // copyOnly / elevatedJump / canOpen for the current dialog
    if (copyOnlyChanged) {
        m_pathCopied = false;
        emit copyStateChanged();
        updateAsking();
        if (m_window && m_window->isVisible())
            place();
    }
}

void DialogBar::setCopyOnlyHint(const QString& hint)
{
    if (hint == m_copyOnlyHint)
        return;
    m_copyOnlyHint = hint;
    emit dialogKindChanged();
}

void DialogBar::updateAsking()
{
    const bool asking = !m_copyOnly && m_askAutoJump && m_dialog && !hidden() && !m_explorer.isEmpty();
    if (asking == m_asking)
        return;
    m_asking = asking;
    emit askingChanged();
    if (m_window && m_window->isVisible())
        place();
}

void DialogBar::answerAutoJump(const QString& when)
{
    if (!m_asking)
        return;
    setAskAutoJump(false);
    emit autoJumpAnswered(when);
    if (when == u"open")
        chooseExplorer(); // what it would have done
    else
        back(); // the click made the bar active: the dialog has the focus again
}

void DialogBar::goBack()
{
    if (originName().isEmpty())
        return;
    const QString origin = std::exchange(m_origin, QString());
    emit originChanged();
    goTo(origin, false);
}

void DialogBar::copyPathFallback(HWND dialog, const QString& path)
{
    // Also comes late (the service's helper may take ~12 s to give up): only
    // while that dialog is still the one in front. setDialog(nullptr) keeps
    // m_dialog for its coming back, so that alone is no proof; the clipboard
    // and the foreground of whatever the user went on to are left alone.
    if (!m_inFront || dialog != m_dialog || !dialog || !::IsWindow(dialog))
        return;
    QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(path));
    m_pathCopied = true;
    emit copyStateChanged();
    setQuery({});
    ::SetForegroundWindow(dialog);
}

void DialogBar::goTo(QString path, bool isFile, bool open)
{
    const HWND dialog = m_dialog;
    if (!dialog || !::IsWindow(dialog) || !m_mover)
        return;
    if (m_copyOnly) {
        copyPathFallback(dialog, path);
        return;
    }
    setQuery({}); // as it was before typing; the list closes as the dialog takes the focus
    ::SetForegroundWindow(dialog); // ours is in front, so it may hand that on
    m_mover(dialog, QDir::toNativeSeparators(path).toStdWString(), isFile, open);
}

// The list follows (Favorites::changed): the favorites first.
void DialogBar::setFavorite(const QString& path, bool isDir, bool on)
{
    if (path.isEmpty() || !m_favorites)
        return;
    if (!on) {
        m_favorites->remove(path);
        return;
    }
    Favorite f;
    f.kind = isDir ? Favorite::Kind::Folder : Favorite::Kind::File;
    f.path = QDir::toNativeSeparators(path);
    f.name = nameOf(f.path);
    m_favorites->add(f);
}

void DialogBar::enter(int row)
{
    const SearchResult* r = m_results.at(row);
    if (r && !r->isApp())
        setQuery(r->isDir ? withSlash(r->path) : r->path);
}

void DialogBar::up()
{
    const pathtext::TypedPath typed = pathtext::splitTyped(m_query);
    if (typed.folder.isEmpty())
        return;
    if (!typed.name.isEmpty()) { // "D:\Projects\Wo" -> "D:\Projects\"
        setQuery(withSlash(typed.folder));
        return;
    }
    const QString parent = parentOf(typed.folder); // "D:\Projects" -> "D:\"
    if (!parent.isEmpty() && parent != typed.folder && !pathtext::splitTyped(withSlash(parent)).folder.isEmpty())
        setQuery(withSlash(parent));
}

QVariantList DialogBar::menuItems(int row) const
{
    // Glyphs: Segoe Fluent Icons / MDL2 Assets.
    const auto entry = [](Action action, const QString& text, const QString& shortcut, const QString& glyph) {
        return QVariantMap {
            {u"action"_s, static_cast<int>(action)},
            {u"text"_s, text},
            {u"shortcut"_s, shortcut},
            {u"glyph"_s, glyph},
        };
    };
    const QVariantMap separator {{u"separator"_s, true}};
    const auto isPinned = [this](const QString& path) { return m_favorites && m_favorites->contains(path); };
    QVariantList items;

    if (row < 0) { // the bar's own
        const QString current = m_dialog ? QString::fromStdWString(filedialog::currentFolder(m_dialog)) : QString();
        if (!current.isEmpty()) {
            const bool pinned = isPinned(current);
            items.append(pinned ? entry(UnpinCurrent, tr("从收藏夹移除“%1”").arg(nameOf(current)), {}, u"\uE8D9"_s)
                                : entry(PinCurrent, tr("收藏“%1”").arg(nameOf(current)), {}, u"\uE734"_s));
            items.append(separator);
        }
        items.append(entry(HideHere, tr("这次不显示"), {}, u"\uE711"_s)); // Cancel
        if (!m_dialogApp.isEmpty())
            items.append(entry(HideInApp, tr("在 %1 中不再显示").arg(m_dialogApp), {}, u"\uED1A"_s)); // Hide
        items.append(separator);
        items.append(entry(OpenSettings, tr("设置"), {}, u"\uE713"_s));
        return items;
    }

    const SearchResult* r = m_results.at(row);
    if (!r || r->isApp())
        return {};
    items.append(entry(Go, tr("转到"), u"Enter"_s, r->isDir ? u"\uE8B7"_s : u"\uE8A5"_s)); // Folder, Document
    if (!r->isDir && canOpen())
        items.append(entry(GoAndOpen, tr("直接打开"), u"Ctrl+Enter"_s, u"\uE8E5"_s)); // OpenFile
    if (r->isDir && r->path.compare(m_browsed, Qt::CaseInsensitive) != 0)
        items.append(entry(Enter, tr("在列表中展开"), u"Tab"_s, QChar(0xE76C))); // ChevronRight
    items.append(entry(CopyPath, tr("复制路径"), u"Ctrl+Shift+C"_s, QChar(0xE8C8))); // Copy
    items.append(separator);
    if (isPinned(r->path))
        items.append(entry(Unpin, tr("从收藏夹移除"), {}, u"\uE8D9"_s)); // Unfavorite
    else
        items.append(entry(Pin, tr("加入收藏夹"), {}, u"\uE734"_s)); // FavoriteStar
    if (const QString folder = r->isDir ? QString() : parentOf(r->path); !folder.isEmpty() && !isPinned(folder))
        items.append(entry(PinFolder, tr("收藏所在的文件夹"), {}, u"\uE8B7"_s)); // Folder
    return items;
}

void DialogBar::trigger(int row, int action)
{
    switch (action) {
    case Go:
        choose(row);
        break;
    case GoAndOpen:
        choose(row, true);
        break;
    case Enter:
        enter(row);
        break;
    case CopyPath:
        if (const SearchResult* r = m_results.at(row); r && !r->isApp())
            QGuiApplication::clipboard()->setText(r->path);
        break;
    case Pin:
    case Unpin:
        if (const SearchResult* r = m_results.at(row); r && !r->isApp())
            setFavorite(r->path, r->isDir, action == Pin);
        break;
    case PinFolder:
        if (const SearchResult* r = m_results.at(row); r && !r->isApp() && !r->isDir)
            setFavorite(parentOf(r->path), true, true);
        break;
    case PinCurrent:
    case UnpinCurrent:
        if (m_dialog)
            setFavorite(QString::fromStdWString(filedialog::currentFolder(m_dialog)), true, action == PinCurrent);
        break;
    case HideHere:
        m_hiddenUnder = m_dialog;
        back();
        hideWindow();
        break;
    case HideInApp:
        if (!m_dialogApp.isEmpty()) {
            back();
            emit excludeAppRequested(m_dialogApp); // and the app's settings come back with it
        }
        break;
    case OpenSettings:
        emit settingsRequested();
        break;
    default:
        break;
    }
}

QRectF DialogBar::screenArea(QPointF globalPos) const
{
    QScreen* screen = QGuiApplication::screenAt(globalPos.toPoint());
    if (!screen && m_window)
        screen = m_window->screen();
    return screen ? QRectF(screen->availableGeometry()) : QRectF();
}

void DialogBar::prepareMenuWindow(QWindow* menu) const
{
    if (!menu)
        return;
    // Showing it must not take the focus from the bar, which keeps handling the keys.
    menu->setProperty("_q_showWithoutActivating", true);
    menu->setTransientParent(nullptr);
    win::followOwner(reinterpret_cast<HWND>(menu->winId()), nullptr);
    menu->setFlag(Qt::WindowStaysOnTopHint, false);
    menu->setTransientParent(m_window);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    win::styleFramelessWindow(menu);
    win::setDarkFrame(menu, dark);
    win::followOwner(reinterpret_cast<HWND>(menu->winId()), reinterpret_cast<HWND>(m_window->winId()));
}

void DialogBar::showWindow()
{
    m_visibility.show(m_dialog, m_elevated);
}

void DialogBar::hideWindow()
{
    m_visibility.hide();
}

void DialogBar::showLogo()
{
    m_logo.setOwner(reinterpret_cast<HWND>(m_window->winId()));
    m_logo.show(m_logoHost, m_logoRow, m_logoWork, &m_logoAvoid, m_logoScale);
}

// By the dialog's visible frame (physical pixels), on the side sideFor()
// picks. Under it: as wide as the dialog; above it when there is no room
// below, else over its top edge. Beside it: level with its top, or higher
// where the list would not fit below. The list, while the bar has the focus,
// opens below, or above where there is more room.
void DialogBar::setUiSize(const QString& size)
{
    const uisize::Metrics metrics = uisize::metrics(size);
    if (metrics.barHeight == s_metrics.barHeight && metrics.barRow == s_metrics.barRow)
        return;
    s_metrics = metrics;
    if (s_instance) {
        emit s_instance->metricsChanged();
        s_instance->m_roomMade.clear(); // made for the old height
        s_instance->place();
    }
}

void DialogBar::place()
{
    if (!m_window || !m_dialog)
        return;
    const std::optional<Placing> at = placingOf(m_dialog);
    if (!at)
        return;
    const RECT& frame = at->frame;
    const RECT& work = at->work;
    QScreen* screen = at->screen;
    const qreal scale = screen->devicePixelRatio();
    const QPoint origin = screen->geometry().topLeft(); // the same in logical and physical pixels
    const auto physical = [scale](int logical) { return qRound(logical * scale); };
    const auto logical = [scale](int physical) { return static_cast<int>(physical / scale); };

    m_side = sideFor(*at, m_place, m_side);
    const int gap = physical(kGap);
    const int bar = physical(s_metrics.barHeight);
    int width = 0;
    int x = 0;
    int barTop = 0;
    if (m_side == Place::Below) {
        width = std::min<int>(std::max<int>(frame.right - frame.left, physical(kMinWidth)), work.right - work.left);
        x = std::clamp<int>(frame.left, work.left, work.right - width);
        barTop = frame.bottom + gap;
        if (barTop + bar > work.bottom)
            barTop = frame.top - gap - bar >= work.top ? frame.top - gap - bar : std::max<int>(work.top, frame.top);
    } else {
        width = std::min<int>(physical(kSideWidth), work.right - work.left);
        x = std::clamp<int>(m_side == Place::Right ? frame.right + gap : frame.left - gap - width, work.left,
            work.right - width);
        barTop = std::max<int>(work.top, std::min<int>(frame.top, work.bottom - physical(panelHeight())));
    }

    const int wanted = m_window->isActive() && !m_listHeld ? std::min(m_results.count(), kMaxRows) : 0;
    const auto fit = [](int room) { return std::max(0, (room - kListChrome) / s_metrics.barRow); };
    const int fitBelow = fit(logical(work.bottom - barTop - bar));
    const int fitAbove = fit(logical(barTop - work.top));
    int rows = std::min(wanted, fitBelow);
    bool above = false;
    if (wanted > fitBelow && fitAbove > fitBelow) {
        rows = std::min(wanted, fitAbove);
        above = true;
    }
    // The list, or the line that asks (asking): below, else above.
    const int list = rows > 0 ? kListChrome + rows * s_metrics.barRow : m_asking ? s_metrics.askHeight : 0;
    if (rows == 0 && m_asking)
        above = barTop + bar + physical(s_metrics.askHeight) > work.bottom;
    const int top = above ? barTop - physical(list) : barTop;
    if (rows != m_rows || above != m_listAbove) {
        m_rows = rows;
        m_listAbove = above;
        emit layoutChanged();
    }
    // In logical pixels, as Qt takes them back: from the screen's origin,
    // scaled. Qt rounds the position and the size to physical pixels each,
    // which may put an edge a pixel past the work area's: moved in by one.
    QRect geometry(origin.x() + qRound((x - origin.x()) / scale), origin.y() + qRound((top - origin.y()) / scale),
        qRound(width / scale), s_metrics.barHeight + list);
    const auto start = [scale](int from, int at) { return from + qRound((at - from) * scale); };
    const auto end = [&](int from, int at, int size) { return start(from, at) + qRound(size * scale); };
    for (int i = 0; i < 2 && end(origin.x(), geometry.x(), geometry.width()) > work.right; ++i)
        geometry.translate(-1, 0);
    for (int i = 0; i < 2 && start(origin.x(), geometry.x()) < work.left; ++i)
        geometry.translate(1, 0);
    for (int i = 0; i < 2 && end(origin.y(), geometry.y(), geometry.height()) > work.bottom; ++i)
        geometry.translate(0, -1);
    for (int i = 0; i < 2 && start(origin.y(), geometry.y()) < work.top; ++i)
        geometry.translate(0, 1);
    // For the logo: the window as Qt will put it, and the box in it.
    const int left = start(origin.x(), geometry.x());
    const int boxTop = start(origin.y(), geometry.y() + (above ? list : 0));
    m_logoHost = {left, start(origin.y(), geometry.y()), end(origin.x(), geometry.x(), geometry.width()),
        end(origin.y(), geometry.y(), geometry.height())};
    m_logoRow = {left, boxTop, m_logoHost.right, boxTop + physical(s_metrics.barHeight)};
    m_logoWork = work;
    m_logoAvoid = frame;
    m_logoScale = scale;
    if (m_window->geometry() != geometry)
        m_window->setGeometry(geometry);
    if (m_visibility.isRevealed())
        showLogo(); // else once its first frame is up
}

} // namespace ws
