#pragma once

#include "MenuHotkey.h"
#include "platform/ComWorker.h"
#include "platform/FolderViewHit.h"

#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QVariantList>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ws {

class Favorites;
class AppCatalog;
class SystemCatalog;
class History;
class KeyRouter;

// A double click on the blank space of a File Explorer folder or of the
// desktop brings up a menu of favorites and folders, as Listary's does
// (GitHub #22, #156): all favorite kinds, recent and open folders, grouped
// into pages with names and icons;
// then "search here" and adding this folder to the favorites. Chosen in a
// File Explorer window, the same tab goes there; on the desktop, the file
// manager opens it.
//
// Directory Opus shows nothing of its rows to accessibility, so where a
// double click landed cannot be told from outside; Opus tells it itself,
// running WinShunSearch.exe --folder-menu "<folder>" for a double click on
// blank space once the user has set that up (requested()). Chosen there,
// Opus's tab goes to the folder.
//
// A hotkey brings the menu up too (GitHub #25: another program may have the
// double click), for whatever window is in front (hotkeyPressed()): a File
// Explorer folder, the desktop, a Directory Opus, Total Commander or
// XYplorer window, an Open or Save dialog, which a folder chosen takes
// there, a Pulse window (GitHub #41: handed to Pulse, which opens it where
// its own setting says, a new window maybe), or any other, from which the
// file manager opens it. Pressed again while the menu is up, it closes it.
// Pressed with the search bar beside a file dialog in front (a window of
// Win顺's own), the dialog is given the front back and the menu is for it.
//
// The menu never takes the focus: the window it is for (File Explorer,
// Total Commander, a dialog…) stays in front, its selection and caret as
// they were, and the keys the menu uses are taken on their way to it
// (KeyRouter, as the clipboard does over other programs); the rest, and a
// Chinese input method's own keys, go on to it. A click anywhere else
// closes it and goes on to where it was made; so does a double click there
// (it does not open the menu again). This holds over Total Commander and
// XYplorer with a Chinese input method on too: measured 2026-10-15 (an
// earlier "Down and Enter never arrive there" was the test script sending
// VK 0, see docs/pitfalls.md).
class FolderMenu : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    // {text, detail, glyph, icon, tooltip, action}, {children: [...]} or a separator.
    // A row with `folder` (a path) opens a page of the folders inside it.
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged FINAL)
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged FINAL)

