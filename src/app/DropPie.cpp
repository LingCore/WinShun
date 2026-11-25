#include "DropPie.h"

#include "DropPieText.h"
#include "Favorites.h"
#include "FolderMenu.h"
#include "PieLabels.h"
#include "SystemCatalog.h"
#include "platform/Integrity.h"

#include <QDebug>
#include <QDir>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QScreen>
#include <QThreadPool>

#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws {

namespace {

// A chip's insides at 界面大小 standard (times the scale): the folder's
// glyph, its name and, for a name shown twice, its parent's.
constexpr double kChipPadStart = 12;
constexpr double kChipPadEnd = 14;
constexpr double kGlyph = 16;
constexpr double kGlyphGap = 6;
constexpr double kHintGap = 6;
constexpr double kHintWidth = 56; // at most
constexpr double kMinChip = 64;
// The line under the chips.
constexpr double kStatusGap = 12;
constexpr double kStatusPad = 10;
constexpr double kStatusMin = 280;
constexpr double kStatusMax = 400;
// After a page is turned, how far the pointer goes (logical pixels) before a chip of the new page is chosen.
constexpr double kTurnSlop = 4;
// The columns (GitHub #463), timed as the folder menu's (FolderMenuWindow):
// resting this long on a folder opens its column, or closes those after it;
// a folder slower to read than kSlow has its column say so meanwhile; on the
// way to a column the pointer passes what lies there, chosen only if it
// stops kAim there (menu aim).
constexpr auto kDwell = 350ms;
constexpr auto kSlow = 200ms;
constexpr auto kAim = 150ms;
constexpr auto kScrollStep = 60ms; // a row at a time while the pointer rests on a strip
constexpr qsizetype kMaxRows = 500; // folders listed in a column, the first in order (FolderMenu::subfolders)
// A row as the folder menu's (FolderMenuColumn::rowWidth): its icon, the
// name, room after it, the arrow of a folder that has a column of its own.
constexpr double kRowName = 42;
constexpr double kRowEnd = 26;
constexpr double kRowArrow = 20;
constexpr double kColumnGap = 4; // between two columns; the first keeps the chips' gap from the pie
constexpr double kAimSlop = 6; // the aim's triangle reaches this far past a column's corners

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

// The screen in whole pixels (pie::wholePixel) that are all on it: Qt
// rounds its size (2560 device pixels at 150 % are 1707 logical, the last
// one half off the screen).
QRect wholeOf(const QScreen* screen)
{
    const QRect logical = screen->geometry();
    const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    MONITORINFO info {.cbSize = sizeof info};
    if (!native || !::GetMonitorInfoW(native->handle(), &info))
        return logical;
    const qreal scale = screen->devicePixelRatio();
    return {logical.topLeft(), QSize(pie::wholePixel((info.rcMonitor.right - info.rcMonitor.left) / scale, scale, -1),
                                   pie::wholePixel((info.rcMonitor.bottom - info.rcMonitor.top) / scale, scale, -1))};
}

// A physical point in Qt's pixels on `screen` (the same origin in both).
QPointF logicalOf(POINT pt, const QScreen* screen)
{
    const QPoint origin = screen->geometry().topLeft();
    const qreal scale = screen->devicePixelRatio();
    return {origin.x() + (pt.x - origin.x()) / scale, origin.y() + (pt.y - origin.y()) / scale};
}

// "D:\Work\" -> "D:\Work"; a drive keeps its backslash.
QString normalized(QString path)
{
    path = QDir::toNativeSeparators(path);
    while (path.size() > 3 && path.endsWith(u'\\'))
        path.chop(1);
    return path;
}

bool sameOrInside(const QString& folder, const QString& item)
{
    if (folder.compare(item, Qt::CaseInsensitive) == 0)
        return true;
    const QString prefix = item.endsWith(u'\\') ? item : item + u'\\';
    return folder.startsWith(prefix, Qt::CaseInsensitive);
}

QString parentOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'\\');
    if (slash < 0)
        return {};
    return path.left(slash == 2 && path.size() > 1 && path.at(1) == u':' ? 3 : slash);
}

// The drive a path is on, case-folded: "d:", or "\\server\share"; empty for
// neither. File Explorer moves within one and copies to another.
QString driveOf(const QString& path)
{
    if (path.size() >= 2 && path.at(1) == u':')
        return path.left(2).toCaseFolded();
    if (!path.startsWith(u"\\\\"_s))
        return {};
    const qsizetype server = path.indexOf(u'\\', 2);
    const qsizetype share = server < 0 ? -1 : path.indexOf(u'\\', server + 1);
    return (share < 0 ? path : path.left(share)).toCaseFolded();
}

bool isNetworkPath(const std::wstring& path)
{
    if (path.starts_with(L"\\\\"))
        return true;
    if (path.size() < 2 || path[1] != L':')
        return false;
    const std::wstring root = path.substr(0, 2) + L"\\";
    return ::GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

// On the worker: a place's command that names a folder of the file system
// ("shell:Downloads", "%USERPROFILE%\Desktop") as its path; empty for the
// rest (Control Panel, a page of Settings, a program).
QString placeFolder(const QString& command)
{
    const auto [program, arguments] = splitCommand(command);
    if (program.isEmpty() || !arguments.isEmpty())
        return {};
    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(::SHParseDisplayName(reinterpret_cast<const wchar_t*>(program.utf16()), nullptr, &item, 0, nullptr)))
        return {};
    wchar_t path[MAX_PATH * 4] {};
    const bool found = ::SHGetPathFromIDListEx(item, path, static_cast<DWORD>(std::size(path)), GPFIDL_DEFAULT);
    ::CoTaskMemFree(item);
    if (!found)
        return {};
    const DWORD attributes = ::GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? QString::fromWCharArray(path)
                                                                                         : QString();
}

// What Windows says of an error, in the user's language.
QString errorText(HRESULT hr)
{
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    QString text = length ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed() : QString();
    ::LocalFree(buffer);
    return text.isEmpty() ? u"0x%1"_s.arg(static_cast<quint32>(hr), 8, 16, u'0') : text;
}

QFont fontOf(int pixelSize, bool bold = false)
{
    QFont font = QGuiApplication::font(); // what a QML Text uses unless told otherwise
    font.setPixelSize(std::max(1, pixelSize));
    if (bold)
        font.setWeight(QFont::Bold);
    return font;
}

// "D:\" + "Work" -> "D:\Work"; "D:\Work" + "2026" -> "D:\Work\2026".
QString childOf(const QString& folder, const QString& name)
{
    return folder.endsWith(u'\\') ? folder + name : folder + u'\\' + name;
}

// A number the window states (DropPieWindow: from Theme), else `fallback`.
double windowNumber(const QQuickWindow* window, const char* name, double fallback)
{
    const double value = window ? window->property(name).toDouble() : 0;
    return value > 0 ? value : fallback;
}

} // namespace

