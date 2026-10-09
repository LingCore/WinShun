#include "DialogBar.h"

#include "PathText.h"
#include "Query.h"
#include "SearchEngine.h"
#include "platform/WindowEffects.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickWindow>
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

constexpr int kGap = 4; // logical pixels between the dialog and the bar
constexpr int kMinWidth = 480;
constexpr int kSideWidth = 480; // beside the dialog
constexpr int kMinDialogHeight = 400; // made shorter down to this (logical pixels), then moved up
constexpr int kMinDialogWidth = 640; // made narrower down to this, then moved aside
// The bar and a full list.
constexpr int kPanelHeight = DialogBar::kBarHeight + DialogBar::kListChrome + DialogBar::kMaxRows * DialogBar::kRowHeight;
constexpr int kMaxExplorer = 5; // suggestions of each kind
constexpr int kMaxRecent = 10;
constexpr int kMaxPins = 20;
constexpr int kMaxListed = 200; // of a folder whose path is typed

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

// The bar and a full list fit on that side of the dialog, as the dialog is.
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
        return at.frame.bottom + qRound((kGap + kPanelHeight) * scale) <= at.work.bottom;
    }
}

// Where the bar goes now: the side set if it fits there, else where it is
// if that still fits (no going back and forth as the dialog moves), else the
// first that fits of below, right and left; below if none does.
DialogBar::Place sideFor(const Placing& at, DialogBar::Place set, DialogBar::Place current)
{
    using enum DialogBar::Place;
    if (set != Auto && fitsBy(at, set))
        return set;
    if (fitsBy(at, current))
        return current;
    for (const DialogBar::Place side : {Below, Right, Left}) {
        if (fitsBy(at, side))
            return side;
    }
    return Below;
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

// The drives of this computer, as they are now. Paths on the others are not
// looked at: a network drive whose server is gone could take long to answer
// (and hold up all that comes after).
class LocalDrives {
public:
    LocalDrives()
    {
        const DWORD present = ::GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(present & (1u << i)))
                continue;
            const wchar_t root[] {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
            const UINT type = ::GetDriveTypeW(root);
            if (type != DRIVE_REMOTE && type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN)
                m_mask |= 1u << i;
        }
    }

    bool contain(const QString& path) const
    {
        if (path.size() < 3 || path[1] != u':' || path[2] != u'\\')
            return false;
        const char16_t letter = path[0].toUpper().unicode();
        return letter >= u'A' && letter <= u'Z' && (m_mask & (1u << (letter - u'A')));
    }

private:
    DWORD m_mask = 0;
};

DWORD attributesOf(const QString& path)
{
    return ::GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()));
}

// A folder, or a file's folder; empty when it is not there (or not on one of `drives`).
QString folderOf(const QString& path, const LocalDrives& drives)
{
    const DWORD attributes = drives.contain(path) ? attributesOf(path) : INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return {};
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? path : parentOf(path);
}

// The folders of what Windows lists as opened lately, newest first: the
// shortcuts in the Recent folder, which the file dialogs of all programs add
// to (what they open and save), and File Explorer too (the folders opened
// in it). Empty while Windows keeps no such list. Needs COM.
QStringList windowsRecentFolders(int max, const LocalDrives& drives)
{
    PWSTR dir = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DONT_VERIFY, nullptr, &dir)))
        return {};
    const QString recent = QString::fromWCharArray(dir);
    ::CoTaskMemFree(dir);
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))
        || FAILED(link.As(&file)))
        return {};
    const QFileInfoList shortcuts = QDir(recent).entryInfoList({u"*.lnk"_s}, QDir::Files, QDir::Time); // newest first
    std::wstring target(32768, L'\0');
    QStringList folders;
    for (const QFileInfo& shortcut : shortcuts.first(std::min<qsizetype>(shortcuts.size(), 100))) {
        if (folders.size() >= max)
            break;
        const std::wstring name = QDir::toNativeSeparators(shortcut.filePath()).toStdWString();
        if (FAILED(file->Load(name.c_str(), STGM_READ))
            || link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, 0) != S_OK)
            continue; // S_FALSE: no file behind it (Control Panel and the like)
        const QString folder = folderOf(QString::fromWCharArray(target.c_str()), drives);
        if (!folder.isEmpty() && !folders.contains(folder, Qt::CaseInsensitive))
            folders.append(folder);
    }
    return folders;
}

