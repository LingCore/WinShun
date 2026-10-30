#pragma once

#include "AiSettings.h"
#include "FrameReveal.h"
#include "Settings.h"
#include "TriggerJournal.h"
#include "Win32Util.h"
#include "platform/ClipboardWatcher.h"
#include "platform/HoldWatch.h"
#include "platform/KeyListener.h"
#include "platform/KeyRouter.h"
#include "platform/TaskbarSearch.h"
#include "platform/TaskbarSearchBox.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QTranslator>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class QImage;
class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;
class QScreen;
class QWinEventNotifier;

namespace ws {

namespace piedrop {
class FileOperations;
}

class AppCatalog;
class ClipImportJob;
class ClipStore;
class Clipboard;
class ComWorker;
class DialogBar;
class DialogJump;
class Favorites;
class FilePreview;
class DropPie;
class FolderMenu;
class SystemCatalog;
class History;
class IndexService;
class IdleTrim;
class KeyListener;
class PointerWatch;
class ServiceDialogNav;
class ServiceKeyListener;
class TriggerJournal;
enum class TriggerSource : std::uint8_t;
namespace win {
enum class Front : std::uint8_t;
}
class Launcher;
class MessageWindow;
class MissingFiles;
class NetworkNotifier;
class Placement;
class SearchEngine;
class SettingsEditor;
class Switches;
class TypeToSearch;
class LauncherKeysWatch;
class Updater;
class Backup;
namespace capture {
class Capture;
class CaptureEditor;
class CaptureOcr;
struct SmartHooks;
class PinManager;
class PinWindow;
namespace recording { class ClickStream; }
}
class VolumeNotifier;
class WindowFrame;
class WindowLogo;
namespace win {
class TextCaret;
}
namespace mcp {
class McpServer;
class ToolHost;
struct Policy;
struct Services;
struct ScreenshotRequest;
struct ScreenshotTaken;
enum class SettingsAnswer : int;
struct ToolResult;
struct SettingsQuery;
struct SettingsPlan;
struct SettingsApply;
struct SettingsAsk;
}
namespace ntfs {
class ServiceVolumeAccess;
enum class ServiceState;
}
namespace plugin {
class AiBridge;
class Host;
class Tools;
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
        QString settingsAt; // there (SettingsWindow.showPlace), after a restart from the settings
    };

    App();
    ~App() override;

    bool start(const StartOptions& options);
    void handleCommand(const QString& command); // from a second instance

    // byShortcut: the custom key or the double tap (ShortcutAgain);
    // pressedAt: when, on the 64-bit tick clock (now if not known): a press
    // made before the window was there to be seen does not close it (seenBy,
    // GitHub #111).
    void toggleLauncher(bool byShortcut = false, std::optional<std::uint64_t> pressedAt = std::nullopt);
    void toggleClipboard(std::optional<std::uint64_t> pressedAt = std::nullopt); // the clipboard window (Win+V)
    // With `spot`, over the taskbar, as Windows' own search (taskbar::);
    // `inBox`: over Win顺's box there, its field drawn in the box when the
    // box is wide enough for it.
    // `on`: the screen to open on (typed in an Explorer window there), else
    // the one under the mouse.
    void showLauncher(const QString& query = {}, const std::optional<taskbar::Spot>& spot = {}, bool inBox = false,
        QScreen* on = nullptr);
    void hideLauncher();
    // The launcher as the search panel (Launcher::panel): the window sized
    // for it (Placement::enterPanel) and resizable, or back as the box at
    // its spot (or over the taskbar it came from). Opening anew is always
    // the box: the panel is a mode its user asks for. Hidden as the panel,
    // it goes back quietly.
    void setPanel(bool on);
    void togglePanel();
    // Launcher::pinned changed. Pinned over Win顺's box on the taskbar, the
    // field comes back into the window and the box goes idle (as for the
    // panel): the window stays where it stands, a window of its own now.
    void launcherPinnedChanged();
    // --dump <file> from another copy (tests): the launcher's state and rows as JSON.
    void dumpLauncher(const QString& file) const;
    void showClipboard();
    void showSettings();
    void showUpdate(); // the settings window with the update dialog
    // Set by "重新启动" in the tray menu or the settings window: the command
    // line for the new copy, which main() starts once this one has let go,
    // and what it runs with: as this one, as administrator (UAC asks once:
    // the 管理员直读 mode chosen, GitHub #21), or with the user's normal
    // rights (that mode left while running elevated).
    enum class Elevation { Same, Elevated, Unelevated };
    std::optional<QStringList> restartArguments() const { return m_restartArguments; }
    Elevation restartElevation() const { return m_restartElevation; }