DropPie::DropPie(Favorites* favorites, SystemCatalog* places, piedrop::FileOperations* operations, Notify notify,
    QObject* parent)
    : QObject(parent)
    , m_favorites(favorites)
    , m_places(places)
    , m_operations(operations)
    , m_notify(std::move(notify))
{
    m_revealWait.setSingleShot(true);
    connect(&m_revealWait, &QTimer::timeout, this, [this] {
        if (!m_open || m_entered)
            return;
        // The drag has not come over the pie. While the pointer is still,
        // OLE may ask no window until it moves: asked to look again. Moved
        // and still nothing, the drag is not over it.
        POINT now {};
        ::GetCursorPos(&now);
        const bool moved = std::abs(now.x - m_shownAt.x) > 4 || std::abs(now.y - m_shownAt.y) > 4;
        if (moved || m_shownClock.elapsed() > 1000) {
            // Nothing seen of it (unseen until DragEnter): a failure to the
            // user, in the log at any level (GitHub #150).
            qWarning().noquote() << u"Drop pie: shown, but the drag did not come over it in %1 ms (pointer moved %2)"_s
                                        .arg(m_shownClock.elapsed()).arg(moved ? u"yes"_s : u"no"_s);
            hide("the drag did not come over it");
        } else {
            nudge();
            m_revealWait.start(100ms);
        }
    });
    m_releaseWait.setSingleShot(true);
    connect(&m_releaseWait, &QTimer::timeout, this, [this] {
        if (m_open && !m_confirming)
            hide("the button was let go, but no drop or leave came");
    });
    m_confirmTimer.setSingleShot(true);
    connect(&m_confirmTimer, &QTimer::timeout, this, [this] { hide("dropped"); });
    m_dwell.setSingleShot(true);
    m_dwell.setInterval(kDwell);
    connect(&m_dwell, &QTimer::timeout, this, &DropPie::dwelt);
    m_slow.setSingleShot(true);
    m_slow.setInterval(kSlow);
    connect(&m_slow, &QTimer::timeout, this, [this] {
        if (m_pending)
            showColumn(m_pending->level, m_pending->index, nullptr); // "reading", its rows to come
    });
    m_scroll.setInterval(kScrollStep);
    connect(&m_scroll, &QTimer::timeout, this, &DropPie::scrollColumn);
    // The pointer stopped where it passed on its way to a column: that is
    // chosen after all (OLE need not ask again while it is still).
    m_aim.setSingleShot(true);
    m_aim.setInterval(kAim);
    connect(&m_aim, &QTimer::timeout, this, [this] {
        if (m_open && m_entered && !m_confirming)
            track(m_lastKeys, m_lastAt);
    });
    if (m_favorites)
        connect(m_favorites, &Favorites::changed, this, &DropPie::gatherFolders);
    if (m_places)
        connect(m_places, &SystemCatalog::changed, this, &DropPie::gatherFolders);
    updateSectors();
    gatherFolders();
}

DropPie::~DropPie()
{
    if (m_window && m_hwnd && ::IsWindow(m_hwnd))
        ::RevokeDragDrop(m_hwnd);
    if (m_target)
        m_target->detach();
    m_target.Reset();
}

void DropPie::setWindow(QQuickWindow* window)
{
    m_window = window;
    if (!m_window)
        return;
    // See-through round the chips: a window colour that is all transparent
    // (Theme: never half), and an alpha channel from the start, before the
    // swap chain is made.
    QSurfaceFormat format = m_window->requestedFormat();
    format.setAlphaBufferSize(8);
    m_window->setFormat(format);
    m_window->setColor(Qt::transparent);
    m_window->setProperty("_q_showWithoutActivating", true);
    m_window->create();
    takeDropSite();
    QTimer::singleShot(2s, this, &DropPie::prewarm); // after the launcher's own (App::prewarmLauncher)
}

// Qt registered its own drop target when it made the window: ours in its
// place. Qt never registers again unless the window is made anew (then its
// handle is another), so this is looked at before every showing.
bool DropPie::takeDropSite()
{
    if (!m_window)
        return false;
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (hwnd == m_hwnd && m_target)
        return true;
    if (m_target)
        m_target->detach();
    ::RevokeDragDrop(hwnd);
    m_target = Microsoft::WRL::Make<piedrop::DropTarget>(static_cast<piedrop::Handler*>(this));
    const HRESULT hr = m_target ? ::RegisterDragDrop(hwnd, m_target.Get()) : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        qWarning().noquote() << u"Drop pie: could not take the window's drops (0x%1)"_s.arg(static_cast<quint32>(hr), 8, 16, u'0');
        m_target.Reset();
        m_hwnd = nullptr; // retry at the next summon instead of remembering a failed registration
        return false;
    }
    m_hwnd = hwnd;
    return true;
}

// Shown once, a pixel big, until its first frame: the graphics are set up
// now, and the first time it is asked for it shows at once.
void DropPie::prewarm()
{
    const QScreen* screen = QGuiApplication::primaryScreen();
    if (!m_window || !screen || m_open)
        return; // up already (a preview that took its time): its window is not to be made one pixel big
    const QRect area = screen->availableGeometry();
    m_window->setGeometry(QRect(area.bottomRight(), QSize(1, 1)));
    connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_window && !m_open)
            m_window->hide();
    }, static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection));
    m_window->show();
    QTimer::singleShot(1s, this, [this] {
        if (m_window && !m_open)
            m_window->hide(); // no frame came (a renderer that never draws hidden windows)
    });
}

void DropPie::summoned(POINT pt, HWND captureWindow, DWORD sourcePid)
{
    QMetaObject::invokeMethod(this, [this, pt, captureWindow, sourcePid] {
        onSummoned(pt, captureWindow, sourcePid);
    }, Qt::QueuedConnection);
}

QString DropPie::keyTakenTitle()
{
    return tr("收藏圆盘打不开");
}

QString DropPie::keyTakenText()
{
    return tr("Esc 下面的 ` 键被另一个程序设成了快捷键，拖动文件时按它打不开圆盘。请在那个程序里换掉这个快捷键。");
}

void DropPie::buttonReleased()
{
    QMetaObject::invokeMethod(this, [this] {
        if (m_open && !m_confirming && !m_releaseWait.isActive())
            m_releaseWait.start(300ms); // the Drop or DragLeave is on its way
    }, Qt::QueuedConnection);
}

void DropPie::onSummoned(POINT pt, HWND captureWindow, DWORD sourcePid)
{
    if (!m_window || m_confirming)
        return;
    if (m_open) {
        // Its key again: away, the drag going on.
        hide("its key pressed again");
        nudge(); // OLE finds what is under the pointer without waiting for a move
        return;
    }
    if (sourcePid == ::GetCurrentProcessId()) {
        // Results dragged out of the launcher: DoDragDrop runs on this very
        // thread, and the pie would be its own drop target from inside it.
        qInfo() << "Drop pie: asked for while dragging out of Win顺 itself; not shown";
        return;
    }
    // A drag from a program run as administrator is shown too: UIPI lets
    // its drops reach us (pitfalls, "管理员进程往普通程序拖文件，UIPI 不挡").
    qInfo().noquote() << u"Drop pie: its key pressed at %1,%2%3"_s.arg(pt.x)
                             .arg(pt.y)
                             .arg(integrity::higherThanOurs(captureWindow) ? u", from an administrator's program"_s
                                                                           : QString());
    open(pt);
}

void DropPie::open(POINT pt)
{
    QScreen* screen = screenAt(pt);
    if (!screen || !m_window)
        return;
    m_page = 0; // always at the first page
    m_awaitMove = m_keepUntilMove = false;
    m_leaving.clear();
    resetColumns();
    updateSectors(); // the size may have changed (界面大小)
    const QRect rect = place(screen, pt);

    m_open = true;
    m_entered = false;
    m_confirming = false;
    m_allowed = DROPEFFECT_MOVE | DROPEFFECT_COPY;
    setItems({});
    m_count = 0;
    m_plain = false;
    m_itemsDrive.clear();
    m_dpr = screen->devicePixelRatio();
    m_warped.invalidate();
    m_lastAt = pt;
    refreshBlocks();
    m_spot = {};
    m_trace.clear();
    m_keys = {};
    m_hovered = m_picked = -1;
    updateSectors();
    updateTexts();
    emit layoutChanged();
    emit stateChanged();

    m_window->setGeometry(rect);
    if (!takeDropSite()) {
        hide("drop registration failed");
        if (m_notify) m_notify(tr("收藏圆盘未打开"), tr("暂时无法接收文件拖放，请松开鼠标后重试。"));
        return;
    }
    m_window->show();
    m_window->raise();
    m_shownAt = pt;
    m_shownClock.start();
    // The key pressed with the pointer still: OLE would
    // not look for a target until it moves.
    QTimer::singleShot(0, this, [this] {
        if (m_open && !m_entered)
            nudge();
    });
    m_revealWait.start(150ms);
    checkFolders();
}

// For the pointer at `pt`: the chips and the line under them (above them by
// a screen's bottom) inside the work area of its monitor. Moved more than
// the middle's size from the pointer, the pointer is to go to the middle
// (m_warp). Returns the window's rectangle; the middle and the line are set
// in it.
void DropPie::preview(POINT pt, int hovered)
{
    QScreen* screen = screenAt(pt);
    if (!screen || !m_window)
        return;
    m_page = 0;
    m_awaitMove = m_keepUntilMove = false;
    m_leaving.clear();
    resetColumns();
    updateSectors();
    const QRect rect = place(screen, pt);
    m_open = true;
    m_entered = true;
    m_confirming = false;
    m_allowed = DROPEFFECT_MOVE | DROPEFFECT_COPY;
    setItems({});
    m_count = 1;
    m_plain = false;
    m_itemsDrive.clear();
    m_dpr = screen->devicePixelRatio();
    m_lastAt = pt;
    m_trace.clear();
    refreshBlocks();
    m_keys = {};
    m_hovered = m_picked = hovered;
    m_spot = hovered >= 0 ? Spot {0, hovered, Part::Chip} : Spot {0, -1, Part::Middle};
    updateSectors();
    updateTexts();
    emit layoutChanged();
    emit stateChanged();
    m_window->setGeometry(rect);
    setRevealed(true);
}