// One from each in turn, the first ones first: neither list knows when the
// other's entries were used.
QStringList interleaved(const QStringList& a, const QStringList& b, int max)
{
    QStringList out;
    for (qsizetype i = 0; out.size() < max && (i < a.size() || i < b.size()); ++i) {
        for (const QStringList* list : {&a, &b}) {
            if (i < list->size() && out.size() < max && !out.contains(list->at(i), Qt::CaseInsensitive))
                out.append(list->at(i));
        }
    }
    return out;
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

DialogBar::DialogBar(SearchEngine* engine, History* history, const QString& pinsFile, Mover mover, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_history(history)
    , m_pins(pinsFile, kMaxPins)
    , m_mover(std::move(mover))
{
    m_pins.load();
    m_pinned = m_pins.items(); // until the first look tells which are still there
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
    m_uncloakTimeout.setSingleShot(true);
    m_uncloakTimeout.setInterval(300ms); // should no frame come
    connect(&m_uncloakTimeout, &QTimer::timeout, this, [this] {
        m_uncloakPending = false;
        uncloak();
    });
}

DialogBar::~DialogBar() = default;

void DialogBar::setWindow(QQuickWindow* window)
{
    m_window = window;
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
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_uncloakPending.exchange(false))
            QMetaObject::invokeMethod(&m_uncloakTimeout, [this] {
                m_uncloakTimeout.stop();
                uncloak();
            });
    }, Qt::DirectConnection); // on the render thread
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
        if (kind != m_kind) {
            m_kind = kind;
            emit dialogKindChanged();
        }
        emit originChanged(); // that of another dialog, if any
        m_extensions.clear();
        m_requestId = 0;
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
    follow();
}