    // Called from SettingsNotice.qml, so public: QML leaves out private
    // Q_INVOKABLE methods (a connect by index, as for the cards' answers,
    // does not).
    // The notice's 撤销: puts back what the batch changed, where it is still
    // what AI set; {ok, text} (text: what to tell the user).
    Q_INVOKABLE QVariantMap undoSettingsBatch(quint64 batchId);
    // Its 打开设置: the settings window at that setting (a key).
    Q_INVOKABLE void showSettingsFor(const QString& key);
    Q_INVOKABLE void restartExplorer(); // so that Win+V and Win+S change hands now

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool createWindow();
    bool createClipWindow();
    QQuickWindow* createPreviewWindow(); // FilePreview's, the first time it is wanted
    // 识别文字 from a capture or a pin: the OCR window (OcrPanel.qml), made
    // the first time, beside `where` (physical), reading `image`.
    // `texts`: show these (QR codes, none found when empty) instead of reading `image`.
    void showCaptureOcr(const QImage& image, double dpr, const QRect& where, const QStringList* texts = nullptr);
    void showQrResult(const QStringList& codes, const QRect& where);
    void closeCaptureOcr(bool destroy = false); // `destroy`: the capture was turned off
    // The OCR window's reader, made the first time it is wanted: the window's
    // text and smart redaction's scans go through the same thread and model.
    capture::CaptureOcr& captureOcr();
    // How the capture and the editing window ask for smart redaction's scans,
    // its rules and whether it scans by itself ([Capture] Smart...).
    capture::SmartHooks smartHooks();
    // 识别文字并复制 / 搜索 / 网页搜索: read `image` on a reader of its own, no
    // window (the OCR window is up only when the models are missing, to say so),
    // then go on with the words. The reader goes when it has answered.
    enum class OcrThen { Copy, Search, Web };
    void captureOcrThen(const QImage& image, double dpr, const QRect& where, OcrThen then);
    void quickOcrDone();
    void openWebSearch(const QString& query); // the default web search (defaultWebSearchUrl)
    // The pictures pinned on the screen: made when first wanted (a capture's
    // 贴图, or 贴到屏幕 with the capture off too); closed with the capture.
    capture::PinManager& pins();
    // 编辑: the editing window (EditWindow.qml, CaptureEditor), made the
    // first time, on a history entry (jt, the clipboard window) or a pin.
    // With the capture off too.
    capture::CaptureEditor* editor();
    void editHistory(qint64 clipId);
    void editPin(capture::PinWindow* pin);
    // 贴到屏幕: a picture file, read off the GUI thread; `screenshot`: one
    // of the screenshot history's (PinWindow::screenshot).
    void pinPicture(const QString& path, bool screenshot = false);
    // 贴上最近一次截图 (its hotkey, the tray menu): the clipboard's picture when one
    // was copied since that capture, else the last capture, where it was.
    void pinLastCapture();
    // A picture pinned at the cursor, inside its monitor's work area, scaled to fit it.
    void pinImage(QImage image, bool screenshot);
    // The clipboard's picture, pinned (pinImage); false when the clipboard holds none,
    // or when it has not changed since the last capture (that picture is m_last).
    bool pinClipboardImage();
    // The screenshot history's keyword as it works now (jt): none with the
    // capture off, nor when a web search or a favorite has the word.
    QString screenshotKeyword() const;
    void applyScreenshots(); // the keyword to the launcher and the plugins' catalog
    // take_screenshot (MCP): the user is asked on the screen
    // (ScreenshotConfirm.qml); only 允许 takes the picture, on a thread of
    // its own, into a PNG in the temporary folder (never the clipboard).
    void confirmScreenshot(const mcp::ScreenshotRequest& request, std::function<void(mcp::ScreenshotTaken)> done);
    Q_INVOKABLE void screenshotAnswered(int how); // the window's answered(): 1 allowed, 0 refused, -1 no answer
    void onDoubleCtrl(const KeyListener::Tap& tap); // toggleLauncher(), unless a game is played (GameGuard)
    // A press that opens or closes a window of ours reached the GUI thread
    // (the diagnostics journal's "trigger", TriggerJournal); a double tap's
    // also says which listener told of it, and when.
    void beginTrigger(TriggerSource source, std::uint64_t pressedAt, std::optional<TriggerJournal::Told> told = {});
    void journalTapNote(const KeyListener::Note& note);
    // win::bringToFront for a window a press asked for: how, and how long, to the journal.
    win::Front toFront(QWindow* window);
    const char* rawInputState() const; // the keyboard's Raw Input registration, for the journal
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
    // The clipboard's hotkey held after it opened the window (GitHub #107,
    // Settings::clipboardHoldToStep, hotkeysteps): its key again moves to an
    // older row, letting go pastes it. Watched (HoldWatch) from the opening
    // press, if it came over a program that keeps the keyboard, until the
    // whole hotkey is let go or something else ends it.
    void armClipboardHold(int hotkey, std::uint64_t pressedAt);
    bool clipboardStepPress(int hotkey); // its WM_HOTKEY: a step (done here), or a press as ever
    void clipboardHoldNote(hotkeysteps::Note note, std::uint64_t at);
    void checkClipboardHold(); // m_holdTimer: the hint a second in; the hook gone quiet
    void endClipboardHold(const char* why); // nothing pasted (any more); the window as it is
    void capDrainingWatch(std::uint64_t run); // its drain not over in time (kDrainCapMs)
    // A Shift pressed alone meanwhile (KeyRouter::Callbacks::shiftTapped),
    // meant for a Chinese input method, which none of the routed keys reach:
    // under a Chinese layout the page says that pinyin finds Chinese
    // (Clipboard::pinyinHintRequested). The window keeps off the focus all
    // the same (GitHub #42).
    void clipboardShiftTapped();
    // Taking the focus after all: to type a group's name (with the input
    // method), to hand over to another program.
    void activateClipboard();
    // Pinned (Clipboard::pinned): what would hide the window gives the keys
    // back instead, to the program it pastes into if `giveFocus` and the
    // window has the focus; engaging takes them again, routed from the
    // program in front, which it then pastes into.
    void releaseClipboard(bool giveFocus);
    void engageClipboard();
    void clipboardPinnedChanged();
    void uncloakClipboard(); // its first frame is up
    void prewarmClipboard();
    void finishClipPrewarm();
    QQuickItem* focusedField() const; // a text field of Win顺's own with the keyboard
    void returnToField(QQuickItem* field); // from the clipboard (Clipboard::fieldRequested)
    void updateCompanions(); // our windows that go with a file dialog (DialogJump)
    // Ctrl+1… on the launcher and the dialog bar: as one of them comes to
    // the front, which digits other programs hold (numberkeys::heldMask).
    void updateNumberKeys();
    void applySettings(bool initial);
    void applyKeyListener(bool force = false);
    ClipboardWatcher::Options clipboardOptions() const;
    void applyClipboard(bool initial = false); // the [Clipboard] settings
    void applyClipboardHotkeys(); // Win+V and the other shortcut; where Win+V stands, for the settings
    void applyQuickPasteHotkeys(); // global digits, suspended while Win顺 has the keys
    void clipCaptured(const ClipCapture& capture);
    void applyTaskbar(bool initial = false); // the [Taskbar] settings: Win+S, typing in the Start menu
    void applyTaskbarHotkeys(); // Win+S and Win+Shift+S; where Win+S stands, for the settings
    // The taskbar button was clicked (`clicked`), or Win+S pressed: the
    // launcher over the taskbar, or away again.
    void toggleAtTaskbar(bool clicked);
    void startScreenClip(); // Win+Shift+S, which Explorer gives up with Win+S
    void applyTypeToSearch(); // typing in the Start menu, in Explorer, Ctrl+V there (TypeToSearch), as the settings say
    void startMenuTyped(); // typed in the Start menu (TypeToSearch): the launcher over the taskbar
    // Ctrl+V taken in the Explorer tab or on the desktop `view` (TypeToSearch):
    // WinShunSearch.exe pastes the clipboard there as a file.
    void pasteAsFile(HWND view);
    // Typed in the Explorer window `root` (TypeToSearch): its `tab`'s folder
    // is asked for on a worker, then the launcher opens in that folder, on
    // that window's screen, and the keys follow; with no folder there, the
    // keys go back to Explorer.
    void folderTyped(HWND root, HWND tab, bool opus, std::uint64_t request);
    void typingFolderKnown(HWND root, HWND tab, bool opus, std::uint64_t request, const QString& folder);
    // A folder opened in the launcher (Launcher::setFolderOpener): up from
    // typing in an Explorer tab, that tab goes there and its window comes
    // back to the front, so typing goes on in the new folder (GitHub #39).
    // False otherwise, or with the tab gone: a new window, as from anywhere.
    // `select`: an item in it to select there (Ctrl+Enter, revealInTypingTab).
    bool openInTypingTab(const QString& folder, const QString& select = {});
    // Ctrl+Enter on a file or folder (Launcher::setRevealer): its folder in
    // that tab, the item selected (GitHub #60). False with another file
    // manager set, for a drive, or as openInTypingTab: the usual way.
    bool revealInTypingTab(const QString& path);
    // Win顺's own search box on the taskbar ([Taskbar] SearchBox, taskbar::SearchBox).
    void applySearchBox();
    void setSearchBoxShown(taskbar::SearchBox::Hidden why); // it is on the taskbar (None), or why not
    void setSearchBoxWide(bool wide); // the whole box, or its label or icon alone (SearchBox::wide)
    void setClassicTaskbar(bool classic); // SearchBox::Callbacks::classicChanged
    void updateSearchBoxLook(); // placeholder, accent
    void searchBoxPressed(const taskbar::SearchBox::Press& press);
    void showLauncherInBox(); // the launcher over the box (or, without one shown, over the taskbar)
    // What the launcher's field has, drawn in the box while it is open over it.
    void mirrorField(const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    std::optional<QRectF> boxCaretInField() const; // the box's caret, in the field's coordinates
    // Asked for by typing there (startMenuTyped, typingFolderKnown): the
    // keys typed go to it once it is ready for them (searchtyping::handoff,
    // GitHub #103). Looked at again as each part of that comes (activeChanged,
    // the focus object, revealLauncher) and at the cap.
    void deliverTyping();
    void beginHandoff(const QString& surface); // typed in "start", "explorer" or "opus": the launcher comes for the keys
    void endHandoff(); // its diagnostics recorded (one entry each, times alone), its watch over
    void explorerRestarted(bool back); // `back`: the taskbar is
    // Explorer registers its keys once, as it starts: ours with a Win+<letter>
    // that is going back to it are let go of before it does, and registered
    // again by the apply functions once it is up.
    void releaseWinChordsForExplorer();
    void applyWinKeyedHotkeys(); // the launcher's, the folder menu's and the capture's, any of which may be a Win+S or Win+V
    void giveBackWinKey(const QString& letter); // "S" or "V": out of DisabledHotkeys, whoever put it there
    void setClipboardPaused(bool paused);
    void showClipboardSettings(); // the settings window on its 剪贴板 page
    // "截图已保存" (or why not) as a tray notification; a click shows the file
    // in its folder, in the file manager chosen (GitHub #99).
    void notifyCaptureSaved(const QString& path, const QString& error);
    void showWebSettings(); // ... on its 网页搜索 page (a web row's menu)
    void showFileTypeSettings(); // ... on its 结果列表 page, at the file types (the + of the launcher's strip)
    void showFavoriteSettings(const QString& path); // ... on its 收藏夹 page, at that favorite (a row's menu)
    void showPluginSettings(const QString& pluginId); // ... on that plugin's card ("winshun:settings")
    void applyHotkey();
    void applyDialogs(); // Ctrl+G, the bar and going by themselves in file dialogs
    void applyFolderMenu(); // the menu on a double click in File Explorer (GitHub #22)
    void applyFolderMenuHotkey(); // ... and on its hotkey, anywhere (GitHub #25)
    void applyDropPie(); // the favorite folders round the pointer, on the key under Esc with a file held
    void folderMenuHotkey(); // the hotkey was pressed: the menu, the launcher out of the way first
    // The screen capture (src/capture): made while it is on in the settings,
    // its hotkey and PrtScn (taken from the Snipping Tool and given back).
    void applyCapture();
    void applyCaptureHotkeys();
    void triggerCapture(); // the hotkey, PrtScn, the tray menu: one more press ends it
    void excludeFromDialogBar(const QString& app); // "not in this program" from the bar's menu
    void answerAutoJump(const QString& when); // asked under the bar: "open", "back" or "" (DialogBar::autoJumpAnswered)
    bool createBarWindow();
    void reloadSettings();
    void settingsEdited(const Settings& settings);
    void showTrayMenu();
    void restart(const QStringList& arguments, Elevation elevation = Elevation::Same);
    // 索引方式 (Settings::indexAccess, GitHub #21): where the volumes are
    // read from, the index service looked for again while it is wanted and
    // not there, the autostart task's run level kept with the mode.
    void applyAutostartLevel(); // at start: the task's run level follows the mode (and an old elevated task sets the mode, portable)
    // Rewrites the task (`task`: whether it runs Win顺 elevated, as read)
    // when that is not the mode's level, where this process may.
    void reconcileAutostart(const std::optional<bool>& task);
    void setAutostart(bool on); // the settings' switch and the tray menu's item
    void readAutostart(); // the task read again in the background, then shown
    void showAutostart(); // in the settings window, as it is now
    void applyIndexAccess(); // the setting changed: restart with other rights, or switch in place
    void startServiceRetry(); // every 2 s for 30 s after start, then every 5 min
    void probeService(); // on a thread of its own; serviceProbed() on the GUI thread
    void serviceProbed(std::shared_ptr<ntfs::ServiceVolumeAccess> service, bool reached, ntfs::ServiceState state);
    void refreshIndexAccess(); // shown in the settings window
    QString serviceProblem() const; // why the index service is not in use and what happens instead, a sentence for the user
    void installIndexService(bool install); // WinShunService.exe --install / --uninstall, elevated (a portable copy)
    // 索引哪些地方 (Settings::indexScope, GitHub #52): only the chosen folders,
    // as the settings and the service have it, when the volumes are walked
    // (`walked`: no way to their MFT and journals, NoVolumeAccess).
    bool foldersInEffect(bool walked) const;
    // The way to the volumes changed: the index takes the chosen folders, or
    // whole disks, as foldersInEffect says now; `told`: the user is told
    // when that changes (the service came or went), not for their own settings.
    // Nothing else when it stays as it was: the service is looked at every
    // few minutes, and the folders' paths are looked up on the disk.
    void applyIndexScope(bool told);
    // Only the chosen folders indexed now: a list never set is taken for good
    // (Settings::chooseDefaultIndexFolders), and saved.
    void takeDefaultIndexFolders();
    void refreshFolderStatuses(); // the chosen folders' cards in the settings window
    // The launcher's notes about what lies outside the chosen folders (GitHub
    // #52): whether only they are indexed, which, and whether the note was closed.
    void tellLauncherFolders();
    void addFolderFromLauncher(const QString& path); // its "加入索引": as the settings add one
    void applyTheme();
    void guardDarkFrames(); // the frames' dark mode back once Qt has set it light (theme or palette change)
    void applyAppearance(); // the chosen theme and language
    QString trayTooltip() const;
    QString doubleCtrlName() const;
    void armReveal();
    void revealLauncher(); // slides in (Placement::slideIn) unless it went away meanwhile
    void finishHidingLauncher(); // slid out (Placement::slideOut), or at once
    bool launcherShown() const; // up, and not on its way out
    void prewarmLauncher();
    void finishPrewarm();
    void refreshContentIndexStatus(); // shown in the settings window
    void refreshShareStatuses(); // network shares: in the settings window, and told once read
    void signInToServer(const QString& path, unsigned long error = 0, const QString& user = {});
    // 索引位置: the index files go where [Index] Folder says, moved in the
    // background; when that fails, the setting goes back to where they are.
    QString startIndexFolder(); // where the index starts out
    void applyIndexFolder();
    QString indexFolderProblem(const QString& folder) const; // why the index cannot go there; empty if it can
    void indexMoved(int error);
    void indexMoveFailed(const QString& attempted, const QString& problem); // `attempted`: the setting
    void refreshIndexFolder(bool measure = false); // shown in the settings window; `measure`: its size too
    // AI 助手: the MCP server (src/mcp) on its pipe while [Mcp] Enabled, its
    // tools gated and shaped by the settings (mcpPolicy); what is connected
    // is shown in the settings window.
    void applyMcp();
    mcp::Policy mcpPolicy() const;
    void refreshMcpStatus();
    void writeMcpToolsCache(); // the tools, for the relay to list while Win顺 is not running
    // The plugins' commands as the AI's tools (plugin::AiBridge): what the
    // [Mcp] Plugins switches and the cards' "给 AI 用" say, applied from
    // applyMcp and applyPlugins alike.
    void applyAiBridge();
    // 插件: the plugin host (src/plugin), told what the [Plugin.<id>]
    // sections and [WebSearch] say; its plugins answer in the launcher.
    void createPluginHost();
    void applyPlugins();
    void pluginsChanged(); // the catalog's entries: the icon provider's folders
    // Win顺's tools for the plugins' winshun/tool (plugin::Tools): made
    // with the first call, whatever the AI switch says.
    plugin::Tools& pluginTools();
    mcp::Services toolServices() const; // what the tool hosts (the AI's, the plugins') need from the app

    // ---- AI 助手: get_settings, change_settings, open_settings (AiSettings) ----
    // What the AI's tool host asks of the app; all on the GUI thread. The
    // host asks the user (confirmSettingsChange) unless the mode is auto
    // and nothing asks always, then has applyAiSettings make the changes
    // from a queued invocation of its own and answers with what it says.
    aisettings::State aiState() const; // the settings, and autostart
    aisettings::Env aiEnv() const;
    QJsonObject aiFacts() const; // what Win顺 knows of this PC that bears on the settings
    mcp::ToolResult describeAiSettings(const mcp::SettingsQuery& query) const;
    mcp::SettingsPlan planAiSettings(const QJsonArray& changes) const;
    void applyAiSettings(const mcp::SettingsApply& apply, std::function<void(mcp::ToolResult)> done);
    mcp::ToolResult openAiSettings(const QString& key, const QString& section);

    // What came of making a state real: what did not take (and was put
    // back), and what the user still has to know, by the settings' keys.
    struct AiFailure {
        QString why; // English, for the model
        QString shown; // Chinese, for the notice: short
    };
    struct AiApplyReport {
        QHash<QString, AiFailure> failed; // by key
        QHash<QString, QString> notes; // key -> what else, in English
        bool explorerRestart = false; // File Explorer has to restart for some of it to show
    };
    // Saves `wanted`, hands it to settingsEdited (so that hotkeys are
    // registered, Win+V taken, and so on) and the autostart task; a shortcut
    // or a Windows setting that did not take is put back to `rollback`'s
    // value for it (a pair per key that changes). `alwaysAllow` also
    // sets [Mcp] Settings to auto.
    AiApplyReport applyAiState(const aisettings::State& wanted,
        const std::vector<std::pair<const aisettings::Entry*, QJsonValue>>& rollback, bool alwaysAllow);
    // The last batch AI changed, for the notice and its 撤销 (undoSettingsBatch).
    struct SettingsBatch {
        quint64 id = 0;
        QString client; // as it named itself; empty: not known
        QString reason; // its own words
        QJsonArray items; // what took: aisettings::itemJson of each, with `note` where there is one
        QJsonArray failed; // what did not and was put back: {key, title, reason} of each
        bool explorerRestart = false; // File Explorer has to restart for some of it to show
        bool undone = false;

        // For QML: {id, client, reason, count, explorerRestart, items: [{key, title, old_text, new_text, diff_text, is_list, effect, note…}]}
        QVariantMap toVariant() const;
    };
    // ---- the seams of the screens (the confirmation card, the notice) ----
    // The card that asks the user to allow what an AI client wants to
    // change: `ask.client`, `ask.reason` (the AI's words, to be shown as
    // such), `ask.items` (QJsonArray::toVariantList() is QML's: title,
    // old_text, new_text, diff_text, is_list, ask_always, effect) and
    // `ask.askAlways` (no "from now on" box). `done(answer, always)` once, with
    // mcp::SettingsAnswer (allowed, refused, no answer in 60 s, could not be
    // shown) and whether the box was ticked; it is not called after
    // closeSettingsConfirm(). The card is m_settingsAsk, one at a time;
    // its QML signal answered(int, bool) goes to settingsAnswered.
    void confirmSettingsChange(const mcp::SettingsAsk& ask, std::function<void(mcp::SettingsAnswer answer, bool always)> done);
    void closeSettingsConfirm(); // the call was given up (notifications/cancelled): the card goes, no answer
    Q_INVOKABLE void settingsAnswered(int answer, bool always);
    // The small notice shown after a change was made, in either mode (batch.toVariant()).
    void showSettingsChangedNotice(const SettingsBatch& batch);
    // Hotkeys, Win+V, PrtScn: what the last apply of a setting reported, by
    // the setting's key, for the answer to change_settings.
    void noteApplyProblem(const QString& key, const QString& problem);

    // The drop pie's copies and moves: first, so it goes last, waiting for
    // one under way (never cut off halfway); the pie itself may go any time.
    std::unique_ptr<piedrop::FileOperations> m_fileOperations;
    Settings m_settings;
    std::unique_ptr<History> m_history;
    std::unique_ptr<Favorites> m_favorites; // the launcher's and the dialog bar's
    std::unique_ptr<IndexService> m_index;
    // The index service (GitHub #21): wanted by the mode, so looked for
    // while not in use; the probe's thread goes before the index does.
    bool m_serviceWanted = false;
    QString m_serviceWhy; // why the last look found it unusable (English, from the pipe client)
    ntfs::ServiceState m_serviceState {}; // what the SCM said at the last look (Unknown at first)
    QTimer m_serviceRetry;
    QElapsedTimer m_serviceRetrySince; // the 30 s of frequent tries after start
    std::atomic<bool> m_serviceProbing {false};
    std::jthread m_serviceProbe;
    bool m_accessTakenFromTask = false; // the mode was taken over from an old elevated task: said in the settings
    bool m_foldersInEffect = false; // only the chosen folders are indexed (foldersInEffect), as last applied
    QString m_serviceMessage; // ... and how installing or uninstalling the service went
    bool m_serviceBusy = false; // WinShunService.exe --install / --uninstall running
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
    // The MCP server's own engine, host and server (applyMcp); in this
    // order, so the server goes first and the engine before the index.
    std::unique_ptr<SearchEngine> m_mcpEngine;
    std::unique_ptr<mcp::ToolHost> m_mcpHost;
    std::unique_ptr<mcp::McpServer> m_mcpServer;
    std::unique_ptr<plugin::AiBridge> m_aiBridge; // with the server: the plugins' commands among its tools
    std::unique_ptr<plugin::Tools> m_pluginTools; // before the host, whose bridge calls it
    std::unique_ptr<plugin::Host> m_pluginHost; // before the launcher, which asks it
    // The system switches' states (docs/design-system-switches.md); likewise.
    std::unique_ptr<Switches> m_switches;
    std::unique_ptr<Launcher> m_launcher;
    std::unique_ptr<ClipStore> m_clipStore; // the clipboard history
    std::unique_ptr<Clipboard> m_clipboard; // its window's view-model
    std::unique_ptr<ClipImportJob> m_clipImport; // an import of Ditto's, a text or a CSV file, while one runs
    std::unique_ptr<MissingFiles> m_missingFiles; // its entries of files that are gone (GitHub #54)
    std::unique_ptr<FilePreview> m_preview; // Alt+P on a result
    std::unique_ptr<WindowFrame> m_previewFrame;
    std::unique_ptr<capture::CaptureOcr> m_captureOcr; // the OCR window's view-model, made with it
    std::unique_ptr<WindowFrame> m_ocrFrame;
    QPointer<QQuickWindow> m_ocrWindow;
    std::unique_ptr<capture::CaptureOcr> m_quickOcr; // while a read for ocrcopy / ocrsearch / ocrweb runs
    OcrThen m_quickOcrThen = OcrThen::Copy;
    // Destroyed before the two above: its thread calls back into the app, and
    // the page waits on its writes.
    std::unique_ptr<ClipboardWatcher> m_clipWatcher;
    bool m_clipboardPaused = false; // from the tray menu, until Win顺 restarts
    bool m_winVRegistryFailed = false;
    bool m_winSRegistryFailed = false;
    bool m_restartingExplorer = false;
    // restartExplorer's waiting, waited for on exit: quitting between ending
    // Explorer and starting it again would leave no taskbar.
    std::jthread m_explorerRestart;
    win32::UniqueHandle m_taskbarEvent; // set by the taskbar button (WinShunSearch.exe)
    std::unique_ptr<QWinEventNotifier> m_taskbarNotifier; // waits on it; goes first
    QElapsedTimer m_launcherHidden; // since the launcher last went away
    QElapsedTimer m_launcherShown; // since it was last asked for
    // It lost the front right after it came: said in the log (GitHub #111).
    // The tick it was brought to the front: input after that is not its
    // own empty one (bringToFront).
    void logLauncherLostFront(HWND front);
    std::uint32_t m_launcherFrontTick = 0;
    int m_launcherLostFrontLogged = 0;
    // When the launcher, and the clipboard, last came on the screen
    // (revealLauncher, uncloakClipboard; GetTickCount64), until hidden: a
    // press of their key before that moment did not see them (seenBy).
    std::optional<std::uint64_t> m_launcherRevealedAt;
    std::optional<std::uint64_t> m_clipRevealedAt;
    std::unique_ptr<TriggerJournal> m_triggers; // what each press came to, for the diagnostics journal
    diagnostics::JournalBudget m_tapNotes; // double taps that came to nothing (journalTapNote)
    // Whether a hidden pointer was Windows' doing on a key press (GameGuard;
    // with double tap and its pause in games).
    std::unique_ptr<PointerWatch> m_pointerWatch;
    bool m_launcherClosing = false; // sliding out (hideLauncher)
    std::unique_ptr<ComWorker> m_typingWorker; // queries a file display's folder (folderTyped); before m_typing
    std::unique_ptr<TypeToSearch> m_typing; // while StartMenuTyping, ExplorerTyping, OpusTyping or PasteFile is on
    // The launcher is up from typing in this Explorer or Opus window and tab:
    // Esc goes back to the window, a folder opened goes to the tab (openInTypingTab).
    // Cleared by any other opening (showLauncher), by hiding the launcher,
    // and when a pinned one loses the focus: never another opening's.
    struct TypingOrigin {
        HWND root = nullptr;
        HWND tab = nullptr;
        bool opus = false;
    };
    TypingOrigin m_typingOrigin;
    std::unique_ptr<taskbar::SearchBox> m_searchBox; // while [Taskbar] SearchBox is on (Windows 11)
    bool m_searchBoxShown = false; // ... and it is on the taskbar
    taskbar::SearchBox::Hidden m_searchBoxHidden = taskbar::SearchBox::Hidden::Pending; // ... or why not
    bool m_classicTaskbar = false; // ... measured on Windows 10's taskbar (StartAllBack, ExplorerPatcher)
    // The launcher is open over the box: typing in it when the box is wide
    // (Launcher::inTaskbarBox), else with its own field, the box looking open.
    bool m_launcherOverBox = false;
    // A press on the box takes the keyboard from the launcher over it (Windows
    // does that for whatever is clicked on the taskbar): it comes back with
    // the press (searchBoxPressed), else it goes once this runs out.
    QTimer m_boxPressGrace;
    // Pinned (Launcher::pinned), it lost the focus to a press on the box:
    // that press closes it (searchBoxPressed). Since when.
    QElapsedTimer m_pinnedLostToBox;
    QPointer<QQuickItem> m_searchField; // the launcher's (SearchBar.qml): the input method asks it where the caret is
    taskbar::SearchBox::Text m_fieldShown; // what it last reported (mirrorField), for the box
    bool m_deliverTyping = false; // the launcher was asked for by typing in the Start menu or in Explorer
    QTimer m_typingCap; // ... the keys go all the same when this runs out (searchtyping::kHandoffCapMs)
    // The handoff of those keys, for its diagnostics (endHandoff): when each
    // part came, from App's side; the hook's and the window's are added then.
    struct TypingHandoff {
        QString surface; // "start", "explorer", "opus"; empty: none
        std::chrono::steady_clock::time_point asked, active, focused, revealed, delivered;
        QString outcome; // ready, cap, hidden, hidden-before-active, never-active, replaced
        QString missing; // what was not ready when they went (cap)
        bool inFront = false; // the launcher was the window in front when they went
        quint32 layout = 0; // the keyboard layout (HKL) then
    };
    TypingHandoff m_handoff;
    std::unique_ptr<LauncherKeysWatch> m_keysWatch; // the launcher's window, a moment after the keys went
    QTimer m_handoffWatch; // ... that moment
    QString m_clipboardShortcut; // what opens the clipboard: "Win+V", another shortcut, or nothing
    std::unique_ptr<MessageWindow> m_messages;
    std::unique_ptr<Updater> m_updater;
    std::unique_ptr<Backup> m_backup; // of the settings, into a folder chosen
    std::unique_ptr<VolumeNotifier> m_volumeNotifier; // after m_messages, whose window it uses
    std::unique_ptr<NetworkNotifier> m_network; // after m_index, whose shares it tells
    std::unique_ptr<IdleTrim> m_idleTrim; // gives the memory back while nothing is in use
    std::vector<std::wstring> m_sharesBuilding; // being read for the first time: told when done
    std::unique_ptr<KeyListener> m_keyListener; // double Ctrl, double clicks for the folder menu
    std::unique_ptr<ServiceKeyListener> m_serviceKeyListener; // destroyed before its target KeyListener
    int m_keyListenerMode = 0; // 1 double Ctrl, 2 double clicks, 4 drop pie, 8 recording clicks; -1 shutting down
    std::shared_ptr<capture::recording::ClickStream> m_recordingClicks;
    std::unique_ptr<FolderMenu> m_folderMenu; // before the window: its view-model
    QPointer<QQuickWindow> m_folderMenuWindow;
    std::unique_ptr<DropPie> m_dropPie; // before the window: its view-model
    QPointer<QQuickWindow> m_dropPieWindow;
    QStringList m_doubleCtrlIgnoredLogged; // "program reason", logged once each: a game gets many
    std::unique_ptr<DialogJump> m_dialogJump; // file dialogs: Ctrl+G, the bar; after m_messages, which has the hotkey
    std::unique_ptr<DialogBar> m_dialogBar; // the search bar under file dialogs
    std::unique_ptr<ServiceDialogNav> m_serviceNav; // navigates elevated dialogs via the service (#104)
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
    std::uint64_t m_drainingRun = 0;
    // The clipboard's hotkey held (armClipboardHold): the watch, what its
    // notes came to, which hotkey (its id) and keys.
    std::unique_ptr<HoldWatch> m_holdWatch;
    std::uint64_t m_holdRun = 0; // which hold the watch's notes belong to
    std::unique_ptr<HoldWatch> m_drainingWatch; // done, until an Esc it took is let go (HoldWatch::drain)
    std::uint64_t m_drainingWatchRun = 0;
    std::optional<hotkeysteps::Hold> m_hold;
    int m_holdHotkey = 0;
    unsigned m_holdModifiers = 0; // hotkeysteps::kAlt...
    unsigned m_holdVk = 0;
    std::uint64_t m_holdLate = 0; // ms from the opening press to the watch, for the journal
    QTimer m_holdTimer;
    FrameReveal m_clipReveal; // shown cloaked until a frame from this opening
    std::atomic<bool> m_clipPrewarming {false}; // shown cloaked for one frame (see prewarmClipboard)
    FrameReveal m_launcherReveal; // armed once the fresh results are ready (showLauncher)
    std::atomic<bool> m_prewarming {false}; // shown cloaked for one frame (see prewarmLauncher)
    std::unique_ptr<QQmlApplicationEngine> m_qml; // destroyed first: QML references the objects above
    // Its overlay windows are the engine's: reset in ~App, before the engine goes.
    std::unique_ptr<capture::Capture> m_capture;
    std::unique_ptr<capture::PinManager> m_pins; // pins(); reset in ~App with the capture
    // The editing window and what it needs; the window is the engine's, deleted in ~App before it.
    std::unique_ptr<capture::CaptureEditor> m_editor;
    std::unique_ptr<WindowFrame> m_editFrame;
    QPointer<QQuickWindow> m_editWindow;
    struct ScreenshotAsk { // take_screenshot waiting for the user's answer: one at a time
        std::function<void(mcp::ScreenshotTaken)> done;
        HMONITOR monitor = nullptr;
        QString label; // the monitor, in English words, for the client
        std::unique_ptr<WindowFrame> frame;
        QPointer<QQuickWindow> window;
    };
    std::unique_ptr<ScreenshotAsk> m_screenshotAsk;
    struct SettingsAskSlot { // change_settings waiting for the user's answer: one at a time
        std::function<void(mcp::SettingsAnswer, bool)> done;
        std::unique_ptr<WindowFrame> frame;
        QPointer<QQuickWindow> window;
    };
    std::unique_ptr<SettingsAskSlot> m_settingsAsk;
    std::unique_ptr<WindowFrame> m_noticeFrame; // the notice after an AI changed settings: made once, kept
    QPointer<QQuickWindow> m_noticeWindow;
    std::optional<SettingsBatch> m_settingsBatch;
    quint64 m_settingsBatchIds = 0;
    QHash<QString, QString> m_applyProblems; // noteApplyProblem
    bool m_aiApplying = false; // an AI's change is being applied: what goes wrong is in its answer, not in a balloon
    QString m_winVState; // as applyClipboardHotkeys last found it: off, on, waiting, releasing, failed, blocked
    unsigned m_quickPasteKeys = 0; // successfully registered digits, 1 << n
    QString m_winSState; // likewise for Win+S
    // In front when the launcher last opened: where a screenshot of the
    // history (jt, Enter) is pasted.
    HWND m_launcherTarget = nullptr;
    std::jthread m_pinSaver; // 保存 on a pin while the capture is off (capture::saveImage)
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
    Elevation m_restartElevation = Elevation::Same;

    QFileSystemWatcher m_settingsWatcher;
    QTimer m_settingsReload;
    QTimer m_indexOptionsApply; // rebuilding the crawler is costly: batch quick edits
    QTimer m_contentStatusTimer; // while the settings window is open
    QTimer m_darkFrameGuard; // see guardDarkFrames
    QElapsedTimer m_darkFrameWatch;
    bool m_darkFrame = false;
    bool m_diagnosticsExporting = false;
    std::jthread m_diagnosticsExport; // joined before the app's objects are destroyed
};

} // namespace ws