void DropPie::previewPage(int page, int hovered)
{
    m_page = pie::clampPage(page, static_cast<int>(m_folders.size()));
    m_awaitMove = m_keepUntilMove = false;
    m_leaving.clear();
    closeColumnsFrom(0);
    m_hovered = m_picked = hovered;
    m_spot = hovered >= 0 ? Spot {0, hovered, Part::Chip} : Spot {0, -1, Part::Middle};
    updateSectors();
    updateTexts();
    emit stateChanged();
}

bool DropPie::previewOpen(int index)
{
    const int level = static_cast<int>(m_columns.size());
    const Spot from {level, index, level == 0 ? Part::Chip : Part::Row};
    const Folder* folder = folderAt(from);
    if (!folder || !opens(from))
        return false;
    const QStringList names = FolderMenu::subfolders(folder->path, kMaxRows);
    m_listings.insert(folder->path.toCaseFolded(), names);
    m_spot = from; // the pointer on what it came from
    m_hovered = level == 0 ? index : -1;
    m_picked = level == 0 && m_blocks[folderOf(index)] == Block::None ? index : -1;
    showColumn(level, index, &names);
    updateTexts();
    emit stateChanged();
    return true;
}

void DropPie::previewColumnPointer(int column, int row, int first, int strip)
{
    if (column < 0 || column >= static_cast<int>(m_columns.size()))
        return;
    Column& c = m_columns[static_cast<std::size_t>(column)];
    c.first = pie::clampFirst(first, static_cast<int>(c.rows.size()), c.box.shown);
    m_spot = {column + 1, row, strip < 0 ? Part::Up : strip > 0 ? Part::Down : row >= 0 ? Part::Row : Part::Box};
    if (m_spot.part != Part::Row)
        m_spot.index = -1;
    m_hovered = m_picked = -1;
    updateTexts();
    emit stateChanged();
}

DWORD DropPie::previewPointer(QPointF local, DWORD keys)
{
    if (!m_open || !takeDropSite())
        return DROPEFFECT_NONE;
    RECT rect {};
    ::GetWindowRect(m_hwnd, &rect);
    return track(keys, POINT {rect.left + static_cast<LONG>(std::lround(local.x() * m_dpr)),
                           rect.top + static_cast<LONG>(std::lround(local.y() * m_dpr))});
}

QRectF DropPie::drawnRect() const
{
    QRectF drawn = m_allBounds.translated(m_centre).united(m_statusRect);
    for (const Column& c : m_columns)
        drawn = drawn.united(c.box.rect);
    constexpr double kShadow = 16; // the soft shadows round the chips and the boxes
    return drawn.adjusted(-kShadow, -kShadow, kShadow, kShadow);
}

// The window is for every page at once (m_allBounds), so a page of wider
// names than the first is not cut off by it, and turning moves nothing.
QRect DropPie::place(QScreen* screen, POINT pt)
{
    const QPointF cursor = logicalOf(pt, screen);
    const QRectF chips = m_allBounds.adjusted(-8 * m_scale, -8 * m_scale, 8 * m_scale, 8 * m_scale);
    const QSizeF status = m_statusRect.size();
    const QRectF below(-status.width() / 2, m_allBounds.bottom() + kStatusGap * m_scale, status.width(), status.height());
    const QRectF above = below.translated(0, m_allBounds.top() - kStatusGap * m_scale - status.height() - below.top());
    const QRectF area(screen->availableGeometry());
    const double safe = m_metrics.dead - 4 * m_scale;
    const pie::Placement low = pie::placeCentre(cursor, area, chips.united(below), safe);
    const pie::Placement high = pie::placeCentre(cursor, area, chips.united(above), safe);
    const auto shift = [&](const pie::Placement& p) { return std::hypot(p.centre.x() - cursor.x(), p.centre.y() - cursor.y()); };
    const bool flipped = shift(high) + 0.5 < shift(low);
    const pie::Placement placement = flipped ? high : low;
    const QRectF statusBox = flipped ? above : below;
    // The pointer inside the window, wherever the pie went: else the drag
    // never enters it. The whole work area too, for the columns to come in
    // without the window changing while it is seen (DropPie's class note).
    const QRectF pointer(cursor - placement.centre, QSizeF(1, 1));
    const QRectF work = area.translated(-placement.centre);
    const QRect rect = pie::windowRect(placement.centre, m_layout.reach.united(statusBox).united(pointer).united(work),
        wholeOf(screen), screen->devicePixelRatio());
    m_centre = placement.centre - rect.topLeft();
    m_statusRect = statusBox.translated(m_centre);
    m_area = area.translated(-QPointF(rect.topLeft())).intersected(QRectF(QPointF(), QSizeF(rect.size())));
    m_warp = placement.warp;
    return rect;
}

void DropPie::hide(const char* why)
{
    const bool shown = m_open || (m_window && m_window->isVisible());
    if (m_target)
        m_target->leaveEarly();
    if (m_window)
        m_window->hide();
    m_revealWait.stop();
    m_releaseWait.stop();
    m_confirmTimer.stop();
    m_open = false;
    m_entered = false;
    m_confirming = false;
    setItems({});
    m_count = 0;
    m_plain = false;
    m_itemsDrive.clear();
    m_warp = false;
    m_hovered = m_picked = -1;
    m_spot = {};
    m_trace.clear();
    m_aim.stop();
    m_awaitMove = m_keepUntilMove = false;
    resetColumns();
    if (m_page != 0 || !m_leaving.isEmpty()) {
        m_page = m_pageNotified = 0; // it opens on the first again; its chips are made then
        m_leaving.clear();
        emit pageChanged();
    }
    setRevealed(false);
    syncPageKeys(); // Tab is the system's again
    emit stateChanged();
    if (shown)
        qInfo() << "Drop pie: hidden," << why;
}

// The pointer put where it is: OLE's drag loop takes it for a move and asks
// the window under it again (a window shown or hidden under a still pointer
// it does not notice).
void DropPie::nudge() const
{
    POINT at {};
    if (::GetCursorPos(&at))
        ::SetCursorPos(at.x, at.y);
}

void DropPie::setRevealed(bool revealed)
{
    if (m_revealed == revealed)
        return;
    m_revealed = revealed;
    emit revealedChanged();
    syncPageKeys();
}

// Tab and Shift+Tab are the pie's only while it is there to see, with more
// than one page, and nothing is being dropped: never for a drag of text (it
// is never revealed) or a pie of one page, and not a moment after.
void DropPie::syncPageKeys()
{
    const bool wanted = m_open && m_revealed && !m_confirming && m_pages > 1;
    if (wanted == m_pageKeys)
        return;
    m_pageKeys = wanted;
    emit pageKeysWanted(wanted);
}

void DropPie::pageKey(bool back)
{
    QMetaObject::invokeMethod(this, [this, back] { turnPage(back); }, Qt::QueuedConnection);
}

// Tab (back: Shift+Tab): the next page of folders, round to the first after
// the last. What was chosen on the page before is dropped with it (a chip
// under the pointer then is not the one it was on), and nothing on the new one
// is chosen until the pointer has moved a little (track).
void DropPie::turnPage(bool back)
{
    const int total = static_cast<int>(m_folders.size());
    if (!m_open || !m_revealed || m_confirming || pie::pageCount(total) < 2)
        return;
    const int page = pie::turnedPage(m_page, total, back);
    if (page == m_page)
        return;
    POINT now {};
    m_turnedAt = ::GetCursorPos(&now) ? now : m_lastAt;
    m_leaving = m_sectorItems;
    for (QVariant& chip : m_leaving) { // drawn fading out, never chosen (DropPieWindow)
        QVariantMap map = chip.toMap();
        map.insert(u"leaving"_s, true);
        chip = map;
    }
    m_page = page;
    m_awaitMove = true;
    m_keepUntilMove = false;
    m_hovered = m_picked = -1;
    // Nothing chosen, as for a pointer that has not moved (track); the
    // columns came from a chip of the page left.
    m_spot = m_spot.level == 0 ? Spot {0, -1, m_spot.part} : Spot {0, -1, Part::Chip};
    closeColumnsFrom(0);
    updateSectors(); // tells of the page
    updateTexts();
    emit pageTurned();
    emit stateChanged();
    qInfo().noquote() << u"Drop pie: page %1 of %2"_s.arg(m_page + 1).arg(m_pages);
}