void DialogBar::dialogMoved()
{
    if (m_inFront)
        follow();
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
    if (hidden() || !::IsWindow(m_dialog) || !::IsWindowVisible(m_dialog) || ::IsIconic(m_dialog)) {
        hideWindow();
        return;
    }
    makeRoom();
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

// Once for each dialog: should the bar and a full list not fit by it on its
// screen (Place), it gets smaller there, its far edge staying: shorter for
// below, down to kMinDialogHeight; narrower for a side, down to
// kMinDialogWidth. Then it moves for the rest. Windows keeps a dialog's size
// for each program, so the next one comes up fitting. Maximised, it stays;
// and so does what the user does with it after.
void DialogBar::makeRoom()
{
    std::erase_if(m_roomMade, [](HWND dialog) { return !::IsWindow(dialog); });
    if (std::ranges::find(m_roomMade, m_dialog) != m_roomMade.end())
        return;
    m_roomMade.push_back(m_dialog);
    const std::optional<Placing> at = placingOf(m_dialog);
    RECT window {};
    if (!at || ::IsZoomed(m_dialog) || !::GetWindowRect(m_dialog, &window))
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
    const int below = physical(kGap + kPanelHeight + 1);
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

QString DialogBar::originName() const
{
    return m_originDialog == m_dialog && !m_origin.isEmpty() ? nameOf(m_origin) : QString();
}

void DialogBar::setQuery(const QString& query)
{
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    relist();
}

void DialogBar::relist()
{
    ++m_listings; // one on its way is for what was typed before
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
    request.scope = Scope::Files;
    if (m_recordHistory)
        request.history = m_history->items();
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
        if (!r.isApp() && (r.isDir || !foldersOnly()))
            rows.append(r);
    }
    // Files of the type the Open dialog shows first; where to save (a Save
    // dialog): folders first, files after (to save over); the pinned folders
    // before all.
    if (!m_extensions.isEmpty())
        std::ranges::stable_partition(rows, [this](const SearchResult& r) { return r.isDir || ofFileType(r); });
    if (m_kind == filedialog::Kind::Save)
        std::ranges::stable_partition(rows, &SearchResult::isDir);
    const QStringList& pins = m_pins.items();
    const auto pinned = [&](const SearchResult& r) { return r.isDir && pins.contains(r.path, Qt::CaseInsensitive); };
    std::ranges::stable_partition(rows, pinned);
    QStringList tags;
    for (const SearchResult& r : std::as_const(rows))
        tags.append(pinned(r) ? tr("固定") : QString());
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
    const auto add = [&](const QString& path, bool isDir, const QString& tag, bool recent = false) {
        if (seen.contains(path, Qt::CaseInsensitive))
            return;
        seen.append(path);
        SearchResult row;
        row.path = path;
        row.name = nameOf(path);
        row.isDir = isDir;
        row.recent = recent;
        rows.append(row);
        tags.append(tag);
    };
    for (const QString& folder : m_explorer.first(std::min<qsizetype>(m_explorer.size(), kMaxExplorer)))
        add(folder, true, tr("资源管理器"));
    if (!m_clipboard.isEmpty())
        add(m_clipboard, m_clipboardIsDir, tr("剪贴板"));
    for (const QString& folder : std::as_const(m_pinned))
        add(folder, true, tr("固定"));
    for (const QString& folder : std::as_const(m_recent))
        add(folder, true, tr("最近"), true);
    setRows(std::move(rows), std::move(tags), {}, refresh);
}

void DialogBar::lookAround()
{
    const int lookup = ++m_lookups;
    struct Found {
        QStringList explorer;
        QStringList recent;
        QStringList pinned;
        QString clipboard;
        bool clipboardIsDir = false;
        QStringList extensions;
    };
    const HWND dialog = m_dialog;
    const bool foldersOnly = this->foldersOnly();
    const bool recordHistory = m_recordHistory;
    QStringList history = m_recordHistory ? m_history->items() : QStringList();
    QStringList pins = m_pins.items();
    m_worker.post([this, lookup, dialog, foldersOnly, recordHistory, history = std::move(history),
                      pins = std::move(pins)] {
        if (lookup != m_lookups)
            return; // another is on its way
        Found found;
        for (const std::wstring& folder : filedialog::explorerFolders())
            found.explorer.append(QString::fromStdWString(folder));
        const LocalDrives drives;
        if (recordHistory) {
            // The folders of what was opened through Win顺 (or the folders themselves).
            QStringList ours;
            for (const QString& item : history) {
                if (ours.size() >= kMaxRecent)
                    break;
                const QString folder = folderOf(QDir::toNativeSeparators(item), drives);
                if (!folder.isEmpty() && !ours.contains(folder, Qt::CaseInsensitive))
                    ours.append(folder);
            }
            found.recent = interleaved(windowsRecentFolders(kMaxRecent, drives), ours, kMaxRecent);
        }
        for (const QString& pin : pins) { // one on a network drive or share is kept unasked
            if (!drives.contain(pin) || folderOf(pin, drives) == pin)
                found.pinned.append(pin);
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
            const bool changed = found.explorer != m_explorer;
            m_explorer = found.explorer;
            m_recent = found.recent;
            m_pinned = found.pinned;
            m_clipboard = found.clipboard;
            m_clipboardIsDir = found.clipboardIsDir;
            if (dialog == m_dialog)
                m_extensions = found.extensions;
            if (changed)
                emit explorerChanged();
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

void DialogBar::goBack()
{
    if (originName().isEmpty())
        return;
    const QString origin = std::exchange(m_origin, QString());
    emit originChanged();
    goTo(origin, false);
}

void DialogBar::goTo(QString path, bool isFile, bool open)
{
    const HWND dialog = m_dialog;
    if (!dialog || !::IsWindow(dialog) || !m_mover)
        return;
    setQuery({}); // as it was before typing; the list closes as the dialog takes the focus
    ::SetForegroundWindow(dialog); // ours is in front, so it may hand that on
    m_mover(dialog, QDir::toNativeSeparators(path).toStdWString(), isFile, open);
}

QString DialogBar::pinTarget(int row) const
{
    const SearchResult* r = m_results.at(row);
    if (!r || r->isApp())
        return {};
    return r->isDir ? r->path : parentOf(r->path);
}

void DialogBar::setPinned(const QString& folder, bool pinned)
{
    if (folder.isEmpty())
        return;
    m_pinned.removeIf([&](const QString& p) { return p.compare(folder, Qt::CaseInsensitive) == 0; });
    if (pinned) {
        m_pins.record(folder);
        m_pinned.prepend(folder);
    } else {
        m_pins.remove(folder);
    }
    relist(); // the pinned ones first
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
    const auto isPinned = [this](const QString& folder) { return m_pins.items().contains(folder, Qt::CaseInsensitive); };
    QVariantList items;

    if (row < 0) { // the bar's own
        const QString current = m_dialog ? QString::fromStdWString(filedialog::currentFolder(m_dialog)) : QString();
        if (!current.isEmpty()) {
            const bool pinned = isPinned(current);
            items.append(pinned ? entry(UnpinCurrent, tr("取消固定“%1”").arg(nameOf(current)), {}, u"\uE77A"_s)
                                : entry(PinCurrent, tr("固定“%1”").arg(nameOf(current)), {}, u"\uE718"_s));
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
    const QString folder = pinTarget(row);
    if (!folder.isEmpty()) {
        items.append(separator);
        if (isPinned(folder))
            items.append(entry(Unpin, tr("取消固定"), {}, u"\uE77A"_s));
        else
            items.append(entry(Pin, r->isDir ? tr("固定到列表") : tr("固定所在的文件夹"), {}, u"\uE718"_s));
    }
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
        setPinned(pinTarget(row), action == Pin);
        break;
    case PinCurrent:
    case UnpinCurrent:
        if (m_dialog)
            setPinned(QString::fromStdWString(filedialog::currentFolder(m_dialog)), action == PinCurrent);
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
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    win::styleFramelessWindow(menu);
    win::setDarkFrame(menu, dark);
}

void DialogBar::showWindow()
{
    if (!m_window || m_window->isVisible())
        return;
    // Cloaked until its first frame: Windows would first put up what the
    // window showed when it was hidden.
    win::setCloaked(m_window, true);
    m_uncloakPending = true;
    m_uncloakTimeout.start();
    m_window->setProperty("_q_showWithoutActivating", true); // the dialog keeps the focus
    m_window->show();
    m_window->setProperty("_q_showWithoutActivating", QVariant());
}

void DialogBar::hideWindow()
{
    if (!m_window || !m_window->isVisible())
        return;
    m_window->hide();
    m_logo.hide();
    m_uncloakPending = false;
    m_uncloakTimeout.stop();
    win::setCloaked(m_window, false);
}

void DialogBar::uncloak()
{
    if (!m_window)
        return;
    win::setCloaked(m_window, false);
    if (m_window->isVisible())
        showLogo();
}

void DialogBar::showLogo()
{
    m_logo.show(m_logoHost, m_logoRow, m_logoWork, &m_logoAvoid, m_logoScale);
}

// By the dialog's visible frame (physical pixels), on the side sideFor()
// picks. Under it: as wide as the dialog; above it when there is no room
// below, else over its top edge. Beside it: level with its top, or higher
// where the list would not fit below. The list, while the bar has the focus,
// opens below, or above where there is more room.
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
    const int bar = physical(kBarHeight);
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
        barTop = std::max<int>(work.top, std::min<int>(frame.top, work.bottom - physical(kPanelHeight)));
    }

    const int wanted = m_window->isActive() && !m_listHeld ? std::min(m_results.count(), kMaxRows) : 0;
    const auto fit = [](int room) { return std::max(0, (room - kListChrome) / kRowHeight); };
    const int fitBelow = fit(logical(work.bottom - barTop - bar));
    const int fitAbove = fit(logical(barTop - work.top));
    int rows = std::min(wanted, fitBelow);
    bool above = false;
    if (wanted > fitBelow && fitAbove > fitBelow) {
        rows = std::min(wanted, fitAbove);
        above = true;
    }
    const int list = rows > 0 ? kListChrome + rows * kRowHeight : 0;
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
        qRound(width / scale), kBarHeight + list);
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
    m_logoRow = {left, boxTop, m_logoHost.right, boxTop + physical(kBarHeight)};
    m_logoWork = work;
    m_logoAvoid = frame;
    m_logoScale = scale;
    if (m_window->geometry() != geometry)
        m_window->setGeometry(geometry);
    if (m_window->isVisible() && !m_uncloakPending)
        showLogo(); // else once its first frame is up
}

} // namespace ws
