#pragma once

#include "ContentIndex.h"
#include "Crawler.h"
#include "SearchTypes.h"
#include "ShareIndexer.h"
#include "WebShortcut.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ws::ntfs {
enum class ServiceState; // ServiceVolumeAccess.h
}

namespace ws {

// A content size limit as kept, 1 KB to 4096 MB: under 1 MB in whole KB
// (shown as KB), from there in hundredths of a MB (GitHub #28).
inline double roundedSizeMB(double mb)
{
    mb = std::clamp(mb, 1.0 / 1024, 4096.0);
    return mb < 1 ? std::max(1.0, std::round(mb * 1024)) / 1024 : std::round(mb * 100) / 100;
}

// The orders of the results (Settings::sort), in Launcher::Sort's order:
// best match, newest, oldest, then by name, by the folder each is in, by
// type, largest first, smallest first (GitHub #44).
inline constexpr QStringView kSortNames[] {
    u"match", u"newest", u"oldest", u"name", u"folder", u"type", u"largest", u"smallest"};

inline bool isSortName(QStringView name)
{
    return std::ranges::find(kSortNames, name) != std::end(kSortNames);
}

// Its place in kSortNames; 0 (best match) for a name not in it.
inline int sortIndex(QStringView name)
{
    const auto at = std::ranges::find(kSortNames, name);
    return at == std::end(kSortNames) ? 0 : static_cast<int>(at - std::begin(kSortNames));
}

// A network share searched with the local disks (ShareIndexer).
struct NetworkShare {
    QString path; // "\\host\share", or a folder in it (unc::normalize)
    QString name; // shown instead of the path; empty: the path
    bool content = false; // its files' contents indexed too ([Shares] N\Content; GitHub #114)
    ShareScan scan;

    bool operator==(const NetworkShare&) const = default;
};

// What the user set for one plugin (src/plugin): its [Plugin.<id>] section.
// Kept as written, values and all, so that save() loses nothing of what the
// settings page (to come) or a hand will put there.
struct PluginConfig {
    QString id;
    bool enabled = false; // Enabled=; plugins start out off
    QList<std::pair<QString, QString>> keywords; // Keyword.<command>=: the user's keyword for a command (empty: none)
    QList<std::pair<QString, QString>> settings; // Setting.<key>=: a declared setting's value, as text (never a password: the Credential Manager keeps those)
    // Confirmed=: the permissions the user said yes to when turning it on;
    // a version asking for more is asked about again.
    QStringList confirmed;
    // Ai=: the card's "给 AI 用" switch (its commands as MCP tools, with
    // [Mcp] Plugins on); on unless turned off.
    bool ai = true;