// ---- the drag over it ------------------------------------------------------

DWORD DropPie::dragEnter(IDataObject* data, DWORD keys, POINT pt, DWORD allowed)
{
    if (!m_open)
        return DROPEFFECT_NONE;
    m_entered = true;
    m_revealWait.stop();
    if (!piedrop::hasFiles(data) && !piedrop::hasShellItems(data)) {
        hide("not files (text, a picture…)");
        return DROPEFFECT_NONE;
    }
    if (!(allowed & (DROPEFFECT_MOVE | DROPEFFECT_COPY))) {
        hide("the source allows neither moving nor copying");
        return DROPEFFECT_NONE;
    }
    m_allowed = allowed;
    // Read once, here: every call to the source waits on another process.
    piedrop::DragItems items = piedrop::readItems(data);
    if (items.count == 0) {
        hide("its items could not be read");
        return DROPEFFECT_NONE;
    }
    setItems(std::move(items.paths));
    m_count = items.count;
    m_plain = items.plain;
    QSet<QString> drives;
    for (const std::wstring& item : m_items)
        drives.insert(driveOf(QString::fromStdWString(item)));
    // Some without a path (a phone's, a zip file's): not on any drive here.
    const bool oneDrive = drives.size() == 1 && m_items.size() == m_count && !drives.begin()->isEmpty();
    m_itemsDrive = oneDrive ? *drives.begin() : QString();
    refreshBlocks();
    qInfo().noquote() << u"Drop pie: %1 %2 over it (read from %3) %4 ms after it was asked for"_s.arg(m_count)
                             .arg(m_plain ? u"files"_s : u"items that are not lasting files"_s)
                             .arg(items.fromIdList ? u"the ID list"_s : u"CF_HDROP"_s)
                             .arg(m_shownClock.elapsed());
    return reveal(keys, pt);
}

// The window round the pointer as it is now: placed where the pie was asked
// for, the hand may have moved since (DragEnter comes a few dozen ms later).
void DropPie::follow(POINT pt)
{
    QScreen* screen = screenAt(pt);
    if (!screen || !m_window)
        return;
    m_dpr = screen->devicePixelRatio();
    const QRect rect = place(screen, pt);
    if (rect != m_window->geometry())
        m_window->setGeometry(rect);
    emit layoutChanged();
}

DWORD DropPie::reveal(DWORD keys, POINT pt)
{
    follow(pt);
    // Placed off the pointer by a screen's edge: now that it is files, the
    // pointer goes to the middle (never for a drag of text, which only
    // passed the listener's test). OLE follows the move SetCursorPos makes.
    if (std::exchange(m_warp, false)) {
        const POINT to = centreOnScreen();
        if (::SetCursorPos(to.x, to.y)) {
            qInfo().noquote() << u"Drop pie: by the screen's edge; the pointer moved from %1,%2 to the middle at %3,%4"_s
                                     .arg(pt.x).arg(pt.y).arg(to.x).arg(to.y);
            pt = to;
            m_warped.start();
        } else {
            qWarning().noquote() << u"Drop pie: could not move the pointer to the middle (%1)"_s.arg(::GetLastError());
        }
    }
    updateSectors();
    setRevealed(true);
    return track(keys, pt);
}

DWORD DropPie::dragOver(DWORD keys, POINT pt, DWORD /*allowed: as at DragEnter*/)
{
    if (!m_open || !m_entered)
        return DROPEFFECT_NONE;
    return track(keys, pt);
}

void DropPie::dragLeave()
{
    if (m_open && !m_confirming)
        hide("the drag left it (Esc, or it went elsewhere)");
}