public:
    FolderMenu(History* history, Favorites* favorites, QObject* parent = nullptr);
    ~FolderMenu() override;

    void setWindow(QWindow* window); // FolderMenuWindow
    void setRecordHistory(bool on);
    void setCatalogs(AppCatalog* apps, SystemCatalog* places) { m_apps = apps; m_places = places; }
    // auto | on (Settings::folderMenu); Explorer and the desktop each on or off.
    void setOptions(bool yieldToListary, bool explorer, bool desktop);
    // Which groups it lists (GitHub #51); one left out is not even gathered.
    void setGroups(bool favorites, bool recent, bool windows);
    // Which favorite kinds appear in the menu's 收藏夹.
    struct FavoriteKinds {
        bool folders = true;
        bool files = true;
        bool apps = true;
        bool places = true;
        bool commands = true;
        friend bool operator==(const FavoriteKinds&, const FavoriteKinds&) = default;
    };
    void setFavoriteKinds(FavoriteKinds kinds);
    // Whether the collapsible "应用" section in 收藏夹 is expanded.
    void setAppsExpanded(bool expanded);
    bool appsExpanded() const noexcept { return m_appsExpanded; }
    // -1: all items directly in the top column; 0: submenu; 1..20: first N directly, then More.
    static constexpr int kInlineAll = -1;
    static constexpr int kInlineMax = 20;
    void setInlineCounts(int favorites, int recent, int windows);
    // Pure menu presentation: keeps action IDs intact across the direct rows and More page.
    static QVariantList groupEntries(const QString& title, const QString& glyph, const QVariantList& children, int inlineCount = 0);
    // Pure menu presentation for favorites: places apps under a collapsible
    // section ("apps") when present and counts actual favorites for badges.
    static QVariantList favoriteGroupEntries(const QString& title, const QString& glyph,
        const QVariantList& primary, const QVariantList& apps, bool appsExpanded, int inlineCount = 0);
    // Combines rendered group lists, adding a separator wherever an inline group
    // meets another group while keeping collapsed-only groups adjacent.
    static QVariantList assembleGroups(const QList<QVariantList>& groups);
    // Which actions it offers under the lists (GitHub #200).
    void setItems(bool search, bool copyPath, bool cmd, bool powerShell, bool favorite, bool settings);
    // Whether a folder's column also lists its files (GitHub #384).
    void setListFiles(bool on) { m_listFiles = on; }
    // Whether something else has the keys now (the clipboard over File
    // Explorer): the menu does not come, the double click only closes that.
    // Where Copy path writes the text: the system clipboard unless a test says otherwise.
    void setClipboardWriter(std::function<void(const QString&)> writer) { m_clipboardWriter = std::move(writer); }
    // How a file chosen, or a folder chosen from a window that is no file
    // manager's, is opened: by the shell unless a test says otherwise.
    void setOpener(std::function<void(const QString&)> opener) { m_opener = std::move(opener); }
    void setBlocked(std::function<bool()> blocked) { m_blocked = std::move(blocked); }
    // Where the pointer is (cursorPos()): the system's unless a test says
    // otherwise (QTest moves Qt's pointer, not the system's).
    void setCursorSource(std::function<QPoint()> source) { m_cursorSource = std::move(source); }
    // Takes an Open or Save dialog to a folder, in order with the app's
    // other moves of it (DialogJump::go); false: not able to, the menu
    // does it itself.
    void setDialogGo(std::function<bool(HWND dialog, const QString& folder)> go) { m_dialogGo = std::move(go); }
    // The Open or Save dialog that `window`, one of Win顺's own beside it
    // (the search bar), stands in front of; null for any other window.
    void setDialogOf(std::function<HWND(HWND window)> dialogOf) { m_dialogOf = std::move(dialogOf); }
    // The folder a hosted view shows (360文件夹, folderview::Hit::hosted;
    // hostedexplorer::place): asked on the menu's worker; empty when not
    // known, or no folder on disk. None set: never known.
    void setHostedFolder(std::function<QString(HWND frame, HWND control)> folder) { m_hostedFolder = std::move(folder); }

    // From the listener thread (KeyListener): a double click there. Window
    // classes are looked at at once; what may be a folder's blank space is
    // asked about on a thread of its own, and the menu comes if it is.
    void doubleClicked(HWND window, POINT pt);
    // From Directory Opus (through WinShunSearch.exe): a double click on
    // blank space in a lister showing `folder`, the pointer still there.
    void requested(const QString& folder);
    // The hotkey (Settings::folderMenuHotkey) was pressed: the menu for the
    // window in front, under the pointer when that is over it; or, up
    // already, closed. On the GUI thread.
    void hotkeyPressed();
    bool isOpen() const noexcept { return m_open.load(); }

    QVariantList entries() const { return m_entries; }
    // The off-screen preview (Preview.cpp, --style-windows): these entries,
    // as a popup lists them; nothing to act on, nothing shown.
    void previewEntries(QVariantList entries)
    {
        m_hasFavorites = false;
        m_favoritePrimary.clear();
        m_favoriteApps.clear();
        m_afterFavoriteGroups.clear();
        m_folderActionEntries.clear();
        m_settingsEntries.clear();
        m_entries = std::move(entries);
        emit entriesChanged();
    }
    // Builds the menu's real entries and actions from the current favorites,
    // groups and options without querying the foreground window (for tests and
    // previews).
    void previewMenu(const QString& folder = {}, const QStringList& recent = {}, const QStringList& windows = {});

    Q_INVOKABLE QRectF screenArea(QPointF globalPos) const;
    // Where the system's pointer is: unlike a row's mapToGlobal, the same
    // while the pointer rests, though pages move the window (GitHub #348).
    Q_INVOKABLE QPoint cursorPos() const;
    Q_INVOKABLE void prepareMenuWindow(QWindow* menu) const;
    // A column of the menu (FolderMenuWindow, or a folder's column beside
    // it) shown as the menu is (prepareMenuWindow), in front of those shown
    // before it, and only with its first frame: Windows would first put it
    // up as it last looked (another folder's rows) and fade it in. Hidden by
    // hideColumn at once, without the fade.
    Q_INVOKABLE void showColumn(QWindow* column);
    Q_INVOKABLE void hideColumn(QWindow* column) const;
    // Puts a column at `at` (Qt's pixels, inside the work area `area`) on
    // the screen's own pixels, which Qt's whole ones do not all reach at
    // 150 %: beside `before` with the border they share one line (side 1:
    // on its right, -1: on its left, 0: at `at`'s x), and with `level` its
    // top `rowOffset` (Qt's pixels) below the top of `before`, its first row
    // level with the row it came from (GitHub #348).
    Q_INVOKABLE void placeColumn(QWindow* column, QWindow* before, QPointF at, int side, qreal rowOffset, bool level,
        QRectF area) const;
    Q_INVOKABLE void trigger(int index);
    // Toggles a collapsible section (e.g. "apps") in place without dismissing
    // the menu, keeping action IDs intact and notifying to persist the choice.
    Q_INVOKABLE void toggleSection(const QString& id);
    // The folders inside `folder`, for the page a folder's row opens (GitHub
    // #170), then its files with setListFiles (#384; not for a dialog, whose
    // menu only takes it to a folder): read off the GUI thread (a share may
    // take its time), answered by folderListed() with rows as the menu's
    // own, each to be opened in turn. A row for an empty folder says so when
    // files are listed; the menu says "no folders" itself for no rows. Nothing
    // comes for a menu closed meanwhile.
    Q_INVOKABLE void listFolder(const QString& folder);
    // What is right inside `folder`, each by name as File Explorer orders them
    // (2 before 10), hidden ones left out: its folders, and its files when
    // asked; at most `max` of each, the first ones in that order.
    struct Listing {
        QStringList folders;
        QStringList files;
        qsizetype moreFiles = 0; // left out past `max`
    };
    static Listing listing(const QString& folder, bool files, qsizetype max = 500);
    static QStringList subfolders(const QString& folder, qsizetype max = 500) { return listing(folder, false, max).folders; }
    Q_INVOKABLE void dismiss();