    bool operator==(const PluginConfig&) const = default;
};

// User settings, edited in the settings window and stored as an INI file
// (%APPDATA%\WinShun\WinShun.ini). Missing keys are written back with
// their defaults so every option is discoverable in the file.
struct Settings {
    // Existing profiles and deliberately prepared profiles skip the welcome
    // screen. load() marks an empty profile incomplete before writing defaults.
    bool onboardingComplete = true;
    bool onboardingCapturePending = false;
    // [Launcher]
    bool doubleCtrl = true; // enable the Ctrl/Alt/Shift gesture (doubleTapKey), routed by doubleTapAction
    QString doubleTapKey = QStringLiteral("ctrl"); // ctrl | alt | shift
    QString doubleTapAction = QStringLiteral("launcher"); // launcher | clipboard | plugin:<id>/<registered key>
    QString doubleCtrlKey = QStringLiteral("both"); // both | left | right: which of the two is tapped (twice the same one)
    bool doubleCtrlPauseInGames = true; // not while a game has the mouse or exclusive full screen (GameGuard)
    bool doubleCtrlPauseInFullScreen = false; // not over any full-screen window
    QStringList doubleCtrlExcludedApps; // program files ("TheFinals.exe") in front of which double Ctrl does nothing
    QString hotkey; // e.g. "Alt+Space"; empty = none
    QString panelHotkey; // opens the full search panel directly; empty = none
    // The programs run as administrator that were told of, once each, that
    // a double tap cannot be heard over them (AdminTapHint), lower case.
    QStringList adminTapHinted;
    // ... and that their file dialogs cannot be taken anywhere (AdminDialogHint, GitHub #216).
    QStringList adminDialogHinted;
    // The Win顺 version that last said its index service is older and can be
    // updated in one click (a portable copy, ServiceUpdate.h): once a version.
    QString serviceUpdateHinted;
    QString shortcutAgain = QStringLiteral("close"); // close | panel: what the shortcut does when the box is up and in front
    QString renderer; // software | d3d11; defaults() picks by memory (defaultRenderer)
    // 网络代理 (GitHub #84), for every request over WinHTTP: system (what Windows
    // has: its proxy, a PAC script, auto-detection) | none | manual (an HTTP
    // proxy at proxyAddress, "127.0.0.1:7890"; checked by proxy::parse, and
    // while it is not a good one, the system's is followed).
    QString proxyMode = QStringLiteral("system");
    QString proxyAddress;
    bool recordHistory = true; // remember what was opened: listed with nothing typed, first among matches
    bool rememberAdmin = false; // Enter runs as administrator what was last run so from here (GitHub #313)
    bool dialogJump = true; // Ctrl+G in an Open or Save dialog goes to the folder open in a file manager (filemanager::)
    // A search bar under Open and Save dialogs (DialogBar). Off for new
    // installs, a module turned on where wanted (the 插件 page's 内置功能);
    // an ini from before without the key keeps it on.
    bool dialogBar = false;
    QString dialogBarPlace = QStringLiteral("auto"); // auto | below | left | right: where it goes by the dialog
    // It searches only the folder the dialog shows (GitHub #286): turned on
    // and off in the bar itself (its menu, Ctrl+Shift+D), kept here.
    bool dialogBarFolderOnly = false;
    bool dialogAutoJump = false; // file dialogs go to the folder open in a file manager by themselves
    // open | back: as a dialog comes up, or only when the user comes back to
    // it from the file manager (GitHub #14: it opens where Windows left it).
    QString dialogAutoJumpWhen = QStringLiteral("open");
    // The user was asked under the dialog bar whether to go by itself (or set it in Settings): asked once.
    bool dialogAutoJumpAsked = false;
    QStringList dialogBarExcludedApps; // program files ("notepad.exe") whose file dialogs go without the bar
    // explorer | totalcmd | dopus | xyplorer | pulse | onecommander | custom: where folders
    // open and items are shown (shell::setFileManager); Explorer while the one
    // chosen is not installed. custom: by the command lines below (GitHub #23).
    // pulse: GitHub #41. onecommander: its public UI Automation interface.
    QString fileManager = QStringLiteral("explorer");
    // For custom: the command line that opens a folder, and the one that
    // shows an item selected (none: the item's folder is opened), {path}
    // standing for it (pathtext::customCommand).
    QString fileManagerOpenCommand;
    QString fileManagerRevealCommand;
    bool explorerMenu = true; // "用 Win顺 搜索此处" in Explorer's right-click menu (shellmenu::)
    // Typing in a File Explorer folder window searches there with Win顺
    // (TypeToSearch), in place of Explorer's own jump to a name by its first
    // letters: off unless asked for, so that an upgrade changes nothing.
    bool explorerTyping = false;
    bool opusTyping = false; // opt in per manager: typing in an Opus file display (GitHub #82)
    // ... and with the desktop's icons having the keyboard (GitHub #314): the
    // launcher as its hotkey opens it, to search the whole PC.
    bool desktopTyping = false;
    // folder | all: typed in Opus, the launcher opens on that tab's folder, or
    // with nothing before the words, to search the whole PC.
    QString opusTypingScope = QStringLiteral("folder");
    // first | only | level: typed in such a window, what is found in its
    // folder first, then the rest of the index under a caption, as Listary
    // lists it (GitHub #110); or that folder alone (with its subfolders), as
    // it searched before; or only what lies directly in it (#182).
    // Ctrl+Shift+D goes through the three for the time. An ini without the
    // key (from before): first.
    QString folderTyping = QStringLiteral("first");
    // auto | on | off: a double click on the blank space of a File Explorer
    // folder or of the desktop brings up a menu of favorite and recent
    // folders (FolderMenu, GitHub #22); auto: not while Listary runs, which
    // does the same. Off for new installs, like dialogBar; turned on, auto.
    // An ini from before without the key has auto.
    QString folderMenu = QStringLiteral("off");
    bool folderMenuDouble = true; // open it with a double click of the left button on blank space (#196; before, always)
    bool folderMenuMiddle = false; // also open it with the middle button on blank space (#137)
    bool folderMenuExplorer = true; // in File Explorer's folders
    bool folderMenuDesktop = true; // on the desktop
    // Which groups the menu lists (GitHub #51). All on is what it always
    // did; at least one stays on (the editor refuses the last one's untick,
    // load() turns all back on if a hand-edited file has none).
    bool folderMenuFavorites = true;
    bool folderMenuRecent = true;
    bool folderMenuWindows = true; // folders open in other windows
    int folderMenuRecentInline = 0; // 0: submenu; 1..10: direct rows, then More
    int folderMenuWindowsInline = 0;
    // Which actions the menu offers under the lists (GitHub #200): all on is
    // what it always did. Favorite covers both adding and removing.
    bool folderMenuItemSearch = true;
    bool folderMenuItemCopyPath = true; // the folder's full path as text (#137)
    bool folderMenuItemCmd = true;
    bool folderMenuItemPowerShell = true;
    bool folderMenuItemFavorite = true;
    bool folderMenuItemSettings = true;
    // A hotkey that brings the menu up in whatever window is in front, for
    // when another program has the double click (GitHub #25); e.g.
    // "Ctrl+Alt+D", empty = none. Works with folderMenu off too.
    QString folderMenuHotkey;
    // Directory Opus's <event> for a double click on blank space, as it was
    // before Win顺 put its own there (SettingsEditor::setOpusFolderMenu); "": none.
    QString folderMenuOpusBefore;
    // 拖放圆盘 (DropPie): the key under Esc, with files held or dragged,
    // brings up the favorite folders round the pointer to drop
    // them in. Does nothing while Win顺 runs as administrator (drops cannot
    // reach it then).
    bool dropPie = true;
    // Told that another program's hotkey has its key (GitHub #150): once, not every run.
    bool dropPieKeyTakenTold = false;

