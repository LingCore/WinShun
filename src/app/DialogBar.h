#pragma once

#include "History.h"
#include "PathText.h"
#include "ResultModel.h"
#include "SearchTypes.h"
#include "platform/ComWorker.h"
#include "platform/FileDialog.h"

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>

namespace ws {

class SearchEngine;

// The search bar under an Open or Save dialog of another program, as in
// Listary: the folder shown in File Explorer, to go to with a click (or
// Ctrl+G), and a box to search folders and files by name; picking one takes
// the dialog there (a file: to its folder, its name in the file name box,
// so Enter opens it; Ctrl+Enter in an Open dialog opens it at once). A path
// typed (or pasted) lists what that folder holds, as a shell completes it:
// Tab goes into the folder of a row, Shift+Tab up one. With nothing typed,
// the list offers the folders open in Explorer, a path on the clipboard (a
// file copied in Explorer too), the folders pinned here, and the ones used
// lately (what Windows lists as recent, then what was opened through Win顺).
//
// The view-model of DialogBarWindow.qml, and its window's placement: shown under
// the dialog while that is in front (DialogJump tells), without taking the
// focus from it; it follows the dialog around and hides with it. The list
// shows while the bar has the focus, below it, or above where there is more
// room.
class DialogBar : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged FINAL)
    Q_PROPERTY(ws::ResultModel* results READ results CONSTANT FINAL)
    // Where each row comes from ("资源管理器", "固定"…); empty for plain matches.
    Q_PROPERTY(QStringList tags READ tags NOTIFY resultsReplaced FINAL)
    // The folder Ctrl+G goes to (its name); empty when Explorer shows none.
    Q_PROPERTY(QString explorerName READ explorerName NOTIFY explorerChanged FINAL)
    Q_PROPERTY(QString explorerPath READ explorerPath NOTIFY explorerChanged FINAL)
    // The folder the dialog showed before it went to Explorer's by itself
    // (its name), to go back to; empty when it did not.
    Q_PROPERTY(QString originName READ originName NOTIFY originChanged FINAL)
    Q_PROPERTY(bool foldersOnly READ foldersOnly NOTIFY dialogKindChanged FINAL) // a folder picker
    Q_PROPERTY(bool canOpen READ canOpen NOTIFY dialogKindChanged FINAL) // an Open dialog: Ctrl+Enter
    Q_PROPERTY(bool nothingFound READ nothingFound NOTIFY resultsReplaced FINAL)
    // The folder whose contents are listed for a path typed; empty while searching.
    Q_PROPERTY(QString browsedFolder READ browsedFolder NOTIFY browsedFolderChanged FINAL)
    Q_PROPERTY(int rows READ rows NOTIFY layoutChanged FINAL) // of the list; 0 while it is closed
    Q_PROPERTY(bool listAbove READ listAbove NOTIFY layoutChanged FINAL)
    Q_PROPERTY(int barHeight READ barHeight CONSTANT FINAL)
    Q_PROPERTY(int rowHeight READ rowHeight CONSTANT FINAL)

