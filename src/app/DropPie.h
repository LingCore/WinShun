#pragma once

#include "PieGeometry.h"
#include "platform/ComWorker.h"
#include "platform/PieDrop.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QQuickWindow;
class QScreen;

namespace ws {

class Favorites;
class SystemCatalog;

// 拖放圆盘: while files are dragged (File Explorer, the desktop, any file
// manager that drags with OLE), or a file only held, the key under Esc
// (` and ·) brings up the favorite folders round the pointer, as small
// chips, eight to a page. The direction the pointer goes from the middle
// chooses one (PieGeometry): let go and the files are moved there within a
// drive, copied to another, Ctrl and Shift changing that as in File
// Explorer. Let go in the middle, or press Esc, and nothing happens; leave
// the chips and they go away, the drag carrying on as before; the key again
// does the same. With more than eight folders Tab turns to the next page
// (Shift+Tab: the one before; they go round): KeyListener holds those keys
// only while the pie shows several pages, and nothing is chosen on a new page
// until the pointer has moved.
//
// KeyListener sees the key (Raw Input, or its hotkey) and that an OLE drag is
// under way; this shows the window (DropPieWindow.qml, made once and kept)
// without taking the focus, and is its OLE drop target in place of Qt's
// (piedrop::DropTarget). It shows itself only once the drag enters it with
// files or shell items: a drag of text (Word, VS Code) passes the
// listener's test too. Lasting files are copied or moved by IFileOperation
// on threads of the application's (piedrop::FileOperations) and the source
// is told nothing was dropped, so it never deletes anything itself (one that
// drops asynchronously, File Explorer, is held until the copy is over);
// everything else (a phone's or a zip file's contents, the Recycle Bin's, an
// archiver's unpacked copies in the temporary folder) goes to the folder's
// own drop target while the source still holds it, as in File Explorer
// (piedrop::DragItems).
//
// The pointer resting a moment on a folder (a chip, or a row) brings up the
// folders inside it in a column beside it, as the folder menu's columns
// (GitHub #463): beside the pie on the chip's side, the next beside the
// column it came from, and so on down. Let go on a row and they go into that
// folder; resting elsewhere closes the columns after it, as a menu's do. A
// column too long for the screen scrolls while the pointer rests on the
// strip at its top or bottom.
//
// Its window is see-through but still takes the pointer where nothing is
// drawn (a layered D3D11 window): it goes the moment the pointer leaves the
// chips' and the columns' reach, so it never stands between a drag and what
// is under it. It covers the work area of the screen the pie is on, so that
// a column comes without the window changing while it is seen (that shows a
// frame of the old picture moved).
class DropPie : public QObject, private piedrop::Handler {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    // The folders' chips of the page shown: {name, label, hint, path,
    // disabled, reason, x, y, width}: label the name as shown (shortened in
    // its middle), hint its parent folder's name when another on that page has
    // the same name; x, y the chip's top left from the centre.
    Q_PROPERTY(QVariantList sectors READ sectors NOTIFY sectorsChanged FINAL)
    // The pages (eight folders each, in the favorites' order): the one shown,
    // from 0, and how many; the chips of the page just left, for the window to
    // fade out as the new ones fade in (pageTurned).
    Q_PROPERTY(int page READ page NOTIFY pageChanged FINAL)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pageChanged FINAL)
    Q_PROPERTY(QVariantList leavingSectors READ leavingSectors NOTIFY pageChanged FINAL)
    Q_PROPERTY(bool noFolders READ noFolders NOTIFY sectorsChanged FINAL)
    Q_PROPERTY(QPointF centre READ centre NOTIFY layoutChanged FINAL) // in the window
    Q_PROPERTY(qreal sizeScale READ sizeScale NOTIFY layoutChanged FINAL) // 界面大小
    Q_PROPERTY(int labelPixelSize READ labelPixelSize NOTIFY layoutChanged FINAL)
    Q_PROPERTY(qreal hubRadius READ hubRadius NOTIFY layoutChanged FINAL)
    Q_PROPERTY(qreal chipHeight READ chipHeight NOTIFY layoutChanged FINAL)
    Q_PROPERTY(QRectF statusRect READ statusRect NOTIFY layoutChanged FINAL) // in the window
    Q_PROPERTY(bool revealed READ revealed NOTIFY revealedChanged FINAL)
    Q_PROPERTY(int hovered READ hovered NOTIFY stateChanged FINAL) // the sector the pointer is in, -1
    Q_PROPERTY(int picked READ picked NOTIFY stateChanged FINAL) // ... if it can be dropped on, else -1
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged FINAL) // the picked one's: "move", "copy" or ""
    Q_PROPERTY(bool confirming READ confirming NOTIFY stateChanged FINAL) // dropped: the chip flashes
    Q_PROPERTY(bool inMiddle READ inMiddle NOTIFY stateChanged FINAL) // the pointer in the middle: letting go cancels
    Q_PROPERTY(bool onFolder READ onFolder NOTIFY stateChanged FINAL) // a chip or a row under the pointer: the line names it
    // The columns of folders inside folders, the first from a chip: {x, y,
    // width, height (the box, in the window), scrolls, strip (a scroll strip's
    // height), rows: [{text, disabled}] (one disabled row that says it is being
    // read or is empty)}. What each shows of them now is columnStates: {first
    // (the first row in sight), hovered (the row under the pointer, -1), target
    // (that row is where letting go puts them), opener (the row the next column
    // came from, -1), up, down (whether its strips scroll further)}.
    Q_PROPERTY(QVariantList columns READ columns NOTIFY columnsChanged FINAL)
    Q_PROPERTY(QVariantList columnStates READ columnStates NOTIFY stateChanged FINAL)
    Q_PROPERTY(int openedSector READ openedSector NOTIFY stateChanged FINAL) // the chip the first column came from, -1
    // The line under the chips: what letting go does ("移动到" and the
    // folder's name, its path, the key that changes it) or why it cannot.
    Q_PROPERTY(QString statusVerb READ statusVerb NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusName READ statusName NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY stateChanged FINAL) // the path, shortened
    Q_PROPERTY(QString statusHint READ statusHint NOTIFY stateChanged FINAL)