    // [Results]
    bool foldersFirst = false; // 全部 and 文件 list the folders found before the files (else after)
    // 优先显示的文件类型 (forum #104, #116): ranked by match, files of these
    // types (lower case, no dot) go before others that match about as well,
    // and first among those named just what was typed: "curl" lists
    // curl.exe before the folder curl and curl.h (PreferredTypes). Empty: none.
    QStringList preferredTypes;
    static QStringList defaultPreferredTypes(); // exe, bat, cmd, ps1, msi
    bool showModified = true; // each file and folder found says when it was last written
    bool showModifiedTime = false; // include the time on older dates too
    bool showFileSize = false; // compact results too; size sorting always shows it (GitHub #80)
    QString folderKeyword = QStringLiteral("folder"); // :folder in the search box
    // A click on a result only selects it; a double click or Enter opens it
    // (GitHub #44: a stray click opened things). Off: a click opens.
    bool doubleClickToOpen = false;
    // GitHub #81: Tab switches scopes by default, including on favorites.
    // Opt in to the former behavior: in 全部, Tab browses a favorite folder.
    bool tabEntersFavoriteFolder = false;
    bool thumbnails = true; // a picture, video, PDF found shows its thumbnail for an icon
    bool rowDeleteButton = true; // a file's row has a delete button among its buttons (GitHub #316)
    // 全部 also finds apps, and Windows' settings pages, Control Panel items
    // and switches; off, it finds names and content only (SearchEngine::Request).
    bool searchApps = true;
    bool searchSystem = true;
    bool regexMode = false; // the search box's (Launcher::regexMode, Ctrl+R)
    // match | newest | oldest | name | folder | type | largest | smallest
    // (kSortNames): how 文件 and 文件夹 order what they find, by best match,
    // by when last written, or (#44) by name, folder, type or size (the
    // footer's switch, remembered).
    QString sort = QStringLiteral("match");
    // opened | modified | none: with nothing typed, what was opened last, or
    // the files written last (the footer's switch, remembered), or nothing
    // but the favorites (GitHub #24: nothing of what was done shows).
    QString startList = QStringLiteral("opened");
    int recentItems = 5; // how many recently opened items to list, 1..200 (GitHub #119)
    bool mergeFileScope = false;
    bool appsTab = false; // 程序 among the launcher's tabs: the apps as a grid of icons (GitHub #296)
    QString queryHistoryKeys = QStringLiteral("ctrl"); // ctrl | alt | off, local Up/Down navigation
    QString logoRightClick = QStringLiteral("menu"); // menu | top | center | restore
    bool favoritesOnStart = true; // show favorites before typing; keywords always work (#135)
    // Each time the launcher opens, nothing is typed and the scope is 全部, as
    // at its first opening (GitHub #24); else it keeps the words, selected.
    bool clearOnOpen = false;
    // Each time the launcher opens, its input method goes into English mode:
    // letters and pinyin initials find most names (GitHub #201). Switched
    // back in the box, it stays so for that opening; put back on closing.
    bool englishOnOpen = false;
    // ctrl | alt | off: Ctrl+1…9 opens the nth row shown, in the launcher and
    // the bar by file dialogs (other programs' hotkeys stay theirs, see numberkeys).
    QString numberKeys = QStringLiteral("ctrl");
    // held | always | off: shared by the launcher's and clipboard's row hints.
    QString numberHints = QStringLiteral("always");
    bool shortcutHelp = true;
    bool footerHints = true;
    bool pureSearch = false; // empty floating launcher: just the input, without start results or chrome
    // auto | builtin: what Alt+P on a row shows the file with. auto: QuickLook
    // or Seer when one is running in the background (FilePreview), else
    // Win顺's own preview window; builtin: always that.
    QString previewWith = QStringLiteral("auto");
    QString previewHotkey = QStringLiteral("Alt+P"); // local; empty disables keyboard preview
    // A second key that opens the result as Enter does (GitHub #293); local,
    // empty: none. Never Space, nor the preview's (previewkeys::normalizedOpen).
    QString openHotkey;
    // An open preview shows the row the mouse rests on; off: only the current
    // row (big files are not opened one after another by a passing pointer,
    // GitHub #224).
    bool previewFollowsPointer = true;
    // A preview left open comes back with the launcher next time; off: it
    // closes with the launcher, the preview key brings it again (GitHub #258).
    bool previewRestore = true;