signals:
    void entriesChanged();
    void folderListed(const QString& folder, const QVariantList& entries);
    void popupRequested(QPointF pos); // logical pixels: the window's top-left corner, or where it flips from
    void hideRequested();
    void keyPressed(int key, const QString& text); // taken on its way to File Explorer
    void searchRequested(const QString& folder); // 在此搜索
    void settingsRequested();
    void pluginSettingsRequested(const QString& pluginId); // a favorite that is a plugin's settings (GitHub #390)
    void notice(const QString& text);
    void openChanged(bool open);
    void appsExpandedRequested(bool expanded);

private:
    void populate(const QString& folder, const QStringList& recent, const QStringList& windows);
    void rebuildEntries();
    QString explorerFolder(const folderview::Hit& hit) const;
    enum class Kind { Folder, File, Search, AddFavorite, RemoveFavorite, Terminal, PowerShell, Favorite, Settings, CopyPath };
    struct Entry {
        Kind kind = Kind::Folder;
        QString path;
        QString title;
    };
    struct Found; // what the worker gathered

    void gatherHere(const folderview::Hit& hit, int ask, const QString& folder, bool recordHistory,
        const QStringList& history);
    void show(const folderview::Hit& hit, const Found& found);
    void startKeys(HWND target);
    void stopKeys();
    void goTo(const QString& folder);
    POINT hotkeyAnchor(HWND root) const; // where the menu goes for the hotkey (physical pixels)
    // The double click's way, for the diagnostics journal (GitHub #45). On
    // the worker; `chain`: the window classes, when there is no hit.
    void journal(const QString& step, const folderview::Hit& hit, const std::wstring& chain, folderview::BlankTrace* trace,
        const QJsonObject& more = {});

    History* m_history;
    Favorites* m_favorites;
    AppCatalog* m_apps = nullptr;
    SystemCatalog* m_places = nullptr;
    QPointer<QWindow> m_window;
    std::function<bool()> m_blocked;
    std::function<QPoint()> m_cursorSource;
    std::uint64_t m_columnShows = 0; // which showing of a column its first frame reveals
    std::function<void(const QString&)> m_clipboardWriter;
    std::function<void(const QString&)> m_opener;
    std::function<bool(HWND, const QString&)> m_dialogGo;
    std::function<HWND(HWND)> m_dialogOf;
    std::function<QString(HWND, HWND)> m_hostedFolder;
    menuhotkey::Toggle m_toggle; // GUI thread: what a hotkey press means now
    bool m_recordHistory = true;
    std::atomic<bool> m_yieldToListary {true};
    std::atomic<bool> m_explorer {true};
    std::atomic<bool> m_desktop {true};
    std::atomic<bool> m_listFavorites {true};
    std::atomic<bool> m_listRecent {true};
    std::atomic<bool> m_listWindows {true};
    FavoriteKinds m_favoriteKinds;
    bool m_appsExpanded = true;
    int m_favoritesInline = 0;
    int m_recentInline = 0;
    int m_windowsInline = 0;
    std::atomic<bool> m_itemSearch {true};
    std::atomic<bool> m_itemCopyPath {true};
    std::atomic<bool> m_itemCmd {true};
    std::atomic<bool> m_itemPowerShell {true};
    std::atomic<bool> m_itemFavorite {true};
    std::atomic<bool> m_itemSettings {true};
    bool m_listFiles = false; // GUI thread
    std::atomic<bool> m_open {false};
    std::atomic<int> m_asks {0}; // the latest double click's number: older answers are dropped
    int m_shown = 0; // which showing the entries are of: a folder listed for an older one is dropped
    folderview::Hit m_hit; // where the menu is up
    QString m_folder; // the tab's folder or Desktop directory; empty in Home / This PC
    QVariantList m_entries;
    // Snapshot of the shown menu's sections so toggleSection() rebuilds
    // m_entries in place without invalidating m_actions or cached folder pages.
    bool m_hasFavorites = false;
    QVariantList m_favoritePrimary;
    QVariantList m_favoriteApps;
    QList<QVariantList> m_afterFavoriteGroups;
    QVariantList m_folderActionEntries;
    QVariantList m_settingsEntries;
    std::vector<Entry> m_actions; // by entry
    std::unique_ptr<KeyRouter> m_router;
    std::uint64_t m_routerRun = 0; // queued input belongs to this opening
    std::unique_ptr<KeyRouter> m_draining; // done, until the keys it took are let go
    std::uint64_t m_drainingRun = 0;
    // The recent folders as last gathered (worker thread only): reading
    // Windows' list takes a good part of a second the first time, so the
    // menu comes with these, and they are gathered again after it.
    QStringList m_recent;
    bool m_recentKnown = false;
    QSet<QString> m_journaled; // the double clicks' stories told this run (worker thread only)
    ComWorker m_worker; // last: its tasks use the members above
};

} // namespace ws