public:
    // A short notice to the user (the tray's), callable from any thread.
    using Notify = std::function<void(const QString& title, const QString& text)>;

    // `operations` is the application's (it outlives the pie and waits for
    // copies under way when the application goes); `notify` tells the user
    // when one fails.
    DropPie(Favorites* favorites, SystemCatalog* places, piedrop::FileOperations* operations, Notify notify,
        QObject* parent = nullptr);
    ~DropPie() override;

    // DropPieWindow: made into a see-through window and taken over as a
    // drop target here, then shown once out of sight so that its first
    // real showing is not slowed by the graphics' setting up.
    void setWindow(QQuickWindow* window);
    // The off-screen preview (Preview.cpp, --style-windows): laid out as a
    // drag at `pt` opens it, revealed, the pointer in sector `hovered` (-1:
    // the middle); no drop site taken, the window left to the preview to show.
    void preview(POINT pt, int hovered);
    // ... and turned to `page` at once (no fade), the pointer in sector
    // `hovered` of it (-1: the middle).
    void previewPage(int page, int hovered = -1);
    // ... the folders inside the one at `index` of the last level (the pie's
    // chip on the page shown when no column is up, else the last column's
    // row) as the next column, read here and now; then the pointer at row
    // `row` of column `column` (from 0; -1 none), or on its strip (`strip` -1
    // up, 1 down), the column scrolled to `first`.
    bool previewOpen(int index);
    void previewColumnPointer(int column, int row, int first = 0, int strip = 0);
    // ... the pointer at `local` (in the window, logical pixels) as a
    // DragOver brings it, with `keys` (MK_*) held: the effect it answers.
    DWORD previewPointer(QPointF local, DWORD keys = 0);
    // What is drawn of it, in the window (logical pixels): the chips of
    // every page, the middle, the line under them, the columns, with room for
    // their shadows. The window covers the work area; a preview cuts this out.
    QRectF drawnRect() const;
    bool dropReady() const noexcept { return m_hwnd && m_target; }

    // From the listener thread (KeyListener::Callbacks): during an OLE drag
    // at `pt` (physical pixels), whose source has captured the mouse with
    // `captureWindow`, the pie's key pressed (again: it goes); the primary
    // button let go.
    void summoned(POINT pt, HWND captureWindow, DWORD sourcePid);
    void buttonReleased();
    // Tab (back: Shift+Tab) pressed while the listener holds those keys for
    // the pie (pageKeysWanted).
    void pageKey(bool back);
    // The notice when another program's hotkey has the pie's key
    // (KeyListener::Callbacks::dragKeyRefused, GitHub #150).
    static QString keyTakenTitle();
    static QString keyTakenText();
    // Inside a drop (the folder's drop target may run a dialog's loop): not
    // to be destroyed now.
    bool inDrop() const noexcept { return m_inDrop; }

    QVariantList sectors() const { return m_sectorItems; }
    int page() const noexcept { return m_page; }
    int pageCount() const noexcept { return m_pages; }
    QVariantList leavingSectors() const { return m_leaving; }
    bool noFolders() const noexcept { return m_folders.empty(); }
    int folderCount() const noexcept { return static_cast<int>(m_folders.size()); }
    QPointF centre() const noexcept { return m_centre; }
    qreal sizeScale() const noexcept { return m_scale; }
    int labelPixelSize() const noexcept { return m_labelPixelSize; }
    qreal hubRadius() const noexcept { return m_metrics.hub; }
    qreal chipHeight() const noexcept { return m_metrics.chipHeight; }
    QRectF statusRect() const noexcept { return m_statusRect; }
    bool revealed() const noexcept { return m_revealed; }
    int hovered() const noexcept { return m_hovered; }
    int picked() const noexcept { return m_picked; }
    QString mode() const;
    bool confirming() const noexcept { return m_confirming; }
    bool inMiddle() const noexcept { return m_spot.part == Part::Middle; }
    bool onFolder() const noexcept { return pointedFolder() != nullptr; }
    QVariantList columns() const { return m_columnItems; }
    QVariantList columnStates() const;
    int openedSector() const noexcept { return m_columns.empty() ? -1 : m_columns.front().from; }
    QString statusVerb() const { return m_statusVerb; }
    QString statusName() const { return m_statusName; }
    QString statusDetail() const { return m_statusDetail; }
    QString statusHint() const { return m_statusHint; }

    // `text` shortened in its middle with "…" to `width` logical pixels, in
    // the application's font at `pixelSize` (Bold if `bold`): the same
    // fitter as the chips' labels, for the line under them.
    Q_INVOKABLE QString elide(const QString& text, int pixelSize, bool bold, qreal width) const;