    // [FileTypes]: what 文件 can be narrowed to, in the strip under the
    // search box (图片: jpg, png…), in its user's order. Written as an
    // array: size=, 1\Id=, 1\Name=, 1\Extensions=; a built-in one keeps
    // its Id and no Name until renamed, so it shows in either language.
    // [QuickFilters]: queries saved under a name, switched on in the search
    // panel's side bar to narrow whatever is typed (QuickFilter). An array:
    // size=, 1\Name=, 1\Query=, 1\Keyword= (when it has one). None at first.
    QuickFilters quickFilters;
    // What is wrong with one's query, to say to its user; empty when it can be saved.
    static QString quickFilterProblem(const QString& query);
    FileTypes fileTypes;
    static FileTypes defaultFileTypes();
    FileTypes namedFileTypes() const; // each with its name, a built-in one's in the language shown
    // "jpg, .PNG *.gif" → {"jpg", "png", "gif"}: lower case, no dots, each once.
    static QStringList fileExtensions(const QString& text);
    // "~$*; Thumbs.db，*.tmp~" → {"~$*", "Thumbs.db", "*.tmp~"}: one name or
    // pattern each, between commas, semicolons or lines (a name may have
    // spaces), each once; what has a folder in it is none, nor wildcards
    // alone ("*": every file).
    static QStringList fileNamePatterns(const QString& text);
    // The extensions of content search's list, as the settings window takes
    // them: like fileExtensions but "tar.gz" stays whole.
    static QStringList contentExtensionList(const QString& text);
    // A program as the exclusion lists keep it: its file name, ".exe" added
    // ("KeePass", a pasted path); empty for none.
    static QString programFileName(const QString& text);
    // A folder name as 跳过的文件夹名称 keeps it: '/' separated, none at the end; empty for none.
    static QString excludedFolderName(const QString& text);
    // A shortcut ("Ctrl+Alt+V") as text to compare: modifiers in Windows' order,
    // lower case, aliases folded ("Control" and "Meta" as ctrl and win); empty
    // for what is not a modifier or two and one key.
    static QString normalizedHotkey(const QString& shortcut);
    // The rule for a shortcut a person (settings window) or AI sets: Ctrl, Alt
    // or Win held with its key, or a function key (F1 to F24) alone or with
    // Shift. A bare letter, Enter, Space, PrintScreen or Shift+letter would
    // take that key from every program.
    static bool hotkeyAllowed(const QString& shortcut);

