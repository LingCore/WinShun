#pragma once

#include "ClipImport.h"
#include "IndexService.h"
#include "Settings.h"
#include "Unc.h"
#include "Wsl.h"

#include <QDir>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

namespace ws {

class FavoritesSettings;
class PluginSettings;
class ShortcutCapture;
class SystemCatalog;

// The view-model behind the settings window. Every change is saved right
// away and reported through edited(); there is no OK / Cancel step.
class SettingsEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool doubleCtrl READ doubleCtrl WRITE setDoubleCtrl NOTIFY changed FINAL)
    Q_PROPERTY(QString doubleTapKey READ doubleTapKey WRITE setDoubleTapKey NOTIFY changed FINAL) // ctrl | alt | shift
    Q_PROPERTY(QString doubleCtrlKey READ doubleCtrlKey WRITE setDoubleCtrlKey NOTIFY changed FINAL) // both | left | right
    // The Ctrl PowerToys' Find My Mouse also opens on with a double tap,
    // when that is one Win顺 listens to as well: left | right; empty when
    // none (refreshDoubleCtrlClash).
    Q_PROPERTY(QString doubleCtrlClash READ doubleCtrlClash NOTIFY doubleCtrlClashChanged FINAL)
    Q_PROPERTY(bool doubleCtrlPauseInGames READ doubleCtrlPauseInGames WRITE setDoubleCtrlPauseInGames NOTIFY changed FINAL)
    Q_PROPERTY(bool doubleCtrlPauseInFullScreen READ doubleCtrlPauseInFullScreen WRITE setDoubleCtrlPauseInFullScreen
            NOTIFY changed FINAL)
    Q_PROPERTY(QStringList doubleCtrlExcludedApps READ doubleCtrlExcludedApps NOTIFY changed FINAL)
    Q_PROPERTY(QString hotkey READ hotkey WRITE setHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString shortcutAgain READ shortcutAgain WRITE setShortcutAgain NOTIFY changed FINAL) // close | panel
    Q_PROPERTY(QString hotkeyError READ hotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(bool recordingHotkey READ recordingHotkey WRITE setRecordingHotkey NOTIFY recordingHotkeyChanged FINAL)
    Q_PROPERTY(bool autostart READ autostart WRITE setAutostart NOTIFY autostartChanged FINAL)
    Q_PROPERTY(bool recordHistory READ recordHistory WRITE setRecordHistory NOTIFY changed FINAL)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY historyCountChanged FINAL)
    // The launcher is wider or narrower than at first (dragged at its edge; Placement).
    Q_PROPERTY(bool launcherWidthSet READ launcherWidthSet NOTIFY launcherWidthSetChanged FINAL)
    Q_PROPERTY(bool dialogJump READ dialogJump WRITE setDialogJump NOTIFY changed FINAL)
    Q_PROPERTY(bool dialogBar READ dialogBar WRITE setDialogBar NOTIFY changed FINAL)
    Q_PROPERTY(QString dialogBarPlace READ dialogBarPlace WRITE setDialogBarPlace NOTIFY changed FINAL)
    Q_PROPERTY(bool dialogAutoJump READ dialogAutoJump WRITE setDialogAutoJump NOTIFY changed FINAL)
    Q_PROPERTY(QString dialogAutoJumpWhen READ dialogAutoJumpWhen WRITE setDialogAutoJumpWhen NOTIFY changed FINAL)
    Q_PROPERTY(QStringList dialogBarExcludedApps READ dialogBarExcludedApps NOTIFY changed FINAL)
    // Where folders open (Settings::fileManager), and what there is to choose
    // from: "explorer" and the others installed, looked for again as the
    // window comes to the front (refreshFileManagers).
    Q_PROPERTY(QString fileManager READ fileManager WRITE setFileManager NOTIFY changed FINAL)
    Q_PROPERTY(QStringList fileManagers READ fileManagers NOTIFY fileManagersChanged FINAL)
    // The command lines of "custom" (Settings::fileManagerOpenCommand,
    // fileManagerRevealCommand), and what is wrong with them: no open
    // command, a program not found (empty: nothing).
    Q_PROPERTY(QString fileManagerOpenCommand READ fileManagerOpenCommand WRITE setFileManagerOpenCommand NOTIFY changed FINAL)
    Q_PROPERTY(QString fileManagerRevealCommand READ fileManagerRevealCommand WRITE setFileManagerRevealCommand NOTIFY changed FINAL)
    Q_PROPERTY(QString fileManagerProblem READ fileManagerProblem NOTIFY changed FINAL)
    Q_PROPERTY(bool explorerMenu READ explorerMenu WRITE setExplorerMenu NOTIFY changed FINAL)
    Q_PROPERTY(bool explorerTyping READ explorerTyping WRITE setExplorerTyping NOTIFY changed FINAL)
    Q_PROPERTY(bool opusTyping READ opusTyping WRITE setOpusTyping NOTIFY changed FINAL)
    Q_PROPERTY(QString folderTyping READ folderTyping WRITE setFolderTyping NOTIFY changed FINAL) // first | only (GitHub #110)
    // The folder menu on a double click (Settings::folderMenu: on unless "off").
    Q_PROPERTY(bool folderMenu READ folderMenu WRITE setFolderMenu NOTIFY changed FINAL)
    Q_PROPERTY(bool folderMenuExplorer READ folderMenuExplorer WRITE setFolderMenuExplorer NOTIFY changed FINAL)
    // The groups it lists (GitHub #51); the last one on cannot be switched off.
    Q_PROPERTY(bool folderMenuFavorites READ folderMenuFavorites WRITE setFolderMenuFavorites NOTIFY changed FINAL)
    Q_PROPERTY(bool folderMenuRecent READ folderMenuRecent WRITE setFolderMenuRecent NOTIFY changed FINAL)
    Q_PROPERTY(bool folderMenuWindows READ folderMenuWindows WRITE setFolderMenuWindows NOTIFY changed FINAL)
    // The folder menu in Directory Opus, which runs a command of its settings
    // on a double click on blank space (refreshOpusFolderMenu): on | stale
    // (another copy's WinShunSearch.exe) | off | taken (a command of the
    // user's own: opusFolderMenuTaken) | manual (no settings file, or one
    // not understood: set up by hand) | busy (setOpusFolderMenu at work).
    Q_PROPERTY(QString opusFolderMenu READ opusFolderMenu NOTIFY opusFolderMenuChanged FINAL)
    Q_PROPERTY(QString opusFolderMenuTaken READ opusFolderMenuTaken NOTIFY opusFolderMenuChanged FINAL)
    Q_PROPERTY(QString opusFolderMenuProblem READ opusFolderMenuProblem NOTIFY opusFolderMenuChanged FINAL)
    Q_PROPERTY(bool opusRunning READ opusRunning NOTIFY opusFolderMenuChanged FINAL) // setting it restarts Opus
    Q_PROPERTY(bool folderMenuDesktop READ folderMenuDesktop WRITE setFolderMenuDesktop NOTIFY changed FINAL)
    // The hotkey that brings the folder menu up anywhere (GitHub #25); its
    // error as the app found it (taken, not understood), like hotkeyError.
    Q_PROPERTY(QString folderMenuHotkey READ folderMenuHotkey WRITE setFolderMenuHotkey NOTIFY changed FINAL)
    Q_PROPERTY(bool dropPie READ dropPie WRITE setDropPie NOTIFY changed FINAL) // 拖放圆盘
    Q_PROPERTY(QString folderMenuHotkeyError READ folderMenuHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    // The screen capture's card on the 插件 page ([Capture]): its switch, its
    // hotkey and that hotkey's error (as hotkeyError), PrtScn, and whether
    // letting go of a drag copies at once; captureNote says what PrtScn's
    // takeover could not do.
    Q_PROPERTY(QString captureLastPinHotkey READ captureLastPinHotkey WRITE setCaptureLastPinHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString captureSaveFolder READ captureSaveFolder WRITE setCaptureSaveFolder NOTIFY changed FINAL)
    Q_PROPERTY(QString captureRecordingFolder READ captureRecordingFolder WRITE setCaptureRecordingFolder NOTIFY changed FINAL)
    Q_PROPERTY(bool captureRecordingSystemAudio READ captureRecordingSystemAudio WRITE setCaptureRecordingSystemAudio NOTIFY changed FINAL)
    Q_PROPERTY(bool captureRecordingMicrophone READ captureRecordingMicrophone WRITE setCaptureRecordingMicrophone NOTIFY changed FINAL)
    Q_PROPERTY(int captureRecordingQuality READ captureRecordingQuality WRITE setCaptureRecordingQuality NOTIFY changed FINAL)
    Q_PROPERTY(bool captureRecordingCursor READ captureRecordingCursor WRITE setCaptureRecordingCursor NOTIFY changed FINAL)
    Q_PROPERTY(bool captureRecordingCountdown READ captureRecordingCountdown WRITE setCaptureRecordingCountdown NOTIFY changed FINAL)
    Q_PROPERTY(bool captureRecordingClicks READ captureRecordingClicks WRITE setCaptureRecordingClicks NOTIFY changed FINAL)
    Q_PROPERTY(QString captureLastPinHotkeyError READ captureLastPinHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(bool captureEnabled READ captureEnabled WRITE setCaptureEnabled NOTIFY changed FINAL)
    Q_PROPERTY(QString captureHotkey READ captureHotkey WRITE setCaptureHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString captureHotkeyError READ captureHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(bool capturePrintScreen READ capturePrintScreen WRITE setCapturePrintScreen NOTIFY changed FINAL)
    Q_PROPERTY(bool captureCopyOnSelect READ captureCopyOnSelect WRITE setCaptureCopyOnSelect NOTIFY changed FINAL)
    Q_PROPERTY(bool captureMagnifier READ captureMagnifier WRITE setCaptureMagnifier NOTIFY changed FINAL)
    Q_PROPERTY(bool capturePinOnCopy READ capturePinOnCopy WRITE setCapturePinOnCopy NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseDoubleLeft READ captureMouseDoubleLeft WRITE setCaptureMouseDoubleLeft NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseRight READ captureMouseRight WRITE setCaptureMouseRight NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseMiddle READ captureMouseMiddle WRITE setCaptureMouseMiddle NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseBack READ captureMouseBack WRITE setCaptureMouseBack NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseForward READ captureMouseForward WRITE setCaptureMouseForward NOTIFY changed FINAL)
    // The capture toolbar's buttons left off it: comma separated ids (Settings::captureToolbarButtonIds).
    Q_PROPERTY(QString captureToolbarHidden READ captureToolbarHidden WRITE setCaptureToolbarHidden NOTIFY changed FINAL)
    Q_PROPERTY(QString captureNote READ captureNote NOTIFY hotkeyErrorChanged FINAL)
    // Smart redaction's options (Settings::captureSmart*).
    Q_PROPERTY(bool captureCursor READ captureCursor WRITE setCaptureCursor NOTIFY changed FINAL)
    Q_PROPERTY(bool captureHdrFile READ captureHdrFile WRITE setCaptureHdrFile NOTIFY changed FINAL)
    Q_PROPERTY(bool captureControlsUia READ captureControlsUia WRITE setCaptureControlsUia NOTIFY changed FINAL)
    Q_PROPERTY(int captureDelay READ captureDelay WRITE setCaptureDelay NOTIFY changed FINAL)
    Q_PROPERTY(QString capturePinDoubleClick READ capturePinDoubleClick WRITE setCapturePinDoubleClick NOTIFY changed FINAL)
    Q_PROPERTY(QString capturePinsHotkey READ capturePinsHotkey WRITE setCapturePinsHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString capturePinsRestoreHotkey READ capturePinsRestoreHotkey WRITE setCapturePinsRestoreHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString capturePinsHotkeyError READ capturePinsHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(QString capturePinsRestoreHotkeyError READ capturePinsRestoreHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(bool captureSmartPrescan READ captureSmartPrescan WRITE setCaptureSmartPrescan NOTIFY changed FINAL)
    Q_PROPERTY(bool captureSmartIpMac READ captureSmartIpMac WRITE setCaptureSmartIpMac NOTIFY changed FINAL)
    Q_PROPERTY(bool captureSmartAllText READ captureSmartAllText WRITE setCaptureSmartAllText NOTIFY changed FINAL)
    Q_PROPERTY(QString captureSmartWords READ captureSmartWords WRITE setCaptureSmartWords NOTIFY changed FINAL)
    // The screenshot history's keyword (jt): set through saveCaptureKeyword.
    Q_PROPERTY(QString captureKeyword READ captureKeyword NOTIFY changed FINAL)
    // 粘贴为文件's card on the 插件 page ([PasteFile]): its switch, Ctrl+V in
    // Explorer and on the desktop, the right-click menu's verb.
    Q_PROPERTY(bool pasteFileEnabled READ pasteFileEnabled WRITE setPasteFileEnabled NOTIFY changed FINAL)
    Q_PROPERTY(bool pasteFileCtrlV READ pasteFileCtrlV WRITE setPasteFileCtrlV NOTIFY changed FINAL)
    Q_PROPERTY(bool pasteFileMenu READ pasteFileMenu WRITE setPasteFileMenu NOTIFY changed FINAL)
    Q_PROPERTY(bool includeRemovableDrives READ includeRemovableDrives WRITE setIncludeRemovableDrives NOTIFY changed FINAL)
    Q_PROPERTY(bool rescanOnStartup READ rescanOnStartup WRITE setRescanOnStartup NOTIFY changed FINAL)
    // 索引方式 (Settings::indexAccess, GitHub #21): auto | service | direct |
    // walk, and, as the app has it (setIndexAccessState): the way in use
    // (direct | service | walk) and what it says, notes worth reading, what
    // this copy and account can offer (direct wants an administrator; the
    // service's install buttons are a portable copy's), and how installing
    // or uninstalling the service goes.
    Q_PROPERTY(QString indexAccess READ indexAccess WRITE setIndexAccess NOTIFY changed FINAL)
    Q_PROPERTY(QString indexAccessEffective READ indexAccessEffective NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QString indexAccessStatus READ indexAccessStatus NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QString indexAccessNote READ indexAccessNote NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool isAdministrator READ isAdministrator NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool isElevated READ isElevated NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool isPortable READ isPortable NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool indexServiceInstalled READ indexServiceInstalled NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool indexServiceBusy READ indexServiceBusy NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QString indexServiceState READ indexServiceState NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool indexServiceInstallable READ indexServiceInstallable NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool indexServiceUnsafeLocation READ indexServiceUnsafeLocation NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QString indexServiceMessage READ indexServiceMessage NOTIFY indexAccessStateChanged FINAL)
    // 索引哪些地方 (Settings::indexScope, GitHub #52): auto | drives | folders,
    // and, as the app has it (setIndexAccessState): only the chosen folders
    // are indexed now, they could be (not while the disks are read whole, by
    // the index service or 管理员直读), and what that comes to, said.
    Q_PROPERTY(QString indexScope READ indexScope WRITE setIndexScope NOTIFY changed FINAL)
    Q_PROPERTY(bool allDrives READ allDrives WRITE setAllDrives NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList drives READ drives NOTIFY drivesChanged FINAL)
    Q_PROPERTY(bool foldersInEffect READ foldersInEffect NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool foldersPossible READ foldersPossible NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QString indexScopeStatus READ indexScopeStatus NOTIFY indexAccessStateChanged FINAL)
    // The chosen folders, each as a ShareCard shows it: {path, name, tone (ok
    // | busy | idle | warn | error), status, busy, verifiable, network,
    // signIn, suspect}, as the app reports them (setFolderStatuses). The
    // known folders not chosen yet: [{name, path, network}].
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged FINAL)
    Q_PROPERTY(QVariantList commonFolders READ commonFolders NOTIFY foldersChanged FINAL)
    Q_PROPERTY(int folderVerifyHours READ folderVerifyHours WRITE setFolderVerifyHours NOTIFY changed FINAL)
    Q_PROPERTY(QStringList excludedPaths READ excludedPaths NOTIFY changed FINAL) // expanded, for display
    Q_PROPERTY(QStringList excludedNames READ excludedNames NOTIFY changed FINAL)
    Q_PROPERTY(QStringList excludedTypes READ excludedTypes NOTIFY changed FINAL)
    Q_PROPERTY(QStringList excludedFileNames READ excludedFileNames NOTIFY changed FINAL)
    Q_PROPERTY(bool contentSearch READ contentSearch WRITE setContentSearch NOTIFY changed FINAL)
    Q_PROPERTY(QStringList contentExtensions READ contentExtensions NOTIFY changed FINAL)
    // Size limits in MB by kind of file (ContentSizeLimits::Kind: text, code, data),
    // and the listed extensions of each kind (".c .cpp .h").
    Q_PROPERTY(QList<double> contentMaxSizeMB READ contentMaxSizeMB NOTIFY changed FINAL)
    Q_PROPERTY(QStringList contentKindExtensions READ contentKindExtensions NOTIFY changed FINAL)
    Q_PROPERTY(bool contentIndex READ contentIndex WRITE setContentIndex NOTIFY changed FINAL)
    Q_PROPERTY(bool contentInLowPriority READ contentInLowPriority WRITE setContentInLowPriority NOTIFY changed FINAL)
    Q_PROPERTY(bool contentDocuments READ contentDocuments WRITE setContentDocuments NOTIFY changed FINAL)
    Q_PROPERTY(QString contentIndexStatus READ contentIndexStatus NOTIFY contentIndexStatusChanged FINAL)
    // 结果列表
    Q_PROPERTY(bool foldersFirst READ foldersFirst WRITE setFoldersFirst NOTIFY changed FINAL)
    // 优先显示的文件类型 as shown and typed: "exe; bat; cmd"; written, read as
    // Settings::fileExtensions does (dots, spaces, commas, any case).
    Q_PROPERTY(QString preferredTypes READ preferredTypes WRITE setPreferredTypes NOTIFY changed FINAL)
    Q_PROPERTY(bool preferredTypesDefault READ preferredTypesDefault NOTIFY changed FINAL)
    Q_PROPERTY(bool showModified READ showModified WRITE setShowModified NOTIFY changed FINAL)
    Q_PROPERTY(bool showModifiedTime READ showModifiedTime WRITE setShowModifiedTime NOTIFY changed FINAL)
    Q_PROPERTY(bool showFileSize READ showFileSize WRITE setShowFileSize NOTIFY changed FINAL)
    Q_PROPERTY(bool doubleClickToOpen READ doubleClickToOpen WRITE setDoubleClickToOpen NOTIFY changed FINAL)
    Q_PROPERTY(bool tabEntersFavoriteFolder READ tabEntersFavoriteFolder WRITE setTabEntersFavoriteFolder NOTIFY changed FINAL)
    Q_PROPERTY(bool thumbnails READ thumbnails WRITE setThumbnails NOTIFY changed FINAL)
    Q_PROPERTY(bool webSuggestions READ webSuggestions WRITE setWebSuggestions NOTIFY changed FINAL)
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY changed FINAL) // match | newest | oldest | name | folder | type | largest | smallest (kSortNames)
    Q_PROPERTY(QString startList READ startList WRITE setStartList NOTIFY changed FINAL) // opened | modified | none
    Q_PROPERTY(bool clearOnOpen READ clearOnOpen WRITE setClearOnOpen NOTIFY changed FINAL)
    Q_PROPERTY(QString numberKeys READ numberKeys WRITE setNumberKeys NOTIFY changed FINAL) // ctrl | alt | off
    Q_PROPERTY(QString numberHints READ numberHints WRITE setNumberHints NOTIFY changed FINAL) // held | always | off
    Q_PROPERTY(QString previewWith READ previewWith WRITE setPreviewWith NOTIFY changed FINAL) // auto | builtin
    // Which of the chosen number keys are other programs' hotkeys, or Win顺's
    // own shortcuts, said for the user; empty when none (refreshNumberKeys).
    Q_PROPERTY(QString numberKeysNote READ numberKeysNote NOTIFY numberKeysNoteChanged FINAL)
    // 剪贴板
    Q_PROPERTY(bool clipboard READ clipboard WRITE setClipboard NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardWinV READ clipboardWinV WRITE setClipboardWinV NOTIFY changed FINAL)
    // Where taking Win+V over stands (set by the app): off | on | waiting
    // (Explorer still holds it: restart Explorer) | releasing (Explorer gets
    // it back once it restarts) | failed (the registry could not be written).
    Q_PROPERTY(QString winVState READ winVState NOTIFY winVStateChanged FINAL)
    Q_PROPERTY(bool canRestartExplorer READ canRestartExplorer NOTIFY winVStateChanged FINAL)
    Q_PROPERTY(QString clipboardHotkey READ clipboardHotkey WRITE setClipboardHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString clipboardHotkeyError READ clipboardHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(int clipboardMaxItems READ clipboardMaxItems WRITE setClipboardMaxItems NOTIFY changed FINAL)
    Q_PROPERTY(int clipboardMaxDays READ clipboardMaxDays WRITE setClipboardMaxDays NOTIFY changed FINAL)
    Q_PROPERTY(QString clipboardMissingFiles READ clipboardMissingFiles WRITE setClipboardMissingFiles NOTIFY changed FINAL) // keep | hide | clean
    Q_PROPERTY(bool clipboardImages READ clipboardImages WRITE setClipboardImages NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardClickToPaste READ clipboardClickToPaste WRITE setClipboardClickToPaste NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardLargePictures READ clipboardLargePictures WRITE setClipboardLargePictures NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardKeepPlace READ clipboardKeepPlace WRITE setClipboardKeepPlace NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardFlipAbove READ clipboardFlipAbove WRITE setClipboardFlipAbove NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardHoldToStep READ clipboardHoldToStep WRITE setClipboardHoldToStep NOTIFY changed FINAL)
    Q_PROPERTY(QString clipboardSearchBoxSide READ clipboardSearchBoxSide WRITE setClipboardSearchBoxSide NOTIFY changed FINAL) // near | far
    Q_PROPERTY(QStringList clipboardExcludedApps READ clipboardExcludedApps NOTIFY changed FINAL)
    Q_PROPERTY(int clipboardCount READ clipboardCount NOTIFY clipboardCountChanged FINAL)
    Q_PROPERTY(bool clipboardImporting READ clipboardImporting NOTIFY clipboardImportChanged FINAL)
    Q_PROPERTY(QString clipboardImportMessage READ clipboardImportMessage NOTIFY clipboardImportChanged FINAL)
    Q_PROPERTY(QString clipboardNumberKeys READ clipboardNumberKeys WRITE setClipboardNumberKeys NOTIFY changed FINAL)
    Q_PROPERTY(QString clipboardNumberKeysNote READ clipboardNumberKeysNote NOTIFY numberKeysNoteChanged FINAL)
    Q_PROPERTY(QString clipboardQuickPaste READ clipboardQuickPaste WRITE setClipboardQuickPaste NOTIFY changed FINAL)
    Q_PROPERTY(QString clipboardQuickPasteError READ clipboardQuickPasteError NOTIFY hotkeyErrorChanged FINAL)
    // Optional Windows search shortcuts (taskbar::)
    Q_PROPERTY(bool taskbarWinS READ taskbarWinS WRITE setTaskbarWinS NOTIFY changed FINAL)
    Q_PROPERTY(QString winSState READ winSState NOTIFY winSStateChanged FINAL) // as winVState, for Win+S
    Q_PROPERTY(bool taskbarStartTyping READ taskbarStartTyping WRITE setTaskbarStartTyping NOTIFY changed FINAL)
    // Win顺's own search box on the taskbar: the setting, whether Windows has
    // the taskbar for it (11), whether it is on the taskbar now (not where
    // the icons leave no room for it).
    Q_PROPERTY(bool taskbarSearchBox READ taskbarSearchBox WRITE setTaskbarSearchBox NOTIFY changed FINAL)
    Q_PROPERTY(QString taskbarSearchBoxStyle READ taskbarSearchBoxStyle WRITE setTaskbarSearchBoxStyle NOTIFY changed FINAL) // box | label | icon
    Q_PROPERTY(bool searchBoxSupported READ searchBoxSupported CONSTANT FINAL)
    Q_PROPERTY(bool searchBoxShown READ searchBoxShown NOTIFY searchBoxShownChanged FINAL)
    Q_PROPERTY(QString searchBoxHidden READ searchBoxHidden NOTIFY searchBoxShownChanged FINAL)
    // Read again as the window comes to the front (refreshTaskbarState): the
    // user pins the button and hides Windows' search outside Win顺.
    Q_PROPERTY(bool taskbarButtonPinned READ taskbarButtonPinned NOTIFY taskbarStateChanged FINAL)
    Q_PROPERTY(bool windowsSearchShown READ windowsSearchShown NOTIFY taskbarStateChanged FINAL)
    // 网页搜索: for each, {keyword, name, url, home, searches, shownUrl, shownHome, homeUrl}
    Q_PROPERTY(QVariantList webShortcuts READ webShortcuts NOTIFY changed FINAL)
    // The kinds of file 文件 narrows to: [{name, extensions: [..]}], named
    // in the language shown; fileTypesDefault while they are as they came.
    Q_PROPERTY(QVariantList fileTypes READ fileTypes NOTIFY changed FINAL)
    Q_PROPERTY(bool fileTypesDefault READ fileTypesDefault NOTIFY changed FINAL)
    Q_PROPERTY(QString folderKeyword READ folderKeyword NOTIFY changed FINAL)
    // Counts the sites' icons that arrived: a binding on webIconUrl() reads it
    // to be asked again (without the list made anew, and a form open in it lost).
    Q_PROPERTY(int siteIconsArrived READ siteIconsArrived NOTIFY siteIconArrived FINAL)
    // 网络共享: for each, {path, name, tone (ok | busy | idle | warn | error),
    // status (where it stands, said for the user), signIn (it needs a user
    // name and password), suspect (a pass found much less: accept it?), busy
    // (being read), content (its files' contents indexed too, GitHub #114),
    // contentStatus (how far those are read; empty while off)}. As the app
    // reports (setShareStatuses, setShareContentStatuses).
    Q_PROPERTY(QVariantList shares READ shares NOTIFY sharesChanged FINAL)
    // Shares the user opened in File Explorer or mapped, not added yet: [{path, drive}]
    Q_PROPERTY(QVariantList knownShares READ knownShares NOTIFY sharesChanged FINAL)
    Q_PROPERTY(QVariantList wslFolders READ wslFolders NOTIFY sharesChanged FINAL)
    Q_PROPERTY(QString renderer READ renderer WRITE setRenderer NOTIFY changed FINAL)
    Q_PROPERTY(QString proxyMode READ proxyMode WRITE setProxyMode NOTIFY changed FINAL) // system | none | manual
    Q_PROPERTY(QString proxyAddress READ proxyAddress WRITE setProxyAddress NOTIFY changed FINAL)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY changed FINAL)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY changed FINAL)
    Q_PROPERTY(QString transparency READ transparency WRITE setTransparency NOTIFY changed FINAL)
    Q_PROPERTY(QString taskbarIcon READ taskbarIcon WRITE setTaskbarIcon NOTIFY changed FINAL) // color | mono
    Q_PROPERTY(bool searchLogo READ searchLogo WRITE setSearchLogo NOTIFY changed FINAL)
    Q_PROPERTY(QString uiSize READ uiSize WRITE setUiSize NOTIFY changed FINAL) // compact | standard | roomy
    Q_PROPERTY(QString corners READ corners WRITE setCorners NOTIFY changed FINAL) // round | square
    Q_PROPERTY(bool autoUpdate READ autoUpdate WRITE setAutoUpdate NOTIFY changed FINAL)
    Q_PROPERTY(QString backupFolder READ backupFolder NOTIFY changed FINAL) // empty: no backups (see Backup)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed FINAL)
    Q_PROPERTY(bool isDefault READ isDefault NOTIFY changed FINAL)
    Q_PROPERTY(QString dataFolder READ dataFolder CONSTANT FINAL)
    Q_PROPERTY(QString diagnosticsStatus READ diagnosticsStatus NOTIFY diagnosticsChanged FINAL)
    Q_PROPERTY(bool diagnosticsBusy READ diagnosticsBusy NOTIFY diagnosticsChanged FINAL)
    // 索引位置 (set by the app): where the index files are, and how moving them goes
    Q_PROPERTY(QString indexFolder READ indexFolder NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(bool indexFolderIsDefault READ indexFolderIsDefault NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(QString indexSize READ indexSize NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(int indexMoveProgress READ indexMoveProgress NOTIFY indexFolderChanged FINAL) // -1: not moving
    Q_PROPERTY(QString indexFolderProblem READ indexFolderProblem NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(QString indexFolderNote READ indexFolderNote NOTIFY indexFolderChanged FINAL)
    // AI 助手 (the MCP server, src/mcp): the switches, the folders AI never
    // sees, who is connected (set by the app), the clients found on this PC
    // and how the last connection made went (McpClients).
    Q_PROPERTY(bool mcp READ mcp WRITE setMcp NOTIFY changed FINAL)
    Q_PROPERTY(bool mcpAllowOpen READ mcpAllowOpen WRITE setMcpAllowOpen NOTIFY changed FINAL)
    Q_PROPERTY(bool mcpClipboard READ mcpClipboard WRITE setMcpClipboard NOTIFY changed FINAL)
    Q_PROPERTY(bool mcpScreenshot READ mcpScreenshot WRITE setMcpScreenshot NOTIFY changed FINAL) // take_screenshot
    Q_PROPERTY(bool mcpPlugins READ mcpPlugins WRITE setMcpPlugins NOTIFY changed FINAL) // the plugins' commands as tools
    // change_settings: "ask" (each change asked of the user), "auto" (made at once, the user told) or "off"
    Q_PROPERTY(QString mcpSettings READ mcpSettings WRITE setMcpSettings NOTIFY changed FINAL)
    Q_PROPERTY(bool mcpPluginActions READ mcpPluginActions WRITE setMcpPluginActions NOTIFY changed FINAL) // and their actions
    Q_PROPERTY(QStringList mcpExcludedPaths READ mcpExcludedPaths NOTIFY changed FINAL) // expanded, for display
    Q_PROPERTY(QString mcpStatus READ mcpStatus NOTIFY mcpStatusChanged FINAL)
    Q_PROPERTY(QVariantList mcpClients READ mcpClients NOTIFY mcpClientsChanged FINAL)
    Q_PROPERTY(QString mcpRelayPath READ mcpRelayPath NOTIFY mcpClientsChanged FINAL) // empty: WinShunMcp.exe is missing
    Q_PROPERTY(QString mcpMessage READ mcpMessage NOTIFY mcpMessageChanged FINAL)
    // 插件: the page's own view-model (PluginSettings), which edits the
    // [Plugin.<id>] sections here.
    Q_PROPERTY(ws::PluginSettings* plugins READ plugins CONSTANT FINAL)
    // 收藏夹: the page's own view-model (FavoritesSettings), on the app's
    // favorites (not in the INI: 恢复默认设置 leaves them).
    Q_PROPERTY(ws::FavoritesSettings* favorites READ favorites CONSTANT FINAL)

public:
    SettingsEditor(const Settings& settings, const QString& runningRenderer, QObject* parent = nullptr);
    ~SettingsEditor() override;

    bool doubleCtrl() const { return m_settings.doubleCtrl; }
    void setDoubleCtrl(bool on);
    QString doubleTapKey() const { return m_settings.doubleTapKey; }
    void setDoubleTapKey(const QString& key);
    QString doubleCtrlKey() const { return m_settings.doubleCtrlKey; }
    void setDoubleCtrlKey(const QString& key);
    QString doubleCtrlClash() const { return m_doubleCtrlClash; }
    Q_INVOKABLE void refreshDoubleCtrlClash();
    bool doubleCtrlPauseInGames() const { return m_settings.doubleCtrlPauseInGames; }
    void setDoubleCtrlPauseInGames(bool on);
    bool doubleCtrlPauseInFullScreen() const { return m_settings.doubleCtrlPauseInFullScreen; }
    void setDoubleCtrlPauseInFullScreen(bool on);
    QStringList doubleCtrlExcludedApps() const { return m_settings.doubleCtrlExcludedApps; }
    QString hotkey() const { return m_settings.hotkey; }
    QString shortcutAgain() const { return m_settings.shortcutAgain; }
    void setShortcutAgain(const QString& what);
    void setHotkey(const QString& hotkey);
    QString hotkeyError() const { return m_hotkeyError; }
    void setHotkeyError(const QString& error);
    bool recordingHotkey() const { return m_recordingHotkey; }
    void setRecordingHotkey(bool recording);
    bool autostart() const;
    void setAutostart(bool on);
    void refreshAutostart(); // changed or read again elsewhere (the tray menu, the task itself)
    bool recordHistory() const { return m_settings.recordHistory; }
    void setRecordHistory(bool on);
    int historyCount() const { return m_historyCount; }
    void setHistoryCount(int count); // set by the app
    bool launcherWidthSet() const { return m_launcherWidthSet; }
    void setLauncherWidthSet(bool set); // ... too
    bool dialogJump() const { return m_settings.dialogJump; }
    void setDialogJump(bool on);
    bool dialogBar() const { return m_settings.dialogBar; }
    void setDialogBar(bool on);
    QString dialogBarPlace() const { return m_settings.dialogBarPlace; }
    void setDialogBarPlace(const QString& place);
    bool dialogAutoJump() const { return m_settings.dialogAutoJump; }
    void setDialogAutoJump(bool on);
    QString dialogAutoJumpWhen() const { return m_settings.dialogAutoJumpWhen; }
    void setDialogAutoJumpWhen(const QString& when); // "open", "back"
    QStringList dialogBarExcludedApps() const { return m_settings.dialogBarExcludedApps; }
    QString fileManager() const { return m_settings.fileManager; }
    void setFileManager(const QString& manager);
    QStringList fileManagers() const { return m_fileManagers; }
    QString fileManagerOpenCommand() const { return m_settings.fileManagerOpenCommand; }
    void setFileManagerOpenCommand(const QString& command);
    QString fileManagerRevealCommand() const { return m_settings.fileManagerRevealCommand; }
    void setFileManagerRevealCommand(const QString& command);
    QString fileManagerProblem() const;
    // Asks for a program: a command line running it with {path}, to fill a
    // command in with; empty when none was picked.
    Q_INVOKABLE QString pickFileManagerProgram();
    bool explorerMenu() const { return m_settings.explorerMenu; }
    void setExplorerMenu(bool on);
    bool explorerTyping() const { return m_settings.explorerTyping; }
    void setExplorerTyping(bool on);
    bool opusTyping() const { return m_settings.opusTyping; }
    void setOpusTyping(bool on);
    QString folderTyping() const { return m_settings.folderTyping; }
    void setFolderTyping(const QString& how);
    bool folderMenu() const { return m_settings.folderMenu != u"off"; }
    void setFolderMenu(bool on);
    bool folderMenuExplorer() const { return m_settings.folderMenuExplorer; }
    void setFolderMenuExplorer(bool on);
    bool folderMenuDesktop() const { return m_settings.folderMenuDesktop; }
    void setFolderMenuDesktop(bool on);
    bool folderMenuFavorites() const { return m_settings.folderMenuFavorites; }
    void setFolderMenuFavorites(bool on);
    bool folderMenuRecent() const { return m_settings.folderMenuRecent; }
    void setFolderMenuRecent(bool on);
    bool folderMenuWindows() const { return m_settings.folderMenuWindows; }
    void setFolderMenuWindows(bool on);
    QString folderMenuHotkey() const { return m_settings.folderMenuHotkey; }
    bool dropPie() const { return m_settings.dropPie; }
    void setDropPie(bool on);
    void setFolderMenuHotkey(const QString& hotkey);
    QString folderMenuHotkeyError() const { return m_folderMenuHotkeyError; }
    void setFolderMenuHotkeyError(const QString& error);
    QString captureLastPinHotkey() const { return m_settings.captureLastPinHotkey; }
    void setCaptureLastPinHotkey(const QString& value);
    QString captureSaveFolder() const { return m_settings.captureSaveFolder; }
    void setCaptureSaveFolder(const QString& value);
    QString captureRecordingFolder() const { return m_settings.captureRecordingFolder; }
    void setCaptureRecordingFolder(const QString& value);
    bool captureRecordingSystemAudio() const { return m_settings.captureRecordingSystemAudio; }
    void setCaptureRecordingSystemAudio(bool value);
    bool captureRecordingMicrophone() const { return m_settings.captureRecordingMicrophone; }
    void setCaptureRecordingMicrophone(bool value);
    int captureRecordingQuality() const { return m_settings.captureRecordingQuality; }
    void setCaptureRecordingQuality(int value);
    bool captureRecordingCursor() const { return m_settings.captureRecordingCursor; }
    void setCaptureRecordingCursor(bool value);
    bool captureRecordingCountdown() const { return m_settings.captureRecordingCountdown; }
    void setCaptureRecordingCountdown(bool value);
    bool captureRecordingClicks() const { return m_settings.captureRecordingClicks; }
    void setCaptureRecordingClicks(bool value);
    QString captureLastPinHotkeyError() const { return m_captureLastPinHotkeyError; }
    void setCaptureLastPinHotkeyError(const QString& error);
    bool captureEnabled() const { return m_settings.captureEnabled; }
    void setCaptureEnabled(bool on);
    QString captureHotkey() const { return m_settings.captureHotkey; }
    void setCaptureHotkey(const QString& hotkey);
    QString captureHotkeyError() const { return m_captureHotkeyError; }
    void setCaptureHotkeyError(const QString& error);
    bool capturePrintScreen() const { return m_settings.capturePrintScreen; }
    void setCapturePrintScreen(bool on);
    bool captureCopyOnSelect() const { return m_settings.captureCopyOnSelect; }
    void setCaptureCopyOnSelect(bool on);
    bool captureMagnifier() const { return m_settings.captureMagnifier; }
    void setCaptureMagnifier(bool on);
    bool capturePinOnCopy() const { return m_settings.capturePinOnCopy; }
    void setCapturePinOnCopy(bool on);
    // What the mouse buttons do after a selection (Settings::captureMouse*).
    QString captureMouseDoubleLeft() const { return m_settings.captureMouseDoubleLeft; }
    void setCaptureMouseDoubleLeft(const QString& action) { setCaptureMouse(m_settings.captureMouseDoubleLeft, action, false); }
    QString captureMouseRight() const { return m_settings.captureMouseRight; }
    void setCaptureMouseRight(const QString& action) { setCaptureMouse(m_settings.captureMouseRight, action, true); }
    QString captureMouseMiddle() const { return m_settings.captureMouseMiddle; }
    void setCaptureMouseMiddle(const QString& action) { setCaptureMouse(m_settings.captureMouseMiddle, action, false); }
    QString captureMouseBack() const { return m_settings.captureMouseBack; }
    void setCaptureMouseBack(const QString& action) { setCaptureMouse(m_settings.captureMouseBack, action, false); }
    QString captureMouseForward() const { return m_settings.captureMouseForward; }
    void setCaptureMouseForward(const QString& action) { setCaptureMouse(m_settings.captureMouseForward, action, false); }
    QString captureToolbarHidden() const { return m_settings.captureToolbarHidden; }
    void setCaptureToolbarHidden(const QString& ids);
    bool captureCursor() const { return m_settings.captureCursor; }
    void setCaptureCursor(bool on);
    bool captureHdrFile() const { return m_settings.captureHdrFile; }
    void setCaptureHdrFile(bool on);
    bool captureControlsUia() const { return m_settings.captureControlsUia; }
    void setCaptureControlsUia(bool on);
    int captureDelay() const { return m_settings.captureDelay; }
    void setCaptureDelay(int seconds);
    QString capturePinDoubleClick() const { return m_settings.capturePinDoubleClick; }
    void setCapturePinDoubleClick(const QString& action);
    QString capturePinsHotkey() const { return m_settings.capturePinsHotkey; }
    void setCapturePinsHotkey(const QString& hotkey);
    QString capturePinsRestoreHotkey() const { return m_settings.capturePinsRestoreHotkey; }
    void setCapturePinsRestoreHotkey(const QString& hotkey);
    QString capturePinsHotkeyError() const { return m_capturePinsHotkeyError; }
    QString capturePinsRestoreHotkeyError() const { return m_capturePinsRestoreHotkeyError; }
    void setCapturePinsHotkeyError(const QString& error);
    void setCapturePinsRestoreHotkeyError(const QString& error);
    bool captureSmartPrescan() const { return m_settings.captureSmartPrescan; }
    void setCaptureSmartPrescan(bool on);
    bool captureSmartIpMac() const { return m_settings.captureSmartIpMac; }
    void setCaptureSmartIpMac(bool on);
    bool captureSmartAllText() const { return m_settings.captureSmartAllText; }
    void setCaptureSmartAllText(bool on);
    QString captureSmartWords() const { return m_settings.captureSmartWords; }
    void setCaptureSmartWords(const QString& words);
    QString captureNote() const { return m_captureNote; }
    void setCaptureNote(const QString& note);
    QString captureKeyword() const { return m_settings.captureKeyword; }
    bool pasteFileEnabled() const { return m_settings.pasteFileEnabled; }
    void setPasteFileEnabled(bool on);
    bool pasteFileCtrlV() const { return m_settings.pasteFileCtrlV; }
    void setPasteFileCtrlV(bool on);
    bool pasteFileMenu() const { return m_settings.pasteFileMenu; }
    void setPasteFileMenu(bool on);
    // Saved if no web search or favorite has it (they keep theirs; a
    // plugin's alike gives way): empty, else what is wrong.
    Q_INVOKABLE QString saveCaptureKeyword(const QString& keyword);
    // The keyword is the screenshot history's (the capture on): what a web
    // search or a favorite given it is told; empty when it is free.
    QString captureKeywordTaken(const QString& keyword) const;
    Q_INVOKABLE void refreshFileManagers();
    // The command Directory Opus runs for a double click on blank space, to
    // bring up the folder menu there (FolderMenu::requested): for its
    // Preferences > File Displays > Mouse > Background Events.
    Q_INVOKABLE QString opusFolderMenuCommand() const;
    Q_INVOKABLE void openOpusPreferences() const;
    QString opusFolderMenu() const { return m_opusFolderMenu; }
    QString opusFolderMenuTaken() const { return m_opusFolderMenuTaken; }
    QString opusFolderMenuProblem() const { return m_opusFolderMenuProblem; }
    bool opusRunning() const { return m_opusRunning; }
    Q_INVOKABLE void refreshOpusFolderMenu();
    // Puts that command into Opus's settings (what was there comes back with
    // `on` false), closing and starting again an Opus that runs.
    Q_INVOKABLE void setOpusFolderMenu(bool on);
    bool includeRemovableDrives() const { return m_settings.includeRemovableDrives; }
    void setIncludeRemovableDrives(bool on);
    bool rescanOnStartup() const { return m_settings.rescanOnStartup; }
    void setRescanOnStartup(bool on);
    QString indexAccess() const { return m_settings.indexAccess; }
    void setIndexAccess(const QString& mode); // "auto", "service", "direct" (administrators only), "walk"
    struct IndexAccessState {
        QString effective; // direct | service | walk: the way in use
        QString status; // what is in use, or why not what was asked for, said for the user
        QString note; // worth reading: the mode taken from an old task, the task's level not the mode's ...
        bool elevated = false; // Win顺 runs as administrator now
        bool administrator = false; // the account is an administrator's (direct means something)
        bool portable = false; // a portable copy: the service is installed from here
        bool serviceInstalled = false;
        QString serviceState; // the SCM's word: not_installed | disabled | stopped | starting | running | unknown
        bool serviceInstallable = false; // the account can install it (an administrator's); else it is not offered (GitHub #52)
        bool serviceUnsafeLocation = false; // installed from a user-writable folder (an old portable copy): offer to repair
        bool serviceBusy = false; // --install / --uninstall running
        QString serviceMessage; // how the last install or uninstall went
        bool foldersInEffect = false; // only the chosen folders are indexed (App::foldersInEffect)
        bool foldersPossible = true; // the disks are walked: choosing folders takes effect
        QString scopeStatus; // what is indexed now, said for the user
        bool operator==(const IndexAccessState&) const = default;
    };
    void setIndexAccessState(const IndexAccessState& state); // set by the app
    QString indexAccessEffective() const { return m_indexAccessState.effective; }
    QString indexAccessStatus() const { return m_indexAccessState.status; }
    QString indexAccessNote() const { return m_indexAccessState.note; }
    bool isAdministrator() const { return m_indexAccessState.administrator; }
    bool isElevated() const { return m_indexAccessState.elevated; }
    bool isPortable() const { return m_indexAccessState.portable; }
    bool indexServiceInstalled() const { return m_indexAccessState.serviceInstalled; }
    bool indexServiceBusy() const { return m_indexAccessState.serviceBusy; }
    QString indexServiceState() const { return m_indexAccessState.serviceState; }
    bool indexServiceInstallable() const { return m_indexAccessState.serviceInstallable; }
    bool indexServiceUnsafeLocation() const { return m_indexAccessState.serviceUnsafeLocation; }
    QString indexServiceMessage() const { return m_indexAccessState.serviceMessage; }
    // The app restarts Win顺 as administrator (UAC asks once), installs or
    // uninstalls the index service (WinShunService.exe, elevated, UAC once).
    Q_INVOKABLE void restartAsAdministrator() { emit elevatedRestartRequested(); }
    Q_INVOKABLE void installIndexService() { emit indexServiceInstallRequested(true); }
    Q_INVOKABLE void uninstallIndexService() { emit indexServiceInstallRequested(false); }
    QString indexScope() const { return m_settings.indexScope; }
    bool allDrives() const { return !m_settings.selectedDrives; }
    void setAllDrives(bool all);
    QVariantList drives() const;
    Q_INVOKABLE void setDriveSelected(const QString& root, bool selected);
    Q_INVOKABLE void refreshDrives() { emit drivesChanged(); }
    void setIndexScope(const QString& scope); // "auto", "drives", "folders"
    bool foldersInEffect() const { return m_indexAccessState.foldersInEffect; }
    bool foldersPossible() const { return m_indexAccessState.foldersPossible; }
    QString indexScopeStatus() const { return m_indexAccessState.scopeStatus; }
    int folderVerifyHours() const { return m_settings.folderVerifyHours; }
    void setFolderVerifyHours(int hours); // 0: only at startup
    QVariantList folders() const;
    QVariantList commonFolders() const;
    void setFolderStatuses(std::vector<IndexService::FolderStatus> statuses); // set by the app
    // The language changed: the folders (Windows' own names, their states) in the new one.
    void retranslateFolders();
    // Adds a folder to index: a path, or a file:/// URL (dropped). Returns
    // why it cannot be ("已经包含在 C:\a 里", a network place: 网络共享's),
    // or an empty string once it is. One that holds folders added takes
    // their place; a drive's root is that whole drive.
    Q_INVOKABLE QString addFolder(const QString& text);
    // The same on `settings` alone, for the launcher's "加入索引" when no
    // settings window is open: `added` says it went in (else the text is
    // returned as the reason, or is empty for a path that came to nothing).
    // The caller saves.
    static QString addFolderTo(Settings& settings, const QString& text, bool& added);
    Q_INVOKABLE void removeFolder(int index);
    Q_INVOKABLE void verifyFolder(int index); // walked again now
    Q_INVOKABLE bool isNetworkFolder(const QString& text) const; // a share's place (path or file:// URL)
    Q_INVOKABLE QString localPath(const QString& text) const; // a path or a file:/// URL, with native separators
    Q_INVOKABLE QString pickFolder(const QString& title); // asks for one; empty when none was chosen
    QStringList excludedPaths() const;
    QStringList excludedNames() const { return m_settings.excludedNames; }
    QStringList excludedTypes() const { return m_settings.excludedTypes; }
    QStringList excludedFileNames() const { return m_settings.excludedFileNames; }
    bool contentSearch() const { return m_settings.contentSearch; }
    void setContentSearch(bool on);
    QStringList contentExtensions() const { return m_settings.contentExtensions; }
    QList<double> contentMaxSizeMB() const { return {m_settings.contentMaxSizeMB.begin(), m_settings.contentMaxSizeMB.end()}; }
    Q_INVOKABLE void setContentMaxSizeMB(int kind, double mb);
    QStringList contentKindExtensions() const;
    bool contentIndex() const { return m_settings.contentIndex; }
    void setContentIndex(bool on);
    bool contentInLowPriority() const { return m_settings.contentInLowPriority; }
    void setContentInLowPriority(bool on);
    bool contentDocuments() const { return m_settings.contentDocuments; }
    void setContentDocuments(bool on);
    QString contentIndexStatus() const { return m_contentIndexStatus; }
    void setContentIndexStatus(const QString& status); // "已收录 … 个文件"; set by the app
    QString renderer() const { return m_settings.renderer; }
    void setRenderer(const QString& renderer);
    QString proxyMode() const { return m_settings.proxyMode; }
    void setProxyMode(const QString& mode);
    QString proxyAddress() const { return m_settings.proxyAddress; }
    void setProxyAddress(const QString& address); // as typed, trimmed; a bad one is kept and not used (see proxyProblem)
    // Why `address` cannot be a proxy, in plain words; empty when it can.
    Q_INVOKABLE QString proxyProblem(const QString& address) const;
    QString theme() const { return m_settings.theme; }
    void setTheme(const QString& theme);
    QString language() const { return m_settings.language; }
    void setLanguage(const QString& language);
    QString transparency() const { return m_settings.transparency; }
    void setTransparency(const QString& transparency);
    QString taskbarIcon() const { return m_settings.taskbarIcon; }
    void setTaskbarIcon(const QString& style);
    bool searchLogo() const { return m_settings.searchLogo; }
    void setSearchLogo(bool on);
    QString uiSize() const { return m_settings.uiSize; }
    void setUiSize(const QString& size);
    QString corners() const { return m_settings.corners; }
    void setCorners(const QString& corners);
    bool autoUpdate() const { return m_settings.autoUpdate; }
    QString backupFolder() const { return QDir::toNativeSeparators(m_settings.backupFolder); }
    void setAutoUpdate(bool on);
    bool restartRequired() const { return Settings::resolveRenderer(m_settings.renderer) != m_runningRenderer; }
    bool isDefault() const;
    QString dataFolder() const { return Settings::dataDir(); }
    QString diagnosticsStatus() const { return m_diagnosticsStatus; }
    bool diagnosticsBusy() const { return m_diagnosticsBusy; }
    void finishDiagnosticsExport(const QString& error);

    // 索引位置, as the app has it.
    struct IndexFolderState {
        QString folder; // where the index files are, with native separators
        bool isDefault = true; // that is Settings::dataDir()
        QString size; // of the files there, "420 MB"
        int moveProgress = -1; // percent, while they are being moved
        QString problem; // why they could not be moved, or are not where the settings say
        QString note; // worth knowing, nothing wrong: they are on a hard disk
        bool operator==(const IndexFolderState&) const = default;
    };
    void setIndexFolderState(const IndexFolderState& state);
    QString indexFolder() const { return m_indexFolderState.folder; }
    bool indexFolderIsDefault() const { return m_indexFolderState.isDefault; }
    QString indexSize() const { return m_indexFolderState.size; }
    int indexMoveProgress() const { return m_indexFolderState.moveProgress; }
    QString indexFolderProblem() const { return m_indexFolderState.problem; }
    QString indexFolderNote() const { return m_indexFolderState.note; }

    bool clipboard() const { return m_settings.clipboard; }
    void setClipboard(bool on);
    bool clipboardWinV() const { return m_settings.clipboardWinV; }
    void setClipboardWinV(bool on);
    QString winVState() const { return m_winVState; }
    void setWinVState(const QString& state, bool canRestartExplorer); // set by the app
    bool canRestartExplorer() const { return m_canRestartExplorer; }
    QString clipboardHotkey() const { return m_settings.clipboardHotkey; }
    void setClipboardHotkey(const QString& hotkey);
    QString clipboardHotkeyError() const { return m_clipboardHotkeyError; }
    void setClipboardHotkeyError(const QString& error);
    int clipboardMaxItems() const { return m_settings.clipboardMaxItems; }
    void setClipboardMaxItems(int items);
    int clipboardMaxDays() const { return m_settings.clipboardMaxDays; }
    void setClipboardMaxDays(int days);
    QString clipboardMissingFiles() const { return m_settings.clipboardMissingFiles; }
    void setClipboardMissingFiles(const QString& mode);
    bool clipboardImages() const { return m_settings.clipboardImages; }
    void setClipboardImages(bool on);
    bool clipboardClickToPaste() const { return m_settings.clipboardClickToPaste; }
    void setClipboardClickToPaste(bool on);
    bool clipboardLargePictures() const { return m_settings.clipboardLargePictures; }
    void setClipboardLargePictures(bool on);
    bool clipboardKeepPlace() const { return m_settings.clipboardKeepPlace; }
    void setClipboardKeepPlace(bool on);
    bool clipboardFlipAbove() const { return m_settings.clipboardFlipAbove; }
    void setClipboardFlipAbove(bool on);
    bool clipboardHoldToStep() const { return m_settings.clipboardHoldToStep; }
    void setClipboardHoldToStep(bool on);
    QString clipboardSearchBoxSide() const { return m_settings.clipboardSearchBoxSide; }
    void setClipboardSearchBoxSide(const QString& side);
    QStringList clipboardExcludedApps() const { return m_settings.clipboardExcludedApps; }
    int clipboardCount() const { return m_clipboardCount; }
    void setClipboardCount(int count); // set by the app
    QString clipboardNumberKeys() const { return m_settings.clipboardNumberKeys; }
    void setClipboardNumberKeys(const QString& keys);
    QString clipboardNumberKeysNote() const { return m_clipboardNumberKeysNote; }
    QString clipboardQuickPaste() const { return m_settings.clipboardQuickPaste; }
    void setClipboardQuickPaste(const QString& keys);
    QString clipboardQuickPasteError() const { return m_clipboardQuickPasteError; }
    void setClipboardQuickPasteError(const QString& error);

    bool foldersFirst() const { return m_settings.foldersFirst; }
    void setFoldersFirst(bool on);
    QString preferredTypes() const { return m_settings.preferredTypes.join(u"; "); }
    void setPreferredTypes(const QString& text);
    bool preferredTypesDefault() const { return m_settings.preferredTypes == Settings::defaultPreferredTypes(); }
    Q_INVOKABLE void resetPreferredTypes();
    bool showModified() const { return m_settings.showModified; }
    void setShowModified(bool on);
    bool showModifiedTime() const { return m_settings.showModifiedTime; }
    void setShowModifiedTime(bool on);
    bool showFileSize() const { return m_settings.showFileSize; }
    void setShowFileSize(bool on);
    bool doubleClickToOpen() const { return m_settings.doubleClickToOpen; }
    void setDoubleClickToOpen(bool on);
    bool tabEntersFavoriteFolder() const { return m_settings.tabEntersFavoriteFolder; }
    void setTabEntersFavoriteFolder(bool on);
    bool thumbnails() const { return m_settings.thumbnails; }
    void setThumbnails(bool on);
    bool webSuggestions() const { return m_settings.webSuggestions; }
    void setWebSuggestions(bool on);
    QString sort() const { return m_settings.sort; }
    void setSort(const QString& sort);
    QString startList() const { return m_settings.startList; }
    void setStartList(const QString& list);
    bool clearOnOpen() const { return m_settings.clearOnOpen; }
    void setClearOnOpen(bool on);
    QString numberKeys() const { return m_settings.numberKeys; }
    void setNumberKeys(const QString& keys);
    QString numberHints() const { return m_settings.numberHints; }
    void setNumberHints(const QString& hints);
    QString numberKeysNote() const { return m_numberKeysNote; }
    QString previewWith() const { return m_settings.previewWith; }
    void setPreviewWith(const QString& with);
    // Looks again at which number keys are taken (as the window comes to the
    // front: a program may have been started or closed meanwhile).
    Q_INVOKABLE void refreshNumberKeys();

    bool taskbarWinS() const { return m_settings.taskbarWinS; }
    void setTaskbarWinS(bool on);
    QString winSState() const { return m_winSState; }
    void setWinSState(const QString& state, bool canRestartExplorer); // set by the app
    bool taskbarStartTyping() const { return m_settings.taskbarStartTyping; }
    void setTaskbarStartTyping(bool on);
    bool taskbarSearchBox() const { return m_settings.taskbarSearchBox; }
    void setTaskbarSearchBox(bool on);
    QString taskbarSearchBoxStyle() const { return m_settings.taskbarSearchBoxStyle; }
    void setTaskbarSearchBoxStyle(const QString& style);
    bool searchBoxSupported() const;
    bool searchBoxShown() const { return m_searchBoxHidden.isEmpty(); }
    QString searchBoxHidden() const { return m_searchBoxHidden; } // why not: "pending", "noroom"... (App)
    void setSearchBoxShown(const QString& hidden); // set by the app: empty when shown
    // Set by the app: the box measured Windows 10's taskbar (StartAllBack,
    // ExplorerPatcher), where Windows' search is another thing (refreshTaskbarState).
    void setClassicTaskbar(bool classic);
    bool taskbarButtonPinned() const { return m_taskbarButtonPinned; }
    bool windowsSearchShown() const { return m_windowsSearchShown; }
    Q_INVOKABLE void refreshTaskbarState();
    // Has the taskbar button ask Windows to pin it (WinShunSearch.exe --pin):
    // the user says yes in Windows' own dialog. Where Windows does not ask,
    // the button's Start menu shortcut is selected in File Explorer for the
    // user to pin. False when there is no button program.
    Q_INVOKABLE bool pinTaskbarButton();
    // The Start menu shortcut to the taskbar button, selected in Explorer for
    // the user to pin. False when there is none.
    Q_INVOKABLE bool showTaskbarButton();

    QVariantList webShortcuts() const;
    QVariantList fileTypes() const;
    bool fileTypesDefault() const { return m_settings.fileTypes == Settings::defaultFileTypes() && m_settings.folderKeyword == u"folder"; }
    // Each returns what is wrong ("已有这个类型"), empty when done.
    Q_INVOKABLE QString addFileType(const QString& name, const QString& extensions);
    Q_INVOKABLE QString renameFileType(int index, const QString& name);
    Q_INVOKABLE QString setFileTypeExtensions(int index, const QString& text); // "jpg, .PNG *.gif"
    Q_INVOKABLE QString setFileTypeKeyword(int index, const QString& text); // -1: folders
    QString folderKeyword() const { return m_settings.folderKeyword; }
    Q_INVOKABLE void removeFileType(int index);
    Q_INVOKABLE void moveFileType(int index, int delta); // up (-1) or down (1) the strip
    Q_INVOKABLE void resetFileTypes();

    QVariantList shares() const;
    QVariantList knownShares() const;
    QVariantList wslFolders() const;
    Q_INVOKABLE bool isWslPath(const QString& text) const;
    Q_INVOKABLE void startWslShare(int index);
    void setShareStatuses(std::vector<ShareIndexer::Status> statuses); // set by the app
    void setShareContentStatuses(std::vector<remotecontent::Indexer::Status> statuses); // set by the app
    Q_INVOKABLE void setShareContent(int index, bool on); // its files' contents indexed too (GitHub #114)
    // "\\host", "\\host\": a server, whose shares can be listed; its name, else empty.
    Q_INVOKABLE QString serverOf(const QString& text) const;
    // Adds a share typed or chosen ("\\host\share[\folder]", or a mapped "Z:\...").
    // Returns why it cannot be, or an empty string once it is.
    Q_INVOKABLE QString addShare(const QString& text);
    Q_INVOKABLE void removeShare(int index);
    Q_INVOKABLE void verifyShare(int index);
    Q_INVOKABLE void signInToShare(int index);
    Q_INVOKABLE void acceptShareMissing(int index);
    // The shares of a server, in the background: serverSharesListed().
    Q_INVOKABLE void listServerShares(const QString& server);
    Q_INVOKABLE void signInToServer(const QString& server); // to list its shares
    void setSignedIn(const QString& path, const QString& problem); // set by the app: how signing in went
    // Why signing in to an http WebDAV address fails (webdav::httpSignInBlocked):
    // `failed`, once it did; else, as it asks for a sign-in.
    static QString httpSignInText(bool failed);

    void setSettings(const Settings& settings); // the file was changed outside this editor
    void setWindow(QWindow* window) { m_window = window; }
    void setPlaces(SystemCatalog* places) { m_places = places; }

    // For a search of the settings that found nothing: the settings and
    // tools of Windows that fit `query` (as the launcher finds them), best
    // first, at most `max`: [{name, icon (an image URL), command}].
    Q_INVOKABLE QVariantList findWindowsSettings(const QString& query, int max) const;
    Q_INVOKABLE void openWindowsSetting(const QString& command);

    Q_INVOKABLE void addExcludedFolder(); // asks for the folder
    Q_INVOKABLE void removeExcludedPath(int index);
    Q_INVOKABLE bool addExcludedName(const QString& name);
    Q_INVOKABLE void removeExcludedName(int index);
    Q_INVOKABLE bool addExcludedTypes(const QString& text); // "wbk, .bak *.tmp"
    Q_INVOKABLE void removeExcludedType(int index);
    Q_INVOKABLE bool addExcludedFileNames(const QString& text); // "~$*; Thumbs.db"
    Q_INVOKABLE void removeExcludedFileName(int index);
    Q_INVOKABLE void removeDialogBarExcludedApp(int index);
    Q_INVOKABLE bool addContentExtensions(const QString& text); // "md, .log *.csv"
    Q_INVOKABLE void removeContentExtension(int index);
    Q_INVOKABLE bool addClipboardExcludedApp(const QString& name); // "KeePass", "keepass.exe"
    Q_INVOKABLE void removeClipboardExcludedApp(int index);
    Q_INVOKABLE bool addDoubleCtrlExcludedApp(const QString& name); // "TheFinals", "thefinals.exe"
    Q_INVOKABLE void removeDoubleCtrlExcludedApp(int index);
    // Adds a web shortcut (`index` -1) or changes the one at `index`. Returns
    // why it cannot be saved, or an empty string once it is.
    // `icon`: a picture chosen with pickWebIcon or webIconFromSvg, empty for
    // the site's own. `suggest`: its suggestion address, empty for none.
    Q_INVOKABLE QString saveWebShortcut(int index, const QString& keyword, const QString& name, const QString& url,
        const QString& home, const QString& icon = {}, const QString& suggest = {});
    // The suggestion address Win顺 knows for a search address's site
    // (knownSuggestUrl), offered in the form of a new shortcut; empty if none.
    Q_INVOKABLE QString knownSuggestUrl(const QString& url) const;
    Q_INVOKABLE void removeWebShortcut(int index);
    Q_INVOKABLE void openWebShortcut(int index); // its site, to try it
    // Asks for a PNG, ICO or SVG to show for a shortcut and keeps it: {icon: its
    // name} to save with it, {error: why not}, or {} when cancelled.
    Q_INVOKABLE QVariantMap pickWebIcon();
    // SVG-looking clipboard text for the form's editable field; unrelated
    // text and oversized contents leave it empty.
    Q_INVOKABLE QString webIconClipboardText() const;
    // The field's SVG code, checked, drawn and kept as a picked one is; with
    // a .svg picked, both answer {icon, svg: its code} for the field.
    Q_INVOKABLE QVariantMap webIconFromSvg(const QString& text);
    // The SVG code a kept icon was drawn from, for the field; empty if none.
    Q_INVOKABLE QString webIconSvg(const QString& icon) const;
    // Asks for a file and adds the shortcuts in it (a keyword already here is
    // replaced), or writes them all to one: what came of it, to show.
    Q_INVOKABLE QString importWebShortcuts();
    Q_INVOKABLE QString exportWebShortcuts();
    // The icon of a shortcut's site (FileIconProvider::webIconUrl), by its home page.
    Q_INVOKABLE QString webIconUrl(const QString& homeUrl, bool dark) const;
    int siteIconsArrived() const { return m_siteIconsArrived; }
    Q_INVOKABLE void clearClipboard() { emit clipboardClearRequested(); } // what is in no group
    // Importing clipboard history from Ditto, a text file or a CSV file
    // (ClipImport): the file is picked here ("" if none), the app does the
    // import on a thread of its own and reports back. `split`: 0 a line each,
    // 1 paragraphs, 2 the whole file (text files only).
    Q_INVOKABLE QString pickClipboardImportFile();
    Q_INVOKABLE bool clipboardImportAsksSplit(const QString& path) const; // a plain text file
    Q_INVOKABLE QString dittoDatabase() const; // Ditto's own database, "" if there is none
    Q_INVOKABLE void importClipboard(const QString& path, int split);
    Q_INVOKABLE void cancelClipboardImport() { emit clipboardImportCancelled(); }
    bool clipboardImporting() const { return m_clipboardImporting; }
    QString clipboardImportMessage() const { return m_clipboardImportMessage; }
    // Set by the app: running (with how far it is), and how it ended.
    void setClipboardImportProgress(int done, int total);
    void setClipboardImportDone(const clipimport::Report& report, const QString& group);
    Q_INVOKABLE void restartExplorer() { emit explorerRestartRequested(); }
    // "S" or "V": the letter out of Windows' DisabledHotkeys, though it is not ours.
    Q_INVOKABLE void giveBackToWindows(const QString& letter) { emit giveBackRequested(letter); }
    Q_INVOKABLE void restoreDefaults();
    Q_INVOKABLE void clearHistory() { emit historyClearRequested(); }
    Q_INVOKABLE void resetLauncherWidth() { emit launcherWidthResetRequested(); }
    Q_INVOKABLE void openDataFolder();
    Q_INVOKABLE void exportDiagnostics();
    // Automatic backups of the settings: into a folder asked for here; off.
    // Returns whether they are on now.
    Q_INVOKABLE bool chooseBackupFolder();
    Q_INVOKABLE void stopBackups();
    // The app moves the index files once the setting changes (App::applyIndexFolder).
    Q_INVOKABLE void chooseIndexFolder(); // asks for the folder
    Q_INVOKABLE void resetIndexFolder() { setIndexFolder({}); } // back to the default place
    Q_INVOKABLE void openIndexFolder();
    Q_INVOKABLE void restart() { emit restartRequested(); } // after changing the renderer

    // 拾穗计划 page
    Q_INVOKABLE void openUrl(const QString& url);
    Q_INVOKABLE void copyText(const QString& text);

    // AI 助手 page
    bool mcp() const { return m_settings.mcp; }
    void setMcp(bool on);
    bool mcpAllowOpen() const { return m_settings.mcpAllowOpen; }
    void setMcpAllowOpen(bool on);
    bool mcpClipboard() const { return m_settings.mcpClipboard; }
    void setMcpClipboard(bool on);
    bool mcpScreenshot() const { return m_settings.mcpScreenshot; }
    void setMcpScreenshot(bool on);
    bool mcpPlugins() const { return m_settings.mcpPlugins; }
    QString mcpSettings() const { return m_settings.mcpSettings; }
    void setMcpSettings(const QString& mode);
    void setMcpPlugins(bool on);
    bool mcpPluginActions() const { return m_settings.mcpPluginActions; }
    void setMcpPluginActions(bool on);
    QStringList mcpExcludedPaths() const;
    Q_INVOKABLE void addMcpExcludedFolder(); // asks for the folder
    Q_INVOKABLE void removeMcpExcludedPath(int index);
    QString mcpStatus() const { return m_mcpStatus; }
    void setMcpStatus(const QString& status); // set by the app: who is connected
    QVariantList mcpClients() const { return m_mcpClients; }
    QString mcpRelayPath() const { return m_mcpRelayPath; }
    Q_INVOKABLE void refreshMcpClients(); // looks again (as the window comes to the front)
    Q_INVOKABLE void connectMcpClient(const QString& id); // writes Win顺 into its configuration; mcpMessage says how it went
    Q_INVOKABLE void copyMcpSnippet(const QString& id); // its configuration (or command) to the clipboard; "": the generic JSON
    QString mcpMessage() const { return m_mcpMessage; }
    Q_INVOKABLE void clearMcpMessage() { setMcpMessage({}); }
    Q_INVOKABLE void openMcpLog(); // mcp.log, or the data folder while there is none

    // 插件 page
    PluginSettings* plugins() const { return m_plugins; }

    // 收藏夹 page
    FavoritesSettings* favorites() const { return m_favorites; }

signals:
    void diagnosticsChanged();
    void diagnosticsExportRequested();
    void historyCountChanged();
    void historyClearRequested();
    void launcherWidthSetChanged();
    void launcherWidthResetRequested();
    void changed();
    void hotkeyErrorChanged();
    void contentIndexStatusChanged();
    void indexFolderChanged();
    void recordingHotkeyChanged(); // the app suspends its hotkeys meanwhile
    void autostartChanged();
    void autostartRequested(bool on); // the switch: App writes it, then refreshAutostart()
    void winVStateChanged();
    void winSStateChanged();
    void searchBoxShownChanged();
    void taskbarStateChanged();
    void fileManagersChanged();
    void clipboardCountChanged();
    void numberKeysNoteChanged();
    void doubleCtrlClashChanged();
    void opusFolderMenuChanged();
    void opusRestarted(); // setOpusFolderMenu started Opus again: it came to the front
    void clipboardClearRequested();
    void clipboardImportChanged();
    void clipboardImportRequested(const QString& path, int split);
    void clipboardImportCancelled();
    void explorerRestartRequested(); // Win+V changes hands when Explorer starts again
    void giveBackRequested(const QString& letter);
    void edited(const ws::Settings& settings);
    void restartRequested();
    void sharesChanged();
    void siteIconArrived();
    // `problem` is empty when the server answered; `signIn` when it wants a user name first.
    void serverSharesListed(const QString& server, const QStringList& shares, const QString& problem, bool signIn);
    void shareVerifyRequested(const QString& path);
    void shareAcceptRequested(const QString& path);
    void foldersChanged();
    void drivesChanged();
    void folderVerifyRequested(const QString& path); // as IndexService has it (FolderStatus::path)
    void signInRequested(const QString& path); // a share, or "\\host"
    void signedIn(const QString& path, const QString& problem); // empty problem: connected
    void mcpStatusChanged();
    void mcpClientsChanged();
    void mcpMessageChanged();
    void indexAccessStateChanged();
    void elevatedRestartRequested();
    void indexServiceInstallRequested(bool install); // false: uninstall

private:
    friend class PluginSettings; // edits m_settings' plugin sections and commits
    friend class FavoritesSettings; // reads the web shortcuts' keywords and the window
    void commit();
    void setCaptureMouse(QString& field, const QString& action, bool allowBack);
    void noteFoldersEdited(); // foldersChanged() when the chosen folders (or the shares) are others now
    // m_settings.indexFolders as IndexService has them (Settings::indexFolderPath),
    // worked out when they change, not each time the cards are: no disk is read for those.
    const std::vector<std::wstring>& folderKeys() const;
    QVariantMap keepSvgIcon(QByteArrayView svg); // pickWebIcon's and webIconFromSvg's answer for SVG code
    void setIndexFolder(const QString& folder);
    void setMcpMessage(const QString& message);

    Settings m_settings;
    QString m_runningRenderer; // resolved: never "auto"
    QString m_hotkeyError;
    QString m_numberKeysNote;
    QString m_clipboardNumberKeysNote;
    QString m_clipboardQuickPasteError;
    QString m_doubleCtrlClash;
    QString m_folderMenuHotkeyError;
    QString m_captureLastPinHotkeyError;
    QString m_captureHotkeyError;
    QString m_capturePinsHotkeyError;
    QString m_capturePinsRestoreHotkeyError;
    QString m_captureNote;
    QString m_opusFolderMenu = QStringLiteral("manual");
    QString m_opusFolderMenuTaken;
    QString m_opusFolderMenuProblem;
    bool m_opusRunning = false;
    int m_historyCount = 0;
    bool m_launcherWidthSet = false;
    int m_siteIconsArrived = 0;
    QString m_contentIndexStatus;
    IndexFolderState m_indexFolderState;
    IndexAccessState m_indexAccessState;
    QStringList m_fileManagers {QStringLiteral("explorer")};
    bool m_recordingHotkey = false;
    QString m_winVState = QStringLiteral("off");
    QString m_winSState = QStringLiteral("off");
    bool m_canRestartExplorer = false;
    bool m_taskbarButtonPinned = false;
    bool m_windowsSearchShown = true;
    QString m_searchBoxHidden = QStringLiteral("pending");
    bool m_classicTaskbar = false;
    QString m_clipboardHotkeyError;
    int m_clipboardCount = 0;
    bool m_clipboardImporting = false;
    QString m_clipboardImportMessage;
    QPointer<QWindow> m_window;
    QString m_pickedWebIcon; // chosen in a form not saved yet: kept from pruning
    QString m_diagnosticsStatus;
    bool m_diagnosticsBusy = false;
    std::vector<ShareIndexer::Status> m_shareStatuses;
    std::vector<remotecontent::Indexer::Status> m_shareContentStatuses;
    std::vector<unc::Known> m_knownShares; // read once, with the window
    std::vector<wsl::Distribution> m_knownWsl;
    QStringList m_wslStarting;
    QHash<QString, QString> m_wslStartProblems;
    std::vector<IndexService::FolderStatus> m_folderStatuses;
    std::vector<Settings::KnownFolder> m_knownFolders; // read once, with the window
    QStringList m_foldersAnnounced; // as foldersChanged() last had them
    QList<NetworkShare> m_sharesAnnounced;
    mutable QStringList m_folderKeysOf; // folderKeys(): of these
    mutable std::vector<std::wstring> m_folderKeys;
    SystemCatalog* m_places = nullptr; // the app's; outlives the window
    std::unique_ptr<ShortcutCapture> m_capture; // while recording a hotkey
    struct TaskbarWatch; // the taskbar's pinned programs and Windows' search box setting
    std::unique_ptr<TaskbarWatch> m_taskbarWatch;
    QString m_mcpStatus;
    QVariantList m_mcpClients;
    QString m_mcpRelayPath;
    QString m_mcpMessage;
    PluginSettings* m_plugins = nullptr; // a child
    FavoritesSettings* m_favorites = nullptr; // a child
};

} // namespace ws