DWORD DropPie::drop(IDataObject* data, DWORD keys, POINT pt, DWORD /*allowed*/)
{
    if (!m_open || !m_entered)
        return DROPEFFECT_NONE;
    // The folder's own drop target may show a dialog, whose loop could run
    // App::applyDropPie: it waits until this returns.
    m_inDrop = true;
    const auto done = qScopeGuard([this] { m_inDrop = false; });
    // What the line under the chips says: on the way to a column, the folder
    // it came from (track keeps it while the aim holds).
    track(keys, pt);
    m_aim.stop();
    if (!m_open)
        return DROPEFFECT_NONE;
    const Folder* target = targetFolder();
    if (!target) {
        hide(m_spot.part == Part::Middle ? "let go in the middle: nothing done" : "let go off the folders");
        return DROPEFFECT_NONE;
    }
    const Folder folder = *target;
    const int level = m_spot.level;
    // Deleted since the pie opened: refused, and why shown a moment.
    if (!folder.network) {
        const DWORD attributes = ::GetFileAttributesW(reinterpret_cast<const wchar_t*>(folder.path.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            m_missing.insert(folder.path.toCaseFolded());
            refreshBlocks();
            m_picked = -1;
            updateSectors();
            updateTexts();
            m_confirming = true;
            m_dwell.stop();
            m_scroll.stop();
            syncPageKeys();
            m_releaseWait.stop();
            emit stateChanged();
            m_confirmTimer.start(1200ms);
            qInfo() << "Drop pie: let go on a folder that is gone; nothing done";
            return DROPEFFECT_NONE;
        }
    }
    m_dwell.stop(); // nothing more opens or closes while it is shown taken
    m_scroll.stop();
    if (level > 0)
        qInfo().noquote() << u"Drop pie: let go on a folder of column %1"_s.arg(level);
    const bool move = modeOf(&folder) == pie::Mode::Move;
    const QString failed = move ? tr("没能移动到“%1”").arg(folder.name) : tr("没能复制到“%1”").arg(folder.name);
    DWORD effect = DROPEFFECT_NONE;
    if (m_plain && m_operations) {
        // Lasting files: copied or moved here by Windows' own file
        // operation, on a thread of its own. The source is told nothing was
        // dropped, so it deletes nothing itself (a "move" it would finish by
        // deleting); one that drops asynchronously (File Explorer) is told
        // the drop goes on, and keeps its data until it is over.
        const auto count = m_items.size();
        Microsoft::WRL::ComPtr<IAgileReference> async = piedrop::startAsync(data);
        qInfo().noquote() << u"Drop pie: dropped, %1 %2 files by our own file operation%3"_s
                                 .arg(move ? u"moving"_s : u"copying"_s)
                                 .arg(count)
                                 .arg(async ? u", the source's asynchronous drop started"_s : QString());
        m_operations->start(m_items, folder.path.toStdWString(), move, std::move(async),
            [count, failed, notify = m_notify](HRESULT hr, bool aborted) {
                // On the operation's thread, the pie perhaps gone (switched
                // off): nothing of it is touched.
                const bool cancelled = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED;
                if (FAILED(hr) && !cancelled) {
                    qWarning().noquote() << u"Drop pie: the file operation for %1 files failed (0x%2)"_s.arg(count).arg(
                        static_cast<quint32>(hr), 8, 16, u'0');
                    if (notify)
                        notify(failed, errorText(hr));
                } else {
                    qInfo().noquote() << u"Drop pie: file operation done%1"_s.arg(
                        aborted || cancelled ? u", partly cancelled"_s : QString());
                }
            });
    } else {
        // Not lasting files (a phone's or a zip file's contents, the Recycle
        // Bin's, an archiver's unpacked copies): the folder's own drop
        // target, as File Explorer hands them on, while the source still
        // holds them. It takes them from the source and returns.
        qInfo().noquote() << u"Drop pie: dropped, %1 %2 items through the folder's own drop target"_s
                                 .arg(move ? u"moving"_s : u"copying"_s)
                                 .arg(m_count);
        HRESULT hr = S_OK;
        effect = piedrop::dropOnFolder(folder.path.toStdWString(), data, move, pt, m_allowed, &hr);
        if (FAILED(hr)) {
            qWarning().noquote() << u"Drop pie: the folder did not take the items (0x%1)"_s.arg(
                static_cast<quint32>(hr), 8, 16, u'0');
            if (m_notify && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
                m_notify(failed, tr("这个文件夹不接受这些项目。"));
        } else {
            qInfo().noquote() << u"Drop pie: dropped on the folder itself, effect %1"_s.arg(effect);
        }
    }
    // The chip shown taken a moment, then gone.
    m_confirming = true;
    syncPageKeys();
    m_releaseWait.stop();
    emit stateChanged();
    m_confirmTimer.start(150ms);
    return effect;
}

// From the window's own corner in physical pixels: Qt's rectangle times the
// scale is off by a pixel at fractional scales.
QPointF DropPie::localOf(POINT pt) const
{
    RECT rect {};
    if (!m_hwnd || !::GetWindowRect(m_hwnd, &rect))
        return {-1e6, -1e6};
    return {(pt.x - rect.left) / m_dpr, (pt.y - rect.top) / m_dpr};
}

// The columns over the pie, a later one over an earlier; then the pie; then
// near a column (as near the chips: still on it); else outside all.
DropPie::Spot DropPie::spotAt(QPointF local) const
{
    const pie::ColumnMetrics metrics = columnMetrics();
    const int count = static_cast<int>(m_columns.size());
    for (int level = count; level >= 1; --level) {
        const Column& column = m_columns[static_cast<std::size_t>(level - 1)];
        const pie::ColumnHit hit
            = pie::hitColumn(local, column.box, column.first, static_cast<int>(column.rows.size()), metrics);
        switch (hit.part) {
        case pie::ColumnPart::Outside:
            continue;
        case pie::ColumnPart::Row:
            return {level, hit.row, Part::Row};
        case pie::ColumnPart::Up:
            return {level, -1, Part::Up};
        case pie::ColumnPart::Down:
            return {level, -1, Part::Down};
        case pie::ColumnPart::Box:
            return {level, -1, Part::Box};
        }
    }
    const pie::Hit hit = pie::hitTest(local - m_centre, m_layout, m_metrics);
    if (hit.zone == pie::Zone::Dead)
        return {0, -1, Part::Middle};
    if (hit.zone == pie::Zone::Sector)
        return {0, m_folders.empty() ? -1 : hit.sector, Part::Chip};
    const double reach = m_metrics.reach;
    const auto around = [&](int level) {
        return level == 0 ? m_layout.reach.translated(m_centre)
                          : m_columns[static_cast<std::size_t>(level - 1)].box.rect.adjusted(-reach, -reach, reach, reach);
    };
    for (int level = count; level >= 1; --level) {
        if (around(level).contains(local))
            return {level, -1, Part::Box};
    }
    // On the way from what a column came from to the column, which may be
    // well below it: across the gap between them (GitHub #463).
    for (int level = count; level >= 1; --level) {
        if (pie::inHullOf(local, around(level - 1), around(level)))
            return {level, -1, Part::Box};
    }
    return {};
}

// The centre's device pixel, as the window lies (DropPieWindow snaps it to
// one), at the scale of the screen it was placed on.
POINT DropPie::centreOnScreen() const
{
    RECT window {};
    ::GetWindowRect(m_hwnd, &window);
    return {window.left + static_cast<LONG>(std::lround(m_centre.x() * m_dpr)),
        window.top + static_cast<LONG>(std::lround(m_centre.y() * m_dpr))};
}

pie::Mode DropPie::modeOf(const Folder* folder) const
{
    if (!folder)
        return pie::Mode::None;
    const bool sameDrive = !m_itemsDrive.isEmpty() && driveOf(folder->path) == m_itemsDrive;
    return pie::modeFor(sameDrive, m_keys, allowed());
}

const DropPie::Folder* DropPie::folderAt(const Spot& spot, Block* block) const
{
    const Folder* folder = nullptr;
    Block found = Block::None;
    if (spot.level == 0 && spot.part == Part::Chip && spot.index >= 0
        && spot.index < static_cast<int>(m_layout.chips.size())) {
        const std::size_t at = folderOf(spot.index);
        if (at < m_folders.size() && at < m_blocks.size()) {
            folder = &m_folders[at];
            found = m_blocks[at];
        }
    } else if (spot.level >= 1 && spot.level <= static_cast<int>(m_columns.size()) && spot.part == Part::Row) {
        const Column& column = m_columns[static_cast<std::size_t>(spot.level - 1)];
        if (spot.index >= 0 && spot.index < static_cast<int>(column.rows.size())) {
            folder = &column.rows[static_cast<std::size_t>(spot.index)];
            found = column.blocks[static_cast<std::size_t>(spot.index)];
        }
    }
    if (block)
        *block = found;
    return folder;
}

const DropPie::Folder* DropPie::targetFolder() const
{
    Block block = Block::None;
    const Folder* folder = folderAt(m_spot, &block);
    return block == Block::None ? folder : nullptr;
}

bool DropPie::isOpener(const Spot& spot) const noexcept
{
    const bool folder = spot.level == 0 ? spot.part == Part::Chip : spot.part == Part::Row;
    return folder && spot.index >= 0 && spot.level < static_cast<int>(m_columns.size())
        && m_columns[static_cast<std::size_t>(spot.level)].from == spot.index;
}

QPointF DropPie::apex(QPointF local) const
{
    // Far enough back for the way it goes to be the hand's, not a pixel's jitter.
    const double enough = 4 * m_scale;
    for (auto it = m_trace.rbegin(); it != m_trace.rend(); ++it) {
        if (std::abs(it->x() - local.x()) + std::abs(it->y() - local.y()) >= enough)
            return *it;
    }
    return m_trace.front();
}

// As menus do (Amazon's "menu aim"): heading from where it was a moment ago
// into the triangle that ends on the column's near edge, a little past its
// corners.
bool DropPie::aiming(QPointF local, const Spot& spot) const
{
    if (m_trace.empty() || spot == m_spot || spot.level > m_spot.level || !isOpener(m_spot))
        return false;
    const pie::ColumnBox& next = m_columns[static_cast<std::size_t>(m_spot.level)].box;
    const double edge = next.side > 0 ? next.rect.left() : next.rect.right();
    const double slop = kAimSlop * m_scale;
    return pie::inTriangle(local, apex(local), QPointF(edge, next.rect.top() - slop),
        QPointF(edge, next.rect.bottom() + slop));
}

DWORD DropPie::track(DWORD keys, POINT pt)
{
    const QPointF local = localOf(pt);
    Spot spot = spotAt(local);
    // Just after the pointer went to the middle, moves from before it may
    // still come: taken as the middle, not as leaving.
    if (spot.part == Part::Outside && m_warped.isValid() && m_warped.elapsed() < 300)
        spot = {0, -1, Part::Middle};
    if (spot.part == Part::Outside) {
        hide("the pointer left it");
        return DROPEFFECT_NONE;
    }
    const pie::Keys held {.ctrl = bool(keys & MK_CONTROL), .shift = bool(keys & MK_SHIFT)};
    m_lastKeys = keys;
    m_lastAt = pt;
    // A page was just turned: the chip the pointer is over is not the one it
    // chose, so none is until it has moved a little from where it was. A
    // column came under it (its folder rested on just past the chip): that
    // folder stays chosen until then.
    if ((m_awaitMove || m_keepUntilMove)
        && std::hypot(double(pt.x - m_turnedAt.x), double(pt.y - m_turnedAt.y)) >= kTurnSlop * m_dpr)
        m_awaitMove = m_keepUntilMove = false;
    if (m_keepUntilMove)
        spot = m_spot;
    else if (m_awaitMove && spot.part == Part::Chip)
        spot.index = -1;
    else if (m_awaitMove && spot.part == Part::Row)
        spot = {spot.level, -1, Part::Box};
    // On its way to a column the pointer passes over chips or rows it is not
    // going for: what the column came from stays chosen while the pointer
    // heads for it, and while it stops there less than kAim; stopped longer,
    // what it is over is chosen after all (m_aim). OLE asks again with the
    // pointer still, so a stop is a point that stays, not a lack of asking.
    const bool moved = m_trace.empty()
        || std::abs(m_trace.back().x() - local.x()) + std::abs(m_trace.back().y() - local.y()) >= 0.5;
    bool keep = false;
    if (spot != m_spot && spot.level <= m_spot.level && isOpener(m_spot))
        keep = moved ? aiming(local, spot) : m_aim.isActive();
    if (!keep)
        m_aim.stop();
    else if (moved)
        m_aim.start();
    if (moved) {
        m_trace.push_back(local);
        if (m_trace.size() > 8)
            m_trace.erase(m_trace.begin());
    }
    if (keep)
        spot = m_spot;

    const int hovered = spot.level == 0 && spot.part == Part::Chip ? spot.index : -1;
    const int picked = hovered >= 0 && folderOf(hovered) < m_blocks.size() && m_blocks[folderOf(hovered)] == Block::None
        ? hovered
        : -1;
    const bool moving = spot != m_spot;
    const bool changed = moving || hovered != m_hovered || picked != m_picked || held.ctrl != m_keys.ctrl
        || held.shift != m_keys.shift;
    m_spot = spot;
    m_hovered = hovered;
    m_picked = picked;
    m_keys = held;
    if (moving)
        pointerMoved();
    if (changed) {
        updateTexts();
        emit stateChanged();
    }
    switch (modeOf(targetFolder())) {
    case pie::Mode::Move:
        return DROPEFFECT_MOVE & m_allowed;
    case pie::Mode::Copy:
        return DROPEFFECT_COPY & m_allowed;
    case pie::Mode::None:
        break;
    }
    return DROPEFFECT_NONE;
}

QString DropPie::mode() const
{
    const pie::Mode mode = modeOf(targetFolder());
    return mode == pie::Mode::Move ? u"move"_s : mode == pie::Mode::Copy ? u"copy"_s : QString();
}

QString DropPie::elide(const QString& text, int pixelSize, bool bold, qreal width) const
{
    return pielabels::elideMiddle(text, QFontMetricsF(fontOf(pixelSize, bold)), std::max<qreal>(0, width));
}

void DropPie::updateTexts()
{
    m_statusVerb.clear();
    m_statusHint.clear();
    m_statusDetail.clear();
    if (m_folders.empty()) {
        m_statusName = tr("还没有收藏文件夹");
        m_statusDetail = tr("在 设置 → 收藏夹 里添加");
        return;
    }
    Block block = Block::None;
    const Folder* folder = folderAt(m_spot, &block);
    if (!folder) {
        m_statusName = tr("朝文件夹拖，松开就放进去");
        m_statusDetail = tr("在中间松开或按 Esc 取消");
        if (m_pages > 1)
            m_statusHint = tr("Tab 下一页");
        return;
    }
    m_statusName = folder->name;
    if (block != Block::None) {
        m_statusDetail = reasonOf(block);
        return;
    }
    const pie::Mode mode = modeOf(folder);
    const int count = static_cast<int>(m_count);
    if (mode == pie::Mode::Copy) {
        m_statusVerb = count > 1 ? tr("复制 %n 项到", "", count) : tr("复制到");
        if (allowed().move)
            m_statusHint = tr("按住 Shift 移动");
    } else {
        m_statusVerb = count > 1 ? tr("移动 %n 项到", "", count) : tr("移动到");
        if (allowed().copy)
            m_statusHint = tr("按住 Ctrl 复制");
    }
    const QFontMetricsF fine(fontOf(std::max(12, m_labelPixelSize - 1)));
    const double hintWidth = m_statusHint.isEmpty() ? 0 : fine.horizontalAdvance(m_statusHint) + 12 * m_scale;
    m_statusDetail = pielabels::elidePath(folder->path, fine, m_statusRect.width() - 2 * kStatusPad * m_scale - hintWidth);
}

// ---- the folders -------------------------------------------------------------

void DropPie::gatherFolders()
{
    struct Candidate {
        QString path; // a folder's, or a place's command
        QString name;
        bool place = false;
    };
    std::vector<Candidate> candidates;
    const std::shared_ptr<const PlaceList> places = m_places ? m_places->places() : nullptr;
    for (const Favorite& f : m_favorites ? m_favorites->items() : FavoriteList()) {
        if (f.kind == Favorite::Kind::Folder) {
            candidates.push_back({normalized(f.path), f.name.isEmpty() ? pielabels::lastName(normalized(f.path)) : f.name});
        } else if (f.kind == Favorite::Kind::Place && places) {
            const auto place = std::ranges::find_if(*places, [&](const PlaceInfo& p) {
                return p.path().compare(f.path, Qt::CaseInsensitive) == 0;
            });
            if (place != places->end() && !isOwnPlace(*place)) // Win顺's settings are no place to drop on
                candidates.push_back({place->command, f.name.isEmpty() ? place->name : f.name, true});
        }
    }
    // Places are looked up through the shell, and drives asked whether they
    // are on the network: on the worker.
    m_worker.post([this, candidates = std::move(candidates)] {
        std::vector<Folder> folders;
        for (const Candidate& c : candidates) {
            const QString path = c.place ? normalized(placeFolder(c.path)) : c.path;
            if (path.isEmpty())
                continue;
            folders.push_back({path, c.name, isNetworkPath(path.toStdWString())});
        }
        QMetaObject::invokeMethod(this, [this, folders = std::move(folders)]() mutable {
            DropPieText::setFolderCount(static_cast<int>(folders.size()));
            if (m_open)
                hide("the favorites changed");
            m_folders = std::move(folders);
            m_blocks.assign(m_folders.size(), Block::None);
            updateSectors();
        }, Qt::QueuedConnection);
    });
}

// Which local folders are gone, asked again every time the pie opens: a
// folder may be deleted any time. Network ones never (a server that does not
// answer would hold this up for half a minute; its folder is taken as there).
void DropPie::checkFolders()
{
    std::vector<std::wstring> local;
    for (const Folder& f : m_folders) {
        if (!f.network)
            local.push_back(f.path.toStdWString());
    }
    const int check = ++m_checks;
    m_worker.post([this, check, local = std::move(local)] {
        QSet<QString> missing;
        for (const std::wstring& path : local) {
            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : ERROR_SUCCESS;
            const bool gone = attributes == INVALID_FILE_ATTRIBUTES
                ? error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_NOT_READY
                : !(attributes & FILE_ATTRIBUTE_DIRECTORY);
            if (gone)
                missing.insert(QString::fromStdWString(path).toCaseFolded());
        }
        QMetaObject::invokeMethod(this, [this, check, missing = std::move(missing)] {
            if (check != m_checks || missing == m_missing)
                return;
            m_missing = missing;
            if (!m_open || m_blocks.size() != m_folders.size())
                return;
            refreshBlocks();
            updateSectors();
            if (m_picked >= 0 && m_blocks[folderOf(m_picked)] != Block::None)
                m_picked = -1;
            updateTexts();
            emit stateChanged();
        }, Qt::QueuedConnection);
    });
}

DropPie::Block DropPie::blockOf(const Folder& folder) const
{
    const QString key = folder.path.toCaseFolded();
    if (m_missing.contains(key))
        return Block::Missing;
    // One of them, or inside one: up its few parents, not through every
    // item (a column's hundreds of rows for thousands of files).
    for (QString at = key; !at.isEmpty();) {
        if (m_itemKeys.contains(at))
            return Block::Inside;
        QString up = parentOf(at);
        if (up == at)
            break;
        at = std::move(up);
    }
    if (m_itemFolders.contains(key))
        return Block::Source;
    // Shell items go to the folder's own drop target on this thread, which
    // must not wait on a server.
    if (m_entered && !m_plain && folder.network)
        return Block::Network;
    return Block::None;
}

QString DropPie::reasonOf(Block block) const
{
    switch (block) {
    case Block::Missing:
        return tr("这个文件夹已不存在");
    case Block::Source:
        return tr("就是它们所在的文件夹");
    case Block::Inside:
        return tr("不能放进它自己里面");
    case Block::Network:
        return tr("这类项目只能放进本机的文件夹");
    case Block::None:
        break;
    }
    return {};
}

// The chips' labels and sizes, and where they go round the middle: those of
// the page shown. Every page is laid out, for the window and the line under
// the chips to be as big as the biggest of them: turning a page then moves and
// cuts off nothing, and a pointer on the way to a chip of the next is still on
// the pie.
void DropPie::updateSectors()
{
    const int step = DropPieText::uiStep();
    const qreal scale = pie::scaleForStep(step);
    if (scale != m_scale) {
        m_scale = scale;
        m_labelPixelSize = pie::labelPixelSize(step);
        m_metrics = pie::Metrics {}.scaled(m_scale);
        const QFontMetricsF line(fontOf(m_labelPixelSize + 1, true));
        const QFontMetricsF fine(fontOf(std::max(12, m_labelPixelSize - 1)));
        m_statusRect.setHeight(std::ceil(2 * kStatusPad * m_scale + line.height() + 3 + fine.height()));
    }
    const QFontMetricsF label(DropPieText::labelFont());
    const QFontMetricsF hintFont(fontOf(std::max(12, m_labelPixelSize - 1)));
    const double nameWidth = DropPieText::nameWidth();
    // One chip's words, and how wide it is.
    const auto chipOf = [&](const QString& name, const QString& path, const QString& parent, Block block, double& width) {
        const QString shown = pielabels::elideMiddle(name.simplified(), label, nameWidth);
        const QString hint = parent.isEmpty() ? QString() : pielabels::elideMiddle(parent, hintFont, kHintWidth * m_scale);
        width = (kChipPadStart + kGlyph + kGlyphGap + kChipPadEnd) * m_scale + std::ceil(label.horizontalAdvance(shown));
        if (!hint.isEmpty())
            width += kHintGap * m_scale + std::ceil(hintFont.horizontalAdvance(hint));
        width = std::max(width, kMinChip * m_scale);
        return QVariantMap {{u"name"_s, name}, {u"label"_s, shown}, {u"hint"_s, hint}, {u"path"_s, path},
            {u"disabled"_s, block != Block::None}, {u"reason"_s, reasonOf(block)}};
    };
    const int total = static_cast<int>(m_folders.size());
    m_page = pie::clampPage(m_page, total); // the favorites may have shrunk under it
    const int pages = pie::pageCount(total);
    QVariantList items;
    QRectF allBounds;
    QRectF allReach;
    for (int page = 0; page < pages; ++page) {
        const pie::PageSlice slice = pie::pageSlice(total, page);
        std::vector<double> widths;
        QVariantList pageItems;
        if (m_folders.empty()) {
            double width = 0;
            pageItems.append(chipOf(tr("还没有收藏文件夹"), QString(), QString(), Block::Missing, width));
            widths.push_back(width);
        } else {
            // A parent's name beside a name that another on the same page has.
            QStringList names;
            QStringList paths;
            for (int i = 0; i < slice.count; ++i) {
                names.append(m_folders[static_cast<std::size_t>(slice.first + i)].name);
                paths.append(m_folders[static_cast<std::size_t>(slice.first + i)].path);
            }
            const QStringList hints = pielabels::parentHints(names, paths);
            for (int i = 0; i < slice.count; ++i) {
                const auto at = static_cast<std::size_t>(slice.first + i);
                double width = 0;
                pageItems.append(chipOf(m_folders[at].name, m_folders[at].path, hints.value(i),
                    at < m_blocks.size() ? m_blocks[at] : Block::None, width));
                widths.push_back(width);
            }
        }
        const pie::Layout layout = pie::layOut(widths, m_metrics);
        allBounds = allBounds.united(layout.bounds);
        allReach = allReach.united(layout.reach);
        if (page != m_page)
            continue;
        m_layout = layout;
        m_pageFirst = slice.first;
        for (qsizetype i = 0; i < pageItems.size(); ++i) {
            const QRectF& chip = layout.chips[static_cast<std::size_t>(i)];
            QVariantMap map = pageItems[i].toMap();
            map.insert(u"x"_s, chip.x());
            map.insert(u"y"_s, chip.y());
            map.insert(u"width"_s, chip.width());
            pageItems[i] = map;
        }
        items = std::move(pageItems);
    }
    m_allBounds = allBounds;
    m_layout.reach = allReach;
    const double statusWidth = std::clamp(m_allBounds.width(), kStatusMin * m_scale, kStatusMax * m_scale);
    if (statusWidth != m_statusRect.width()) {
        m_statusRect.setWidth(statusWidth);
        emit layoutChanged();
    }
    const bool itemsChanged = items != m_sectorItems;
    if (itemsChanged) {
        m_sectorItems = std::move(items);
        emit sectorsChanged();
    }
    if (m_page != m_pageNotified || pages != m_pages) {
        m_pageNotified = m_page;
        m_pages = pages;
        emit pageChanged();
    }
}

// ---- the columns (GitHub #463) -----------------------------------------------

void DropPie::setItems(std::vector<std::wstring> paths)
{
    m_items = std::move(paths);
    m_itemKeys.clear();
    m_itemFolders.clear();
    for (const std::wstring& item : m_items) {
        const QString path = normalized(QString::fromStdWString(item));
        m_itemKeys.insert(path.toCaseFolded());
        m_itemFolders.insert(parentOf(path).toCaseFolded());
    }
}

void DropPie::refreshBlocks()
{
    m_blocks.resize(m_folders.size());
    for (std::size_t i = 0; i < m_folders.size(); ++i)
        m_blocks[i] = blockOf(m_folders[i]);
    bool changed = false;
    for (Column& column : m_columns) {
        column.blocks.resize(column.rows.size());
        for (std::size_t i = 0; i < column.rows.size(); ++i) {
            const Block block = blockOf(column.rows[i]);
            changed = changed || block != column.blocks[i];
            column.blocks[i] = block;
        }
    }
    if (changed) {
        updateColumnItems();
        emit columnsChanged();
    }
}

void DropPie::resetColumns()
{
    m_dwell.stop();
    m_slow.stop();
    m_scroll.stop();
    m_aim.stop();
    m_pending.reset();
    m_listings.clear();
    m_reading.clear();
    ++m_opening; // what is still being read is for nothing now
    if (m_columns.empty() && m_columnItems.isEmpty())
        return;
    m_columns.clear();
    m_columnItems.clear();
    emit columnsChanged();
}

// The pointer onto something else: a strip scrolls its column while the
// pointer stays on it; resting a moment on anything else opens a column, or
// closes those after it (dwelt).
void DropPie::pointerMoved()
{
    if (m_spot.part == Part::Up || m_spot.part == Part::Down) {
        m_dwell.stop();
        if (!m_scroll.isActive())
            m_scroll.start();
        return;
    }
    m_scroll.stop();
    m_dwell.start();
}

void DropPie::dwelt()
{
    if (!m_open || m_confirming || m_spot.part == Part::Up || m_spot.part == Part::Down)
        return;
    if (isOpener(m_spot)) { // back on what the next column came from: those after that one go
        closeColumnsFrom(m_spot.level + 1);
        return;
    }
    if (m_pending && m_pending->level == m_spot.level && m_pending->index == m_spot.index)
        return; // its folders are on their way
    closeColumnsFrom(m_spot.level);
    if (opens(m_spot))
        openColumn(m_spot.level, m_spot.index);
}

// Not a folder the items are in, nor inside: nothing could go further in.
bool DropPie::opens(const Spot& spot) const
{
    Block block = Block::None;
    const Folder* folder = folderAt(spot, &block);
    return folder && (block == Block::None || block == Block::Source);
}

void DropPie::openColumn(int level, int index)
{
    const Folder* folder = folderAt({level, index, level == 0 ? Part::Chip : Part::Row});
    if (!folder)
        return;
    const QString key = folder->path.toCaseFolded();
    if (const auto known = m_listings.constFind(key); known != m_listings.cend()) {
        const QStringList names = *known;
        showColumn(level, index, &names);
        return;
    }
    m_pending = Pending {level, index, folder->path};
    m_slow.start();
    if (!m_reading.contains(key)) {
        m_reading.insert(key);
        listFolder(folder->path);
    }
}

void DropPie::showColumn(int level, int index, const QStringList* names)
{
    const Folder* folder = folderAt({level, index, level == 0 ? Part::Chip : Part::Row});
    if (!folder)
        return;
    Column column;
    column.folder = *folder;
    column.from = index;
    if (names)
        fillColumn(column, *names);
    else
        column.message = tr("正在读取…");
    column.width = columnWidth(column);
    const std::optional<Pending> reading = names ? std::nullopt : m_pending; // its rows still to come
    closeColumnsFrom(level);
    m_pending = reading;
    m_columns.push_back(std::move(column));
    layOutColumns();
    // Come under the pointer where DragOver last had it (OLE asks every so
    // often while it is still): what it had chosen stays until it moves (track).
    if (m_entered && m_columns.back().box.rect.contains(localOf(m_lastAt))) {
        m_keepUntilMove = true;
        m_turnedAt = m_lastAt;
    }
    updateColumnItems();
    emit columnsChanged();
    emit stateChanged();
}

void DropPie::closeColumnsFrom(int level)
{
    level = std::max(0, level);
    if (m_pending && m_pending->level >= level) {
        m_pending.reset();
        m_slow.stop();
    }
    if (static_cast<int>(m_columns.size()) <= level)
        return;
    m_columns.erase(m_columns.begin() + level, m_columns.end());
    if (m_spot.level > level) { // in one of them: nothing there now, until the pointer is next asked about
        m_spot = {level, -1, Part::Box};
        m_scroll.stop();
        m_aim.stop();
    }
    updateColumnItems();
    emit columnsChanged();
    emit stateChanged();
}

// Off the GUI thread (a share may take its time), as the folder menu's: the
// answer for a showing of the pie that is over is dropped.
void DropPie::listFolder(const QString& path)
{
    QThreadPool::globalInstance()->start([self = QPointer(this), opening = m_opening, path] {
        QStringList names = FolderMenu::subfolders(path, kMaxRows);
        QMetaObject::invokeMethod(qApp, [self, opening, path, names = std::move(names)] {
            if (self && self->m_opening == opening)
                self->listed(path, names);
        }, Qt::QueuedConnection);
    });
}

void DropPie::listed(const QString& path, const QStringList& names)
{
    const QString key = path.toCaseFolded();
    m_reading.remove(key);
    m_listings.insert(key, names);
    if (!m_open || m_confirming || !m_pending || m_pending->path.toCaseFolded() != key)
        return;
    const Pending pending = *m_pending;
    showColumn(pending.level, pending.index, &names);
}

void DropPie::fillColumn(Column& column, const QStringList& names) const
{
    column.rows.clear();
    column.rows.reserve(static_cast<std::size_t>(names.size()));
    for (const QString& name : names)
        column.rows.push_back({childOf(column.folder.path, name), name, column.folder.network});
    column.blocks.clear();
    column.blocks.reserve(column.rows.size());
    for (const Folder& row : column.rows)
        column.blocks.push_back(blockOf(row));
    column.message = column.rows.empty() ? tr("没有子文件夹") : QString();
}

void DropPie::scrollColumn()
{
    const int level = m_spot.level;
    const bool strip = m_spot.part == Part::Up || m_spot.part == Part::Down;
    if (!strip || level < 1 || level > static_cast<int>(m_columns.size()) || m_confirming) {
        m_scroll.stop();
        return;
    }
    Column& column = m_columns[static_cast<std::size_t>(level - 1)];
    const int first = pie::clampFirst(column.first + (m_spot.part == Part::Up ? -1 : 1),
        static_cast<int>(column.rows.size()), column.box.shown);
    if (first == column.first) {
        m_scroll.stop(); // at its end
        return;
    }
    column.first = first;
    closeColumnsFrom(level); // their rows moved from under them
    emit stateChanged();
}

// The first column beside the pie on the side of its chip, its first row
// level with the chip (clear of the line under the chips); each next beside
// the one before, level with the row it came from, on the same side while
// there is room.
void DropPie::layOutColumns()
{
    const pie::ColumnMetrics metrics = columnMetrics();
    const QRectF pieBox = m_layout.bounds.translated(m_centre);
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        Column& column = m_columns[i];
        const int rows = std::max(1, static_cast<int>(column.rows.size()));
        if (i == 0) {
            const auto from = static_cast<std::size_t>(column.from);
            const QRectF chip = from < m_layout.chips.size() ? m_layout.chips[from].translated(m_centre) : pieBox;
            const int side = chip.center().x() < m_centre.x() - 0.5 ? -1 : 1;
            pie::ColumnMetrics first = metrics;
            first.gap = m_metrics.chipGap;
            column.box = pie::placeColumn(pieBox, chip.center().y(), side, column.width, rows, m_area, first);
            if (column.box.rect.intersects(m_statusRect))
                column.box = pie::placeColumn(pieBox.united(m_statusRect), chip.center().y(), side, column.width, rows,
                    m_area, first);
        } else {
            const Column& before = m_columns[i - 1];
            const double levelY = before.box.rowsTop(metrics) + (column.from - before.first + 0.5) * metrics.row;
            column.box = pie::placeColumn(before.box.rect, levelY, before.box.side, column.width, rows, m_area, metrics);
        }
        column.first = pie::clampFirst(column.first, static_cast<int>(column.rows.size()), column.box.shown);
    }
}

void DropPie::updateColumnItems()
{
    const pie::ColumnMetrics metrics = columnMetrics();
    QVariantList items;
    for (const Column& column : m_columns) {
        QVariantList rows;
        if (column.rows.empty())
            rows.append(QVariantMap {{u"text"_s, column.message}, {u"disabled"_s, true}, {u"message"_s, true}});
        for (std::size_t i = 0; i < column.rows.size(); ++i) {
            const Block block = column.blocks[i];
            rows.append(QVariantMap {{u"text"_s, column.rows[i].name}, {u"disabled"_s, block != Block::None},
                {u"branch"_s, block == Block::None || block == Block::Source}});
        }
        const QRectF& box = column.box.rect;
        items.append(QVariantMap {{u"x"_s, box.x()}, {u"y"_s, box.y()}, {u"width"_s, box.width()},
            {u"height"_s, box.height()}, {u"scrolls"_s, column.box.scrolls}, {u"shown"_s, column.box.shown},
            {u"strip"_s, metrics.strip()}, {u"rows"_s, rows}});
    }
    m_columnItems = std::move(items);
}

QVariantList DropPie::columnStates() const
{
    QVariantList states;
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        const Column& column = m_columns[i];
        const bool here = m_spot.level == static_cast<int>(i) + 1;
        const int hovered = here && m_spot.part == Part::Row ? m_spot.index : -1;
        const bool target = hovered >= 0 && column.blocks[static_cast<std::size_t>(hovered)] == Block::None;
        const int strip = !here ? 0 : m_spot.part == Part::Up ? -1 : m_spot.part == Part::Down ? 1 : 0;
        states.append(QVariantMap {{u"first"_s, column.first}, {u"hovered"_s, hovered}, {u"target"_s, target},
            {u"opener"_s, i + 1 < m_columns.size() ? m_columns[i + 1].from : -1}, {u"up"_s, column.first > 0},
            {u"down"_s, column.first + column.box.shown < static_cast<int>(column.rows.size())}, {u"strip"_s, strip}});
    }
    return states;
}