    // [Appearance]
    QString theme = QStringLiteral("system"); // system | light | dark
    QString language = QStringLiteral("system"); // system | zh | en
    QString transparency; // off | on: Mica behind the windows, where Windows 11 has it; defaults() picks by memory
    int glassTransparency = 50; // 0..100, liquid background; 50 preserves the theme's default
    int glassClarity = 50; // 0..100, liquid reading surfaces; higher is more opaque
    QString taskbarIcon = QStringLiteral("color"); // color | mono: the tray's and the taskbar box's icon (monologo)
    QString uiSize = QStringLiteral("standard"); // compact | standard | roomy: type and row heights (uisize::)
    int maxRows = 8; // MaxRows=: the most rows the search box shows before its list scrolls, 4..20 (fewer where the screen has no room)
    QString uiFont; // empty: bundled default; otherwise an installed font family
    QString corners; // round | square: defaults() picks by Windows version
    QString uiStyle = QStringLiteral("fluent"); // fluent | brand | tactile | liquid: 界面风格 (Theme.style, themes/)

    // [Update]
    bool autoUpdate = true; // look for new versions on GitHub (see Updater)

    // [Backup]
    QString backupFolder; // where the settings are backed up (see Backup); empty: not

    // [Index]
    QStringList excludedPaths;
    QStringList excludedNames;
    // File types (extensions, lower case, no dot) left out of search results.
    // Not of the index: the files stay in it, so a change needs no rescan.
    QStringList excludedTypes;
    // And files so named (GitHub #132): wildcards against the whole name,
    // any case ("~$*": the lock files Office and WPS leave next to an open
    // document), as fileNamePatterns() keeps them. Search results only too.
    QStringList excludedFileNames;
    bool includeRemovableDrives = false;
    std::optional<QStringList> selectedDrives; // unset: all eligible disks; empty: no local disks
    bool rescanOnStartup = true;
    // auto | service | direct | walk: where NTFS volumes are read from
    // (GitHub #21; App::volumeAccessFor). auto: directly when running as
    // administrator, else through the index service (WinShunService.exe)
    // once it answers, else by walking the folders. direct (管理员直读) has
    // Win顺 itself run as administrator: its logon task runs elevated and
    // the setting restarts it so, which only means something on an
    // administrator's account; service and walk never read the volumes
    // themselves, and the task runs with the user's normal rights.
    QString indexAccess = QStringLiteral("auto");
    // auto | drives | folders: how much of the disks the index holds
    // (GitHub #52; App::foldersInEffect). drives: every fixed disk, whole.
    // folders: only indexFolders, while the disks are walked (with the index
    // service or 管理员直读 they are read whole all the same). auto: folders
    // when indexAccess is walk, or when Win顺 runs without administrator
    // rights and the service is not installed or is disabled; else drives.
    // An older file without the key indexed whole disks, and keeps doing so.
    QString indexScope = QStringLiteral("auto");
    // The folders indexed when that is folders, as written (%VARS% kept);
    // "D:\" stands for that whole drive. Kept as the user left it, nested
    // ones too (IndexService takes the outer one). defaults(): the user's
    // known folders (defaultIndexFolders).
    QStringList indexFolders;
    // indexFolders was set: it is in the file (as the user or App left it,
    // an empty list too). Until then it is the account's known folders as
    // they are when the settings are read, not written into the file: App
    // takes them for good (chooseDefaultIndexFolders) the first time only
    // the chosen folders are indexed, so a known folder moved meanwhile (to
    // OneDrive, say) is taken where it is then (GitHub #52 review). save()
    // writes a list changed from them in any case.
    bool indexFoldersChosen = false;
    // Hours between two walks of each of indexFolders (IndexService's
    // folderVerifyEvery, at least 60 times as long as the last one took); 0:
    // only when Win顺 starts.
    int folderVerifyHours = 24;
    // The note under the results that only the chosen folders are indexed
    // (Launcher) was closed with 不再提示, or its button taken: not shown again.
    bool folderHintDismissed = false;
    // Where the index files (index.bin, content\) are kept, '/' separated;
    // %VARS% are expanded. Empty: dataDir(), the default.
    QString indexFolder;

