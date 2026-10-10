#pragma once

#include "Settings.h"
#include "Win32Util.h"
#include "platform/ClipboardWatcher.h"
#include "platform/KeyRouter.h"
#include "platform/TaskbarSearch.h"
#include "platform/TaskbarSearchBox.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QTranslator>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;
class QWinEventNotifier;

namespace ws {

class AppCatalog;
class ClipStore;
class Clipboard;
class DialogBar;
class DialogJump;
class SystemCatalog;
class History;
class IndexService;
class KeyListener;
class Launcher;
class MessageWindow;
class NumberKeys;
class Placement;
class SearchEngine;
class SettingsEditor;
class StartMenuTyping;
class Updater;
class VolumeNotifier;
class WindowFrame;
class WindowLogo;
namespace win {
class TextCaret;
}

// Wires the pieces together and owns their lifetimes: settings, index,
// search engine, the QML window, tray icon and global hotkeys.
class App : public QObject {
    Q_OBJECT

public:
    struct StartOptions {
        bool background = false; // start in the tray without showing the window
        QString query;
        bool settings = false; // open the settings window
    };

    App();
    ~App() override;

    bool start(const StartOptions& options);
    void handleCommand(const QString& command); // from a second instance

    void toggleLauncher();
    void toggleClipboard(); // the clipboard window (Win+V)
    // With `spot`, over the taskbar, as Windows' own search (taskbar::);
    // `inBox`: over Win顺's box there, its field drawn in the box.
    void showLauncher(const QString& query = {}, const std::optional<taskbar::Spot>& spot = {}, bool inBox = false);
    void hideLauncher();
    void showClipboard();
    void showSettings();
    void showUpdate(); // the settings window with the update dialog
    // Set by "重新启动" in the tray menu or the settings window: the command
    // line for the new copy, which main() starts once this one has let go.
    std::optional<QStringList> restartArguments() const { return m_restartArguments; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool createWindow();
    bool createClipWindow();
    void onDoubleCtrl(); // toggleLauncher(), unless a game is played (GameGuard)
    // `launcherToo`: and the launcher, when the clipboard was under its
    // search box and the keyboard went elsewhere.
    void hideClipboard(bool launcherToo = true);
    bool clipboardUnderLauncher() const; // opened from the launcher's search box
    bool clipboardInCharge() const; // shown, the keys its own or through KeyRouter
    // Over another program the clipboard leaves it the focus, its keys come
    // through KeyRouter: from its showing until it goes or takes the focus
    // after all (activateClipboard).
    bool startKeyRouting(HWND target);
    void stopKeyRouting();
    void routedKey(const KeyRouter::Key& key);
    void routedForeground(HWND window);
    // Taking the focus after all: to type a group's name (with the input
    // method), to hand over to another program.
    void activateClipboard();
    void uncloakClipboard(); // its first frame is up
    void prewarmClipboard();
    void finishClipPrewarm();
    QQuickItem* focusedField() const; // a text field of Win顺's own with the keyboard
    void returnToField(QQuickItem* field); // from the clipboard (Clipboard::fieldRequested)
    void updateCompanions(); // our windows that go with a file dialog (DialogJump)
    // Ctrl+1… on the launcher and the dialog bar: NumberKeys while one of
    // them is in front, with the modifier the settings name.
    void updateNumberKeys();
    void numberKeyPressed(HWND window, int number);
    void applySettings(bool initial);
    ClipboardWatcher::Options clipboardOptions() const;
    void applyClipboard(); // the [Clipboard] settings
    void applyClipboardHotkeys(); // Win+V and the other shortcut; where Win+V stands, for the settings
    void clipCaptured(const ClipCapture& capture);
    void applyTaskbar(); // the [Taskbar] settings: Win+S, typing in the Start menu
    void applyTaskbarHotkeys(); // Win+S and Win+Shift+S; where Win+S stands, for the settings
    // The taskbar button was clicked (`clicked`), or Win+S pressed: the
    // launcher over the taskbar, or away again.
    void toggleAtTaskbar(bool clicked);
    void startScreenClip(); // Win+Shift+S, which Explorer gives up with Win+S
    void startMenuTyped(); // typed in the Start menu (StartMenuTyping): the launcher over the taskbar
    // Win顺's own search box on the taskbar ([Taskbar] SearchBox, taskbar::SearchBox).
    void applySearchBox();
    void setSearchBoxShown(bool shown); // it is on the taskbar, or not (no room)
    void updateSearchBoxLook(); // placeholder, accent
    void searchBoxPressed(const taskbar::SearchBox::Press& press);
    void showLauncherInBox(); // the launcher over the box (or, without one shown, over the taskbar)
    // What the launcher's field has, drawn in the box while it is open over it.
    void mirrorField(const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    std::optional<QRectF> boxCaretInField() const; // the box's caret, in the field's coordinates
    void deliverTyping(); // ... has the keyboard: the keys typed there go to it
    void restartExplorer(); // so that Win+V and Win+S change hands now
    void explorerRestarted(bool back); // `back`: the taskbar is
    void setClipboardPaused(bool paused);
    void showClipboardSettings(); // the settings window on its 剪贴板 page
    void showWebSettings(); // ... on its 网页搜索 page (a web row's menu)
    void applyHotkey();
    void applyDialogs(); // Ctrl+G, the bar and going by themselves in file dialogs
    void excludeFromDialogBar(const QString& app); // "not in this program" from the bar's menu
    bool createBarWindow();
    void reloadSettings();
    void settingsEdited(const Settings& settings);
    void showTrayMenu();
    void restart(const QStringList& arguments);
    void applyTheme();
    void applyAppearance(); // the chosen theme and language
    QString trayTooltip() const;
    void armReveal();
    void revealLauncher(); // slides in (Placement::slideIn) unless it went away meanwhile
    void finishHidingLauncher(); // slid out (Placement::slideOut), or at once
    bool launcherShown() const; // up, and not on its way out
    void prewarmLauncher();
    void finishPrewarm();
    void refreshContentIndexStatus(); // shown in the settings window
    // 索引位置: the index files go where [Index] Folder says, moved in the
    // background; when that fails, the setting goes back to where they are.
    QString startIndexFolder(); // where the index starts out
    void applyIndexFolder();
    QString indexFolderProblem(const QString& folder) const; // why the index cannot go there; empty if it can
    void indexMoved(int error);
    void indexMoveFailed(const QString& attempted, const QString& problem); // `attempted`: the setting
    void refreshIndexFolder(bool measure = false); // shown in the settings window; `measure`: its size too

    Settings m_settings;
    std::unique_ptr<History> m_history;
    std::unique_ptr<IndexService> m_index;
    QString m_indexDir; // where the index files are, '/' separated
    QString m_indexFolder; // the [Index] Folder setting m_indexDir stands for
    QString m_indexMoveFolder; // ... and the one being moved to
    QString m_indexMoveDir;
    bool m_indexMoveMadeDir = false; // the move made that folder: it goes again if the move fails
    int m_indexMoveProgress = 0;
    QString m_indexFolderProblem; // shown in the settings window until the next move
    std::uint64_t m_indexBytes = 0; // of the files in m_indexDir, as last measured
    bool m_indexOnHardDisk = false; // likewise
    std::unique_ptr<AppCatalog> m_apps; // before the engine, which reads it
    std::unique_ptr<SystemCatalog> m_places; // likewise
    std::unique_ptr<SearchEngine> m_engine;
    // The dialog bar's own: an engine runs only its newest request, so one
    // shared with the launcher would drop the launcher's (and its rows would
    // wait for results that never come).
    std::unique_ptr<SearchEngine> m_dialogEngine;
    std::unique_ptr<Launcher> m_launcher;
    std::unique_ptr<ClipStore> m_clipStore; // the clipboard history
    std::unique_ptr<Clipboard> m_clipboard; // its window's view-model
    // Destroyed before the two above: its thread calls back into the app, and
    // the page waits on its writes.
    std::unique_ptr<ClipboardWatcher> m_clipWatcher;
    bool m_clipboardPaused = false; // from the tray menu, until Win顺 restarts
    bool m_clipboardNotified = false; // the last tray notification told of the clipboard history
    std::optional<bool> m_winVApplied; // the Win+V switch, as last put into the registry
    bool m_winVRegistryFailed = false;
    std::optional<bool> m_winSApplied; // the Win+S switch, likewise
    bool m_winSRegistryFailed = false;
    bool m_restartingExplorer = false;
    win32::UniqueHandle m_taskbarEvent; // set by the taskbar button (WinShunSearch.exe)
    std::unique_ptr<QWinEventNotifier> m_taskbarNotifier; // waits on it; goes first
    QElapsedTimer m_launcherHidden; // since the launcher last went away
    QElapsedTimer m_launcherShown; // since it was last asked for
    bool m_launcherClosing = false; // sliding out (hideLauncher)
    std::unique_ptr<StartMenuTyping> m_startTyping; // while [Taskbar] StartMenuTyping is on
    std::unique_ptr<taskbar::SearchBox> m_searchBox; // while [Taskbar] SearchBox is on (Windows 11)
    bool m_searchBoxShown = false; // ... and it is on the taskbar
    // A press on the box takes the keyboard from the launcher over it (Windows
    // does that for whatever is clicked on the taskbar): it comes back with
    // the press (searchBoxPressed), else it goes once this runs out.
    QTimer m_boxPressGrace;
    QPointer<QQuickItem> m_searchField; // the launcher's (SearchBar.qml): the input method asks it where the caret is
    taskbar::SearchBox::Text m_fieldShown; // what it last reported (mirrorField), for the box
    bool m_deliverTyping = false; // the launcher was asked for by typing in the Start menu
    QString m_clipboardShortcut; // what opens the clipboard: "Win+V", another shortcut, or nothing
    std::unique_ptr<MessageWindow> m_messages;
    std::unique_ptr<Updater> m_updater;
    bool m_updateNotified = false; // the last tray notification announced a new version
    std::unique_ptr<VolumeNotifier> m_volumeNotifier; // after m_messages, whose window it uses
    std::unique_ptr<KeyListener> m_keyListener; // double Ctrl
    QStringList m_doubleCtrlIgnoredLogged; // "program reason", logged once each: a game gets many
    std::unique_ptr<DialogJump> m_dialogJump; // file dialogs: Ctrl+G, the bar; after m_messages, which has the hotkey
    std::unique_ptr<DialogBar> m_dialogBar; // the search bar under file dialogs
    QDeadlineTimer m_barFocusDeadline; // gone back to from the clipboard: until then, it takes the keyboard as it shows
    std::unique_ptr<Placement> m_placement; // where the launcher opens; moving it
    std::unique_ptr<WindowFrame> m_frame; // the launcher's header and footer drag it
    std::unique_ptr<Placement> m_clipPlacement; // likewise for the clipboard window
    std::unique_ptr<WindowFrame> m_clipFrame;
    std::unique_ptr<win::TextCaret> m_textCaret; // where another program's caret is: the clipboard opens there
    std::unique_ptr<KeyRouter> m_keyRouter; // while the clipboard is over a program that keeps the focus
    HWND m_keyRouterTarget = nullptr; // that program's window
    std::uint64_t m_keyRouterRun = 0; // which routing its posted keys belong to
    std::unique_ptr<KeyRouter> m_drainingRouter; // done, until the keys it took are let go (KeyRouter::drain)
    std::unique_ptr<NumberKeys> m_numberKeys; // see updateNumberKeys()
    std::vector<HWND> m_numberWindows; // the windows it takes the keys for
    bool m_numberKeysAlt = false; // ... with Alt, else with Ctrl
    std::uint64_t m_drainingRun = 0;
    std::atomic<bool> m_clipUncloak {false}; // shown cloaked until its first frame
    QTimer m_clipUncloakTimeout;
    std::atomic<bool> m_clipPrewarming {false}; // shown cloaked for one frame (see prewarmClipboard)
    // The launcher is shown cloaked and revealed once it has drawn a frame
    // with fresh results (see showLauncher). Set on the GUI thread, read on
    // the render thread.
    bool m_revealing = false;
    std::atomic<bool> m_revealArmed {false}; // results are in: the next synchronised frame is the one
    std::atomic<bool> m_revealSynced {false}; // that frame has been synchronised
    QTimer m_revealTimeout;
    std::atomic<bool> m_prewarming {false}; // shown cloaked for one frame (see prewarmLauncher)
    std::unique_ptr<QQmlApplicationEngine> m_qml; // destroyed first: QML references the objects above
    QPointer<QQuickWindow> m_window;
    WindowLogo* m_launcherLogo = nullptr; // m_window's
    QPointer<QQuickWindow> m_clipWindow; // the clipboard's; created a moment after the start
    WindowLogo* m_clipLogo = nullptr; // m_clipWindow's
    QPointer<QQuickWindow> m_settingsWindow; // created on demand, deleted when closed
    QPointer<QQuickWindow> m_barWindow; // the dialog bar's; created with the first file dialog
    QPointer<SettingsEditor> m_settingsEditor; // owned by m_settingsWindow
    QString m_renderer; // the one in use; changing it takes a restart
    QString m_language; // resolved: zh or en
    QTranslator m_translator; // English; the source strings are Chinese
    std::optional<QStringList> m_restartArguments;

    QFileSystemWatcher m_settingsWatcher;
    QTimer m_settingsReload;
    QTimer m_indexOptionsApply; // rebuilding the crawler is costly: batch quick edits
    QTimer m_contentStatusTimer; // while the settings window is open
    QTimer m_darkFrameGuard; // see applyTheme
    QElapsedTimer m_darkFrameWatch;
    bool m_darkFrame = false;
};

} // namespace ws