public:
    static constexpr int kBarHeight = 44; // logical pixels
    static constexpr int kRowHeight = 52;
    static constexpr int kListChrome = 9; // the divider and the list's margins
    static constexpr int kMaxRows = 8;

    // The context menus' entries.
    enum Action {
        Go, // the row: there
        GoAndOpen, // a file in an Open dialog: there, and open it
        Enter, // a folder: its path in the box, to list what it holds (Tab)
        CopyPath,
        Pin, // a folder (a file's folder) first in the list with nothing typed
        Unpin,
        PinCurrent, // the bar's menu: the folder the dialog shows
        UnpinCurrent,
        HideHere, // not under this dialog again
        HideInApp, // not under this program's dialogs (Settings)
        OpenSettings,
    };
    Q_ENUM(Action)

    // Takes a dialog to a folder or a file (DialogJump::go).
    using Mover = std::function<void(HWND dialog, std::wstring path, bool isFile, bool open)>;

    // `pinsFile`: where the pinned folders are kept.
    DialogBar(SearchEngine* engine, History* history, const QString& pinsFile, Mover mover, QObject* parent = nullptr);
    ~DialogBar() override;

    void setWindow(QQuickWindow* window); // DialogBarWindow.qml, created by the app
    QQuickWindow* window() const { return m_window; }
    // Off: what was opened lately is neither offered nor ranks first.
    void setRecordHistory(bool on) { m_recordHistory = on; }
    void setExcludedApps(const QStringList& apps); // program files ("notepad.exe") whose dialogs go without

    void setDialog(HWND dialog); // in front, or nullptr (DialogJump::dialogChanged)
    void dialogMoved();
    // The dialog went to Explorer's folder by itself, from `from` (DialogJump::autoJumped).
    void setOrigin(HWND dialog, const QString& from);
    bool isShown() const; // under a dialog now
    // Double Ctrl with the dialog in front: into the box; from the box, back
    // to the dialog.
    void toggleFocus();

    QString query() const { return m_query; }
    void setQuery(const QString& query);
    ResultModel* results() { return &m_results; }
    QStringList tags() const { return m_tags; }
    QString explorerName() const;
    QString explorerPath() const { return m_explorer.isEmpty() ? QString() : m_explorer.constFirst(); }
    QString originName() const;
    bool foldersOnly() const { return m_kind == filedialog::Kind::Folder; }
    bool canOpen() const { return m_kind == filedialog::Kind::Open; } // not the XP style: it may be for saving
    bool nothingFound() const { return m_nothingFound; }
    QString browsedFolder() const { return m_browsed; }
    int rows() const { return m_rows; }
    bool listAbove() const { return m_listAbove; }
    int barHeight() const { return kBarHeight; }
    int rowHeight() const { return kRowHeight; }

    Q_INVOKABLE void choose(int row, bool open = false);
    Q_INVOKABLE void chooseExplorer(); // the folder shown in Explorer (as Ctrl+G)
    Q_INVOKABLE void goBack(); // to the folder the dialog showed before it went by itself
    Q_INVOKABLE void back(); // Esc with nothing typed: the dialog has the focus again
    Q_INVOKABLE void enter(int row); // Tab: the row's path in the box ("D:\Projects\"), listing what it holds
    Q_INVOKABLE void up(); // Shift+Tab: the typed path's folder, then the one above

    // The context menu of a row, or of the bar itself (row -1); see ContextMenu.qml.
    Q_INVOKABLE QVariantList menuItems(int row) const;
    Q_INVOKABLE void trigger(int row, int action);
    Q_INVOKABLE QRectF screenArea(QPointF globalPos) const;
    Q_INVOKABLE void prepareMenuWindow(QWindow* menu) const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void queryChanged();
    void explorerChanged();
    void originChanged();
    void dialogKindChanged();
    void browsedFolderChanged();
    void resultsReplaced(); // select the first row
    void layoutChanged();
    void excludeAppRequested(const QString& app); // HideInApp
    void settingsRequested();
    void contextMenuKeyPressed(); // Menu key / Shift+F10 (arrive as a context-menu event, not a key)

private:
    void showWindow();
    void hideWindow();
    void follow(); // shown under the dialog, or hidden while it is not to be seen
    void place();
    void relist(); // for the query: suggestions, a folder's contents or the search's results
    void search();
    void browse(const pathtext::TypedPath& typed); // in the background
    void showSuggestions(bool refresh = false); // nothing typed; `refresh`: the list was already up
    // The rows to show and the tag of each; `refresh`: keep the selection if nothing changed.
    void setRows(SearchResults rows, QStringList tags, const QStringList& highlights, bool refresh = false);
    void lookAround(); // Explorer's folders, the recent ones and the rest, in the background
    void onResults(quint64 id, const SearchResults& results);
    bool ofFileType(const SearchResult& r) const; // one the Open dialog shows
    void goTo(QString path, bool isFile, bool open = false); // a copy: the rows change on the way
    bool hidden() const; // not under this dialog: HideHere, HideInApp
    QString pinTarget(int row) const; // the folder Pin and Unpin are about
    void setPinned(const QString& folder, bool pinned);

    SearchEngine* m_engine;
    History* m_history;
    History m_pins; // newest first
    Mover m_mover;
    ResultModel m_results;
    QStringList m_tags;
    QPointer<QQuickWindow> m_window;
    HWND m_dialog = nullptr;
    QString m_dialogApp; // its program file, "notepad.exe"
    filedialog::Kind m_kind = filedialog::Kind::None;
    bool m_inFront = false; // m_dialog (or the bar) is in front
    bool m_recordHistory = true;
    QStringList m_excludedApps;
    HWND m_hiddenUnder = nullptr; // HideHere
    HWND m_originDialog = nullptr;
    QString m_origin; // see originName

    QString m_query;
    quint64 m_requestId = 0;
    QString m_browsed; // see browsedFolder
    std::atomic<int> m_listings {0}; // the newest browse() wins
    bool m_nothingFound = false;
    QStringList m_explorer; // open in Explorer, front first
    QStringList m_recent; // folders used lately, newest first
    QStringList m_pinned; // the pins still there
    QString m_clipboard; // a file or folder whose path was copied
    bool m_clipboardIsDir = false;
    QStringList m_extensions; // the Open dialog's file type: these first
    std::atomic<int> m_lookups {0}; // the newest lookAround() wins

    int m_rows = 0;
    bool m_listAbove = false;
    bool m_listHeld = false; // just made active: no list until a click is through
    bool m_clickSeen = false;
    QTimer m_listHold; // no click came
    std::atomic<bool> m_uncloakPending {false}; // shown cloaked until its first frame
    QTimer m_uncloakTimeout;
    // Last: done with their tasks before the members above go. Two: a folder
    // that is slow to list (on a network) holds up no look around.
    ComWorker m_worker;
    ComWorker m_lister;
};

} // namespace ws