    // [Content]
    // Off: no 内容 tab, 全部 finds names only, and no file is read for its
    // text (no content index either).
    bool contentSearch = true;
    QStringList contentExtensions;
    // Larger files are not searched, by kind (ContentSizeLimits::Kind): plain
    // text and logs, source code, data and web pages, documents. In MB
    // (roundedSizeMB). What people write is small; the large ones are mostly
    // logs, generated code, exported data and documents full of pictures.
    // Measured 2026-10-19 on a developer's PC (470k files outside system and
    // program folders), these leave out 0.25% of text and logs (92% of their
    // bytes), 0.1% of code (19%), 1.5% of data (71%) and 0.25% of documents
    // (10%). Changing them: bump kContentDefaultsVersion in Settings.cpp and
    // keep the old ones there.
    std::array<double, ContentSizeLimits::kKinds> contentMaxSizeMB {2, 1, 2, 50};
    // Which files have which Chinese, Japanese and Korean characters. Off
    // for new installs (内容 reads the files then, slower); the 内容 strip
    // has a switch for it. An ini from before without the key keeps it on.
    bool contentIndex = false;
    bool contentInLowPriority = false; // 内容 also looks in system, program and tool folders
    bool contentDocuments = true; // and in Word, Excel, PowerPoint and PDF files (documentExtensions())

    // [Clipboard]
    bool clipboard = true; // keep a history of what is copied (Win+V page); at first as Windows' own clipboard history is
    bool clipboardWinV = false; // Win+V opens it, instead of Windows' clipboard history (see winv::)
    QString clipboardHotkey; // another shortcut for it; empty = none
    int clipboardMaxItems = 300; // entries kept outside groups, 0 = no limit
    int clipboardMaxDays = 30; // since last copied or pasted; 0 = no limit
    // keep | hide | clean: entries of copied files deleted or moved since,
    // left as they are, hidden from the list, or cleaned out (MissingFiles, GitHub #54)
    QString clipboardMissingFiles = QStringLiteral("keep");
    bool clipboardImages = true;
    QStringList clipboardExcludedApps; // program files whose copies are not kept, "KeePass.exe"
    QString clipboardNumberKeys = QStringLiteral("alt"); // alt | ctrl | off: Alt+1…9 pastes the nth row
    QString clipboardQuickPaste = QStringLiteral("off"); // off | ctrl | alt: the first nine of All, without opening the window
    bool clipboardClickToPaste = false; // a click pastes the row (Windows' way); off: picks it, a double click pastes
    QString clipboardUiSize = QStringLiteral("follow"); // follow (界面大小) | dense | compact | standard | roomy: its own type and row heights
    bool clipboardLargePictures = false; // pictures on taller rows, thumbnails large enough to make out
    bool clipboardKeepPlace = false; // pasting from the history leaves the entry where it is (GitHub #122); off: it moves to the top
    bool clipboardAtPointer = false; // always open beside the mouse, including over our own fields (GitHub #105)
    bool clipboardPauseInFullScreen = false; // only global opening hotkeys, not recording or menu actions (#121)
    // Opened over the caret (or a search box; Placement::above), the window
    // turned upside down: the newest entry by the caret, older ones upwards.
    bool clipboardFlipAbove = true;
    QString clipboardSearchBoxSide = QStringLiteral("near"); // near | far: the search box's end, by the caret or away from it
    // The clipboard's hotkey held after it opened the window, as Ditto's
    // "Move selection on open hot key" (GitHub #107, hotkeysteps): its key
    // again moves to the next older entry, letting go pastes it. Off: Win+V
    // pressed again closes the window, as Windows' own (and Win顺) always did.
    bool clipboardHoldToStep = false;
    // Into a program run as administrator, the index service's helper
    // presses the paste keys (GitHub #277); needs that service.
    bool clipboardPasteElevated = true;

    // [Taskbar]: Win顺 in the place of Windows' search (see taskbar::)
    bool taskbarWinS = false; // Win+S opens it over the taskbar, instead of Windows' search (see winv::)
    bool taskbarStartTyping = false; // typing in the Start menu searches with Win顺 (TypeToSearch)
    bool taskbarSearchBox = false; // Win顺's own search box on the taskbar (taskbar::SearchBox)
    QString taskbarSearchBoxStyle = QStringLiteral("box"); // box | label | icon: the widest form it takes, as Windows' own
    QString taskbarSearchBoxSide = QStringLiteral("auto"); // auto | tray: where on the taskbar (taskbar::Side, GitHub #271)

