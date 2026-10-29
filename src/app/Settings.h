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
    // [Launcher]
    bool doubleCtrl = true; // a double tap opens the launcher: of Ctrl, Alt or Shift (doubleTapKey)
    QString doubleTapKey = QStringLiteral("ctrl"); // ctrl | alt | shift
    QString doubleCtrlKey = QStringLiteral("both"); // both | left | right: which of the two is tapped (twice the same one)
    bool doubleCtrlPauseInGames = true; // not while a game has the mouse or exclusive full screen (GameGuard)
    bool doubleCtrlPauseInFullScreen = false; // not over any full-screen window
    QStringList doubleCtrlExcludedApps; // program files ("TheFinals.exe") in front of which double Ctrl does nothing
    QString hotkey; // e.g. "Alt+Space"; empty = none
    QString shortcutAgain = QStringLiteral("close"); // close | panel: what the shortcut does when the box is up and in front
    QString renderer; // software | d3d11; defaults() picks by memory (defaultRenderer)
    // 网络代理 (GitHub #84), for every request over WinHTTP: system (what Windows
    // has: its proxy, a PAC script, auto-detection) | none | manual (an HTTP
    // proxy at proxyAddress, "127.0.0.1:7890"; checked by proxy::parse, and
    // while it is not a good one, the system's is followed).
    QString proxyMode = QStringLiteral("system");
    QString proxyAddress;
    bool recordHistory = true; // remember what was opened: listed with nothing typed, first among matches
    bool dialogJump = true; // Ctrl+G in an Open or Save dialog goes to the folder open in a file manager (filemanager::)
    // A search bar under Open and Save dialogs (DialogBar). Off for new
    // installs, a module turned on where wanted (the 插件 page's 内置功能);
    // an ini from before without the key keeps it on.
    bool dialogBar = false;
    QString dialogBarPlace = QStringLiteral("auto"); // auto | below | left | right: where it goes by the dialog
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
    // first | only: typed in such a window, what is found in its folder
    // first, then the rest of the index under a caption, as Listary lists it
    // (GitHub #110); or that folder alone, as it searched before. Ctrl+D
    // switches it for the time. An ini without the key (from before): first.
    QString folderTyping = QStringLiteral("first");
    // auto | on | off: a double click on the blank space of a File Explorer
    // folder or of the desktop brings up a menu of favorite and recent
    // folders (FolderMenu, GitHub #22); auto: not while Listary runs, which
    // does the same. Off for new installs, like dialogBar; turned on, auto.
    // An ini from before without the key has auto.
    QString folderMenu = QStringLiteral("off");
    bool folderMenuExplorer = true; // in File Explorer's folders
    bool folderMenuDesktop = true; // on the desktop
    // Which groups the menu lists (GitHub #51). All on is what it always
    // did; at least one stays on (the editor refuses the last one's untick,
    // load() turns all back on if a hand-edited file has none).
    bool folderMenuFavorites = true;
    bool folderMenuRecent = true;
    bool folderMenuWindows = true; // folders open in other windows
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
    // match | newest | oldest | name | folder | type | largest | smallest
    // (kSortNames): how 文件 and 文件夹 order what they find, by best match,
    // by when last written, or (#44) by name, folder, type or size (the
    // footer's switch, remembered).
    QString sort = QStringLiteral("match");
    // opened | modified | none: with nothing typed, what was opened last, or
    // the files written last (the footer's switch, remembered), or nothing
    // but the favorites (GitHub #24: nothing of what was done shows).
    QString startList = QStringLiteral("opened");
    // Each time the launcher opens, nothing is typed and the scope is 全部, as
    // at its first opening (GitHub #24); else it keeps the words, selected.
    bool clearOnOpen = false;
    // ctrl | alt | off: Ctrl+1…9 opens the nth row shown, in the launcher and
    // the bar by file dialogs (other programs' hotkeys stay theirs, see numberkeys).
    QString numberKeys = QStringLiteral("ctrl");
    // held | always | off: shared by the launcher's and clipboard's row hints.
    QString numberHints = QStringLiteral("held");
    // auto | builtin: what Alt+P on a row shows the file with. auto: QuickLook
    // or Seer when one is running in the background (FilePreview), else
    // Win顺's own preview window; builtin: always that.
    QString previewWith = QStringLiteral("auto");

    // [FileTypes]: what 文件 can be narrowed to, in the strip under the
    // search box (图片: jpg, png…), in its user's order. Written as an
    // array: size=, 1\Id=, 1\Name=, 1\Extensions=; a built-in one keeps
    // its Id and no Name until renamed, so it shows in either language.
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
    // The ids of the capture toolbar's action buttons, in its order.
    static QStringList captureToolbarButtonIds();
    // `text` (comma separated ids) cut to the known ones: each once, in the toolbar's order.
    static QString normalizedToolbarHidden(const QString& text);
    // The user's words to hide, from one line or several, separated by commas,
    // semicolons or line breaks (Chinese ones too); trimmed, none empty or twice.
    static QStringList normalizedSmartWords(const QString& text);
    // The rule for a shortcut a person (settings window) or AI sets: Ctrl, Alt
    // or Win held with its key, or a function key (F1 to F24) alone or with
    // Shift. A bare letter, Enter, Space, PrintScreen or Shift+letter would
    // take that key from every program.
    static bool hotkeyAllowed(const QString& shortcut);

    // [Appearance]
    QString theme = QStringLiteral("system"); // system | light | dark
    QString language = QStringLiteral("system"); // system | zh | en
    QString transparency; // off | on: Mica behind the windows, where Windows 11 has it; defaults() picks by memory
    QString taskbarIcon = QStringLiteral("color"); // color | mono: the tray's and the taskbar box's icon (monologo)
    bool searchLogo = true; // the logo peeking out behind the launcher, the clipboard and the dialog bar (GitHub #109)
    QString uiSize = QStringLiteral("standard"); // compact | standard | roomy: type and row heights (uisize::)
    QString corners; // round | square: defaults() picks by Windows version

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
    bool clipboardLargePictures = false; // pictures on taller rows, thumbnails large enough to make out
    bool clipboardKeepPlace = false; // pasting from the history leaves the entry where it is (GitHub #122); off: it moves to the top
    // Opened over the caret (or a search box; Placement::above), the window
    // turned upside down: the newest entry by the caret, older ones upwards.
    bool clipboardFlipAbove = true;
    QString clipboardSearchBoxSide = QStringLiteral("near"); // near | far: the search box's end, by the caret or away from it
    // The clipboard's hotkey held after it opened the window, as Ditto's
    // "Move selection on open hot key" (GitHub #107, hotkeysteps): its key
    // again moves to the next older entry, letting go pastes it. Off: Win+V
    // pressed again closes the window, as Windows' own (and Win顺) always did.
    bool clipboardHoldToStep = false;

    // [Taskbar]: Win顺 in the place of Windows' search (see taskbar::)
    bool taskbarWinS = false; // Win+S opens it over the taskbar, instead of Windows' search (see winv::)
    bool taskbarStartTyping = false; // typing in the Start menu searches with Win顺 (TypeToSearch)
    bool taskbarSearchBox = false; // Win顺's own search box on the taskbar (taskbar::SearchBox)
    QString taskbarSearchBoxStyle = QStringLiteral("box"); // box | label | icon: the widest form it takes, as Windows' own

    // [Capture]: the screen capture (src/capture), shown as a card on the
    // 插件 page. Off until turned on; nothing of it is made while off.
    bool captureEnabled = false;
    QString captureHotkey; // no default (PixPin and Snipaste have the usual ones): the user records one
    bool capturePrintScreen = false; // PrtScn captures too, taken from the Snipping Tool (printscreen::)
    // What PrintScreenKeyForSnippingEnabled was before PrtScn was taken
    // ("absent" or the number), to put back when given up; empty while not taken.
    QString capturePrintScreenBefore;
    bool captureCopyOnSelect = false; // letting go of a drag or a click on a window copies at once
    bool captureMagnifier = true; // the magnifier with the colour beside the pointer while picking an area
    bool capturePinOnCopy = false; // copying a screenshot also pins it (#59)
    // What a mouse button does once a selection is there (and no annotation
    // tool, stroke or text is under way): none | copy | copypin | save | pin | ocr |
    // ocrcopy | ocrsearch | ocrweb | cancel, the right button also back (Esc's
    // step back). Issue #46; the ocr... ones (#57) read the picture and go on
    // at once: to the clipboard, the search box, the web search.
    QString captureMouseDoubleLeft = QStringLiteral("copy");
    QString captureMouseRight = QStringLiteral("back");
    QString captureMouseMiddle = QStringLiteral("none");
    QString captureMouseBack = QStringLiteral("none"); // the side button toward the user (X1)
    QString captureMouseForward = QStringLiteral("none"); // X2
    // The capture toolbar's action buttons left off it (#57), comma separated:
    // undo, redo, copy, save, pin, ocr, long, cancel (captureToolbarButtonIds); empty
    // shows them all. Their keys work all the same.
    QString captureToolbarHidden;
    // Keyword=: typed first in the search box with a space after it, the
    // screenshot history (the launcher's jt). Plugins' keywords alike give
    // way to it; a web search's or a favorite's alike is refused when set.
    QString captureKeyword = QStringLiteral("jt");
    // What each annotation tool remembers (width, colour, ...), as JSON in
    // logical units (annot::StyleMemory); empty until a tool's style changes.
    QString captureAnnotateStyles;
    // Smart redaction (docs/design-capture-roadmap.md, 4.1): look for what to
    // hide as soon as a selection has held still (SmartPrescan), also hide IP
    // and MAC addresses, hide every word, and the user's own words to hide
    // (comma separated, whole words).
    bool captureSmartPrescan = true;
    bool captureSmartIpMac = false;
    bool captureSmartAllText = false;
    QString captureSmartWords;
    // Stage 7. The mouse cursor drawn into the frozen picture (IncludeCursor);
    // UI Automation for the controls of WinUI and the like, never Chromium's (ControlsUia);
    // the delayed capture's seconds, 3 | 5 | 10 (DelaySeconds); the colour
    // format the magnifier writes, hex | rgb | hsl (ColorFormat, changed with
    // Shift alone); the hotkeys that hide / show every pin and that give click-through
    // pins the mouse back (PinsHotkey, PinsRestoreHotkey: recorded, none by default).
    bool captureCursor = false;
    // Saving a capture from an HDR monitor writes an HDR file (.jxr) beside
    // the PNG too (HdrFile, GitHub #133). Off: the frames stay 8-bit.
    bool captureHdrFile = false;
    bool captureControlsUia = true;
    int captureDelay = 5;
    QString captureColorFormat = QStringLiteral("hex");
    QString capturePinsHotkey;
    QString capturePinsRestoreHotkey;
    QString captureLastPinHotkey;
    QString captureSaveFolder;
    QString captureRecordingFolder;
    bool captureRecordingSystemAudio = false;
    bool captureRecordingMicrophone = false;
    int captureRecordingQuality = 1; // 0: smaller file, 1: standard, 2: higher quality
    bool captureRecordingCursor = true;
    bool captureRecordingCountdown = true;
    bool captureRecordingClicks = false;
    int captureRecordingFormat = 0; // 0: MP4, 1: GIF
    // GitHub #98: the keys pressed shown in recordings: off | shortcuts
    // (Ctrl/Alt/Win combinations and Esc, Enter, F1…; never typing) | all.
    QString captureRecordingKeys = QStringLiteral("off");
    bool captureRecordingMouseButtons = false; // which mouse button, shown in recordings
    // What a double click on a pin does: reset (back to 100%) | close |
    // collapse (to a tile) | none (PinDoubleClick).
    QString capturePinDoubleClick = QStringLiteral("reset");

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

    // [WebSearch]: keywords the user gives web pages ("gh" opens GitHub, "gh
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

    // [Plugin.<id>]: one section per plugin the user set anything for.
    QList<PluginConfig> plugins;
    const PluginConfig* plugin(QStringView id) const; // nothing when the user set nothing for it
    PluginConfig& pluginConfig(const QString& id); // the section, made when there is none

    static Settings defaults();
    static bool exists(); // false on the very first run

    void load();
    void save() const;

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
