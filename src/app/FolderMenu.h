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
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class QWindow;

namespace ws {

class Favorites;
class History;
class KeyRouter;

// A double click on the blank space of a File Explorer folder or of the
// desktop brings up a menu of folders to go to, as Listary's does (GitHub
// #22): the favorite folders, the recent ones, those open in other windows;
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
    // {text, detail, glyph} or {separator: true}
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged FINAL)
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged FINAL)

public:
    FolderMenu(History* history, Favorites* favorites, QObject* parent = nullptr);
    ~FolderMenu() override;

    void setWindow(QWindow* window); // FolderMenuWindow
    void setRecordHistory(bool on);
    // auto | on (Settings::folderMenu); Explorer and the desktop each on or off.
    void setOptions(bool yieldToListary, bool explorer, bool desktop);
    // Which groups it lists (GitHub #51); one left out is not even gathered.
    void setGroups(bool favorites, bool recent, bool windows);
    // Whether something else has the keys now (the clipboard over File
    // Explorer): the menu does not come, the double click only closes that.
    void setBlocked(std::function<bool()> blocked) { m_blocked = std::move(blocked); }
    // Takes an Open or Save dialog to a folder, in order with the app's
    // other moves of it (DialogJump::go); false: not able to, the menu
    // does it itself.
    void setDialogGo(std::function<bool(HWND dialog, const QString& folder)> go) { m_dialogGo = std::move(go); }
    // The Open or Save dialog that `window`, one of Win顺's own beside it
    // (the search bar), stands in front of; null for any other window.
    void setDialogOf(std::function<HWND(HWND window)> dialogOf) { m_dialogOf = std::move(dialogOf); }

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

    Q_INVOKABLE QRectF screenArea(QPointF globalPos) const;
    Q_INVOKABLE void prepareMenuWindow(QWindow* menu) const;
    Q_INVOKABLE void trigger(int index);
    Q_INVOKABLE void dismiss();

signals:
    void entriesChanged();
    void popupRequested(QPointF pos); // logical pixels: the window's top-left corner, or where it flips from
    void hideRequested();
    void keyPressed(int key, const QString& text); // taken on its way to File Explorer
    void searchRequested(const QString& folder); // 在此搜索
    void openChanged(bool open);

private:
    enum class Kind { Folder, Search, AddFavorite, RemoveFavorite };
    struct Entry {
        Kind kind = Kind::Folder;
        QString path;
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
    QPointer<QWindow> m_window;
    std::function<bool()> m_blocked;
    std::function<bool(HWND, const QString&)> m_dialogGo;
    std::function<HWND(HWND)> m_dialogOf;
    menuhotkey::Toggle m_toggle; // GUI thread: what a hotkey press means now
    bool m_recordHistory = true;
    std::atomic<bool> m_yieldToListary {true};
    std::atomic<bool> m_explorer {true};
    std::atomic<bool> m_desktop {true};
    std::atomic<bool> m_listFavorites {true};
    std::atomic<bool> m_listRecent {true};
    std::atomic<bool> m_listWindows {true};
    std::atomic<bool> m_open {false};
    std::atomic<int> m_asks {0}; // the latest double click's number: older answers are dropped
    folderview::Hit m_hit; // where the menu is up
    QString m_folder; // the folder that tab shows; empty on the desktop, Home, This PC
    QVariantList m_entries;
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