    // [Capture]: the screenshot is a plugin of the plugin library since
    // 0.17 (winshun.capture, GitHub #138), with its settings in its own
    // capture.ini. What Win顺 keeps of the group: Enabled, read only, which
    // MovedPlugins acts on (the plugin installed and the group moved to it);
    // PrintScreenBefore (what PrintScreenKeyForSnippingEnabled was before an
    // older Win顺 took PrtScn: "absent" or the number), put back while the
    // plugin is not there to keep it (App::restorePrintScreen); Keyword,
    // typed first in the search box with a space after it: the screenshot
    // history (jt) of the clipboard history's screenshots, while the plugin
    // is on. Plugins' keywords alike give way to it; a web search's or a
    // favorite's alike is refused when set.
    bool captureEnabled = false;
    QString capturePrintScreenBefore;
    QString captureKeyword = QStringLiteral("jt");

    // [PasteFile]: 粘贴为文件 (the PasteEx request): the words or the
    // picture on the clipboard made a file in the folder shown, selected for
    // its name to be typed (src/stub/PasteAsFile, core/PasteFile). A card on
    // the 插件 page, off until turned on; while off, nothing of it runs.
    bool pasteFileEnabled = false;
    // Ctrl+V on the files of an Explorer folder or the desktop, when what is
    // copied is words or a picture and no files: Explorer pastes nothing then
    // (TypeToSearch).
    bool pasteFileCtrlV = true;
    bool pasteFileMenu = true; // "粘贴为文件" in the right-click menu of a folder's empty space (shellmenu::)

    // The web plugin's editable data, kept in [Plugin.winshun.web] Setting.*.
    // Loaded regardless of enabled/installed state, so disabling loses no data.
    // Keywords the user gives web pages ("gh" opens GitHub, "gh
    // WinShun" searches it; see WebShortcut). At first just one, "winshun".
    // Written as an array: size=, 1\Keyword=, 1\Name=, 1\Url=, 1\Home=,
    // 1\Icon=, 1\Suggest= (written even empty: an entry without it is one
    // from before suggestions, and gets the one Win顺 knows for its site).
    WebShortcuts webShortcuts;
    // Suggestions=: the sites' search suggestions under a keyword's search
    // (WebSuggester). On: only words typed after a site's keyword go out,
    // to that site, which the user is about to search anyway.
    bool webSuggestions = true;

    // [Shares]: network shares searched too, written as an array: size=,
    // 1\Path=, 1\Name=. A mapped drive (Z:\...) is kept as the share it maps.
    QList<NetworkShare> shares;

    // [Network]: how shares are read
    // Read in full again, also when the server tells of changes: Samba (most NAS)
    // tells only of those made over the network, and some tell of none while
    // seeming to. 0: on start only.
    int networkVerifyHours = 1;
    int networkVerifyHoursUnwatched = 1; // when it does not
    int networkThreads = 16; // folders read at a time from a server (half that when verifying)
    QStringList networkExcludedNames; // folders NAS boxes and Macs keep for themselves; on top of [Index]
    QStringList networkExcludedFiles; // file names left out on shares; '*' at the start only

    // [Mcp]: AI clients (Claude, Cursor, VS Code …) use Win顺 through
    // WinShunMcp.exe (src/mcp). The switch covers the read-only tools (off
    // until a client is connected from the settings, which turns it on);
    // opening things and reading the clipboard history have switches of their own.
    bool mcp = false;
    bool mcpAllowOpen = false; // open_item, show_in_folder
    bool mcpClipboard = false; // clipboard_history, clipboard_get, screenshot_history
    // Screenshot=: take_screenshot; even on, every call waits for the user
    // to click 允许 on the screen (App::confirmScreenshot).
    bool mcpScreenshot = false;
    QStringList mcpExcludedPaths; // folders AI never sees, on top of the index's exclusions
    // Plugins=: the plugins' commands as tools (plugin::AiBridge; queries
    // only); PluginActions=: their actions too (plugin_<id>_<command>_run).
    bool mcpPlugins = false;
    bool mcpPluginActions = false;
    // Settings=: whether AI may change Win顺's settings (change_settings, see
    // AiSettings): ask (the user allows each change on the screen), auto
    // (changes at once, the user is told and can undo) or off. Never
    // changed by an AI: only the user's own click sets it.
    QString mcpSettings = QStringLiteral("ask");

    // [Plugins]: the plugin system's own (the plugin design, §5.5).
    bool pluginDeveloperMode = false; // DeveloperMode=: the log pane, scripts reloaded as they change
    bool pluginBuiltinsExpanded = false; // BuiltinsExpanded=: keep installed plugins within reach
    int pluginMaxResident = 8; // MaxResident=: resident plugins running at once, the longest unused leaving past it; 0: no limit (GitHub #252)