signals:
    void sectorsChanged();
    void pageChanged(); // the page, how many there are, or the page just left
    void pageTurned(); // by Tab: the chips fade from the page left to the new one
    // The listener is to hold Tab and Shift+Tab (KeyListener::holdDragPages)
    // while the pie shows several pages, and to let go of them when it does not.
    void pageKeysWanted(bool on);
    void layoutChanged();
    void revealedChanged();
    void stateChanged();
    void columnsChanged();

private:
    struct Folder {
        QString path; // file-system path, no trailing backslash but a drive's
        QString name; // as the favorite is named
        bool network = false;
    };
    enum class Block { None, Missing, Source, Inside, Network };
    // Where the pointer is: the middle; a chip's direction (Chip, `index` the
    // sector, -1 when none is chosen yet); in column `level` (from 1) on a
    // row (`index`), on a strip, elsewhere in it or near it (Box); outside all.
    enum class Part { Outside, Middle, Chip, Row, Box, Up, Down };
    struct Spot {
        int level = 0; // 0 the pie, k the k-th column
        int index = -1;
        Part part = Part::Outside;
        friend bool operator==(const Spot&, const Spot&) = default;
    };
    // A column of the folders inside `folder`, beside the chip (the first) or
    // the row of the column before it that it came from (`from`).
    struct Column {
        Folder folder;
        int from = -1;
        std::vector<Folder> rows;
        std::vector<Block> blocks; // by row
        QString message; // instead of rows: being read, or there are none
        double width = 0; // as its rows want (columnWidth)
        pie::ColumnBox box;
        int first = 0; // the first row in sight
    };

    // piedrop::Handler, on the GUI thread
    DWORD dragEnter(IDataObject* data, DWORD keys, POINT pt, DWORD allowed) override;
    DWORD dragOver(DWORD keys, POINT pt, DWORD allowed) override;
    void dragLeave() override;
    DWORD drop(IDataObject* data, DWORD keys, POINT pt, DWORD allowed) override;

    void onSummoned(POINT pt, HWND captureWindow, DWORD sourcePid);
    void open(POINT pt);
    QRect place(QScreen* screen, POINT pt); // the window's, the middle and the line in it, m_warp
    void follow(POINT pt); // the window placed for the pointer at `pt`
    DWORD reveal(DWORD keys, POINT pt);
    void hide(const char* why);
    void nudge() const;
    bool takeDropSite();
    void prewarm();
    void gatherFolders(); // favorites -> m_folders, places resolved on the worker
    void checkFolders(); // which local ones are missing, on the worker
    void updateSectors();
    Block blockOf(const Folder& folder) const;
    QString reasonOf(Block block) const;
    QPointF localOf(POINT pt) const; // a physical point in the window's logical pixels
    Spot spotAt(QPointF local) const;
    POINT centreOnScreen() const; // physical pixels
    pie::Allowed allowed() const noexcept { return {bool(m_allowed & DROPEFFECT_MOVE), bool(m_allowed & DROPEFFECT_COPY)}; }
    pie::Mode modeOf(const Folder* folder) const;
    DWORD track(DWORD keys, POINT pt); // DragOver's work: the state and the effect
    void updateTexts();
    void setRevealed(bool revealed);
    void turnPage(bool back);
    void syncPageKeys();
    // The favorite a sector of the page shown stands for.
    std::size_t folderOf(int sector) const noexcept { return static_cast<std::size_t>(m_pageFirst + sector); }
    // The folder a chip or a row of `spot` stands for, and its block; null
    // for anything else. pointedFolder: the one under the pointer (named by the
    // line under the chips); targetFolder: the same if it can be dropped on.
    const Folder* folderAt(const Spot& spot, Block* block = nullptr) const;
    const Folder* pointedFolder() const { return folderAt(m_spot); }
    const Folder* targetFolder() const;
    void refreshBlocks(); // the favorites' and the columns' rows' blocks, for this drag
    void resetColumns(); // all closed, nothing pending, the folders read forgotten (a new opening)

    // The columns: the pointer resting on a chip or a row opens its column, on
    // anything else closes the columns after it (dwell); on a strip scrolls.
    void pointerMoved();
    void dwelt();
    bool opens(const Spot& spot) const; // a chip or row whose folders can be listed
    bool isOpener(const Spot& spot) const noexcept; // the chip or row the next column came from
    // Menu aim: the pointer, at `local` over `spot`, still on its way from
    // the opener (m_spot) to its column, which it is heading for.
    bool aiming(QPointF local, const Spot& spot) const;
    QPointF apex(QPointF local) const; // where the pointer was a little way back (m_trace)
    void setItems(std::vector<std::wstring> paths); // m_items, and what blockOf looks them up by
    void openColumn(int level, int index); // the column of `index` at `level`, now or once read
    void showColumn(int level, int index, const QStringList* names); // up, its rows or "reading"
    void closeColumnsFrom(int level); // columns level+1 on (level 0: all)
    void listFolder(const QString& path);
    void listed(const QString& path, const QStringList& names);
    void fillColumn(Column& column, const QStringList& names) const;
    void scrollColumn();
    void layOutColumns(); // their boxes, the first beside the pie
    void updateColumnItems();
    pie::ColumnMetrics columnMetrics() const;
    double columnWidth(const Column& column) const;

    Favorites* m_favorites;
    SystemCatalog* m_places;
    QPointer<QQuickWindow> m_window;
    HWND m_hwnd = nullptr; // the window's, as registered for drops
    Microsoft::WRL::ComPtr<piedrop::DropTarget> m_target;

    std::vector<Folder> m_folders; // every favorite folder, a page of eight at a time (pie::pageSlice)
    QSet<QString> m_missing; // case-folded paths of local folders found missing
    QVariantList m_sectorItems; // the page shown
    QVariantList m_leaving; // the page before the last turn
    int m_page = 0;
    int m_pageFirst = 0; // the first favorite on it
    int m_pages = 1;
    int m_pageNotified = 0; // what pageChanged last said
    pie::Metrics m_metrics;
    pie::Layout m_layout; // the page shown, its reach that of every page
    QRectF m_allBounds; // the chips and the middle of every page: where the window and the line go
    qreal m_scale = 0; // none yet: the first updateSectors sets it
    int m_labelPixelSize = 13;
    QPointF m_centre; // in the window
    QRectF m_statusRect; // in the window
    QRectF m_area; // the work area, in the window: where the columns go

    // The columns, from the first (beside the pie) on, and what QML draws of them.
    std::vector<Column> m_columns;
    QVariantList m_columnItems;
    // The folders inside folders read while the pie is up, by case-folded path.
    QHash<QString, QStringList> m_listings;
    struct Pending { // a folder being read, its column not up yet
        int level = 0;
        int index = -1;
        QString path;
    };
    std::optional<Pending> m_pending;
    QSet<QString> m_reading; // case-folded paths being read now
    std::uint64_t m_opening = 0; // which showing of the pie a folder read belongs to
    QTimer m_dwell; // the pointer resting where it is: its column opens, or those after close
    QTimer m_slow; // the folder pending is slow to read: its column says so meanwhile
    QTimer m_scroll; // the pointer on a scroll strip
    QTimer m_aim; // on the way to a column, stopped over something else: that is chosen after all
    std::vector<QPointF> m_trace; // the pointer's last few places, in the window (menu aim)
    DWORD m_lastKeys = 0; // the last DragOver's

    // The drag over the pie
    bool m_open = false; // shown (revealed or not)
    bool m_entered = false; // DragEnter came
    bool m_revealed = false;
    bool m_confirming = false;
    bool m_pageKeys = false; // pageKeysWanted(true) was the last word
    // A page was turned and the pointer has not moved since: nothing under
    // it is chosen yet. A column came under the pointer and it has not moved
    // since: what it had chosen stays chosen (m_keepUntilMove). OLE asks
    // again and again with the pointer still, so it is the point that counts,
    // not the asking.
    bool m_awaitMove = false;
    bool m_keepUntilMove = false;
    POINT m_turnedAt {};
    POINT m_lastAt {}; // the last DragOver's
    bool m_inDrop = false;
    DWORD m_allowed = 0; // what the source allows
    std::vector<std::wstring> m_items; // the file-system paths of what is dragged (piedrop::DragItems)
    QSet<QString> m_itemKeys; // ... normalized and case-folded
    QSet<QString> m_itemFolders; // ... the folders they are in, the same
    std::size_t m_count = 0; // ... and how many items there are
    bool m_plain = false; // ... all lasting files: copied by our own file operation
    std::vector<Block> m_blocks; // by favorite, for this drag
    QString m_itemsDrive; // the drive every item is on (driveOf), empty when not one
    Spot m_spot; // where the pointer is
    pie::Keys m_keys;
    int m_hovered = -1; // the pie's: the chip the pointer is on or heads for
    int m_picked = -1;
    QString m_statusVerb;
    QString m_statusName;
    QString m_statusDetail;
    QString m_statusHint;
    POINT m_shownAt {};
    qreal m_dpr = 1; // the scale of the screen it is on (the window's may lag behind on its first showing there)
    bool m_warp = false; // placed off the pointer by a screen's edge: the pointer goes to the middle at DragEnter
    QElapsedTimer m_warped; // since it went: moves OLE saw before it are still on their way
    QElapsedTimer m_shownClock;
    QTimer m_revealWait; // no DragEnter yet: the pointer still, or the drag elsewhere
    QTimer m_releaseWait; // the button let go without a Drop or a DragLeave
    QTimer m_confirmTimer; // the dropped-on chip shown a moment
    int m_checks = 0; // the latest existence check: older answers are dropped

    piedrop::FileOperations* m_operations;
    Notify m_notify;
    ComWorker m_worker; // last: its tasks use the members above
};

} // namespace ws