// As the window draws them (DropPieWindow: Theme's menu rows), read off it;
// as the standard size's without it.
pie::ColumnMetrics DropPie::columnMetrics() const
{
    pie::ColumnMetrics metrics;
    metrics.row = windowNumber(m_window, "columnRow", std::round(32 * m_scale));
    metrics.padding = windowNumber(m_window, "columnPadding", 4);
    metrics.gap = kColumnGap;
    return metrics;
}

// As the folder menu's (FolderMenuColumn::rowWidth): its widest row, between
// its narrowest and widest.
double DropPie::columnWidth(const Column& column) const
{
    const pie::ColumnMetrics metrics = columnMetrics();
    const QFontMetricsF text(fontOf(static_cast<int>(windowNumber(m_window, "columnTextSize", m_labelPixelSize))));
    double widest = column.rows.empty() ? kRowName + std::ceil(text.horizontalAdvance(column.message)) + kRowEnd : 0;
    for (const Folder& row : column.rows)
        widest = std::max(widest, kRowName + std::ceil(text.horizontalAdvance(row.name)) + kRowEnd + kRowArrow);
    const double least = windowNumber(m_window, "columnMinWidth", 180);
    const double most = std::max(least, std::min(windowNumber(m_window, "columnMaxWidth", 420),
                                            m_area.width() - 2 * metrics.margin));
    return std::clamp(std::ceil(widest + 2 * metrics.padding), least, most);
}

} // namespace ws