    // [Plugin.<id>]: one section per plugin the user set anything for.
    QList<PluginConfig> plugins;
    const PluginConfig* plugin(QStringView id) const; // nothing when the user set nothing for it
    PluginConfig& pluginConfig(const QString& id); // the section, made when there is none
    const WebShortcuts& webShortcutsInUse() const; // Disabled plugin data reserves no keywords.

    static Settings defaults();
    static bool exists(); // false on the very first run

    void load();
    void save() const;
    bool saveChecked() const; // sync to disk; the welcome screen must not continue after a failed save

    bool operator==(const Settings&) const = default;

    CrawlRules crawlRules() const;
    ShareIndexer::Options shareOptions() const;
    ContentSizeLimits contentSizeLimits() const;
    QString indexDir() const; // indexFolder expanded, with native separators; dataDir() when it is empty
    // indexFolders as IndexService takes them (Options::folders): expanded,
    // full, the drive letter upper case, "D:" for a whole drive. Network
    // places (a share, a mapped drive) are left out: those are 网络共享's.
    std::vector<std::wstring> indexFolderPaths() const;
    static std::wstring indexFolderPath(const QString& folder); // one of them; empty when it is left out
    // GitHub #52: the first time only the chosen folders are indexed, a list
    // never set (indexFoldersChosen) is taken for good as it is: the
    // account's known folders as load() found them, or as changed since.
    // True when it was (to be saved). Not while it is empty (the known
    // folders all on a server, say): until then, it follows them.
    bool chooseDefaultIndexFolders();
    // Whether only indexFolders are indexed now (App::foldersInEffect).
    // Never while the disks are read whole (`walked` false: through the index
    // service, or as administrator), nor with drives. While the index service
    // is wanted and only on its way (stopped, starting, not answering yet),
    // whole disks for folders and auto alike: the chosen folders alone would
    // drop the rest of each disk, to be read again once the service is there
    // (at every login, when Win顺 comes up first). Else folders; auto only
    // when the walk is for good: told to walk, or the service not installed
    // or disabled and Win顺 not running as administrator.
    bool foldersInEffect(bool walked, bool elevated, bool serviceWanted, ntfs::ServiceState service) const;
    // The folders a new list starts with (GitHub #52): the user's desktop,
    // documents, downloads, pictures, videos and music, and OneDrive's root;
    // none inside another (OneDrive's root holds a desktop it backs up),
    // none that is not there, none on a network.
    static QStringList defaultIndexFolders();
    // ... the same known folders by name (桌面, 文档 …), on a network too,
    // for the settings to offer: {name, path, network}.
    struct KnownFolder {
        QString name; // translated
        QString path; // native separators
        bool network = false; // a share or a mapped drive: 网络共享's
    };
    static std::vector<KnownFolder> knownFolders();
    // A share's place, not one of this PC's disks: "\\host\..." or on a mapped drive.
    static bool isNetworkPath(const QString& path);

    static QString filePath();
    static QString storedRenderer(); // just [Launcher] Renderer; works before QGuiApplication exists
    static QString resolveRenderer(const QString& renderer); // anything but software or d3d11 (an old "auto") -> defaultRenderer()
    static QString defaultRenderer(); // d3d11 with 8 GB of RAM or more, else software
    static bool lowMemory(); // 16 GB of RAM or less: transparency starts off
    static QString dataDir(); // %LOCALAPPDATA%\WinShun, or the profile's folder
    // The hidden --profile <folder>: the settings and all data in that folder
    // instead, for screenshots of made-up files with a history and clipboard
    // of their own (tools/make_screenshots.py). Before anything reads them.
    static void useProfile(const QString& folder);
    static QString profile(); // empty without --profile
    // A portable copy (GitHub #19): "data" beside WinShun.exe, when that
    // folder exists, else empty. Used as the profile; works before
    // QCoreApplication exists.
    static QString portableFolder();
    static bool windowsClipboardHistory(); // Windows' own clipboard history is on (Settings > System > Clipboard)
    static bool hasClipboardSettings(); // the file has a [Clipboard] section: written by a version with the clipboard history
    static QString resolveLanguage(const QString& language); // "system" -> zh with a Chinese Windows, else en
    // What is typed or written for a share, as it is kept: "\\host\share[\folder]"
    // (a mapped drive's share instead of the letter); empty when it is no share.
    static QString sharePath(const QString& text);
};

} // namespace ws
