#include "AiSettings.h"

#include "WebShortcut.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::aisettings {

namespace {

QString translated(const char* source)
{
    return QCoreApplication::translate("AiSettings", source);
}

// ---- what AI may write in --------------------------------------------------------
//
// Whatever AI sends reaches the user's eyes on the card and the notice, and
// the settings file: so it is short, one line, and plain.

constexpr qsizetype kMaxNameChars = 260; // a name, a keyword, a program (MAX_PATH)
constexpr qsizetype kMaxPathChars = 1024; // a folder path, a web address
constexpr qsizetype kMaxItemsPerChange = 50;

// Control characters (line breaks, tabs), format characters (right-to-left
// marks that make text read as another), line and paragraph separators,
// stray surrogates: none of them belongs in a name or an address.
bool hasHiddenChars(const QString& text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t c = text[i].unicode();
        if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate())
            c = QChar::surrogateToUcs4(text[i], text[i + 1]), ++i;
        switch (QChar::category(c)) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
            return true;
        default:
            break;
        }
    }
    return false;
}

// An error for text AI wrote (`what` names it), or empty.
QString textProblem(const QString& text, qsizetype maxChars, const QString& what)
{
    if (text.size() > maxChars)
        return u"%1 is longer than %2 characters."_s.arg(what).arg(maxChars);
    if (hasHiddenChars(text))
        return u"%1 has a control, line-break or invisible direction character; write it as plain text."_s.arg(what);
    return {};
}

// ---- the table -----------------------------------------------------------------
//
// Titles are what the settings page says (the user sees them on the card);
// each is written out as QT_TRANSLATE_NOOP so that lupdate finds it.

struct Head {
    const char* key;
    const char* title;
    const char* section;
    int tab;
    const char* about;
};

Entry base(const Head& head, Type type)
{
    Entry e;
    e.key = QString::fromLatin1(head.key);
    e.title = head.title;
    e.section = QString::fromLatin1(head.section);
    e.tab = head.tab;
    e.about = QString::fromUtf8(head.about);
    e.type = type;
    if (e.section == u"capture" || e.section == u"paste_file") // a card of the 插件 page, not a row the search finds
        e.rowTitle = "";
    return e;
}

Entry boolean(const Head& head, bool Settings::* field, Effect effect = Effect::Now, Widens widens = Widens::No)
{
    Entry e = base(head, Type::Bool);
    e.effect = effect;
    e.widens = widens;
    e.field = field;
    return e;
}

// A switch that shows AI more once it is on: turning it on asks the user.
Entry recording(const Head& head, bool Settings::* field)
{
    return boolean(head, field, Effect::Now, Widens::TurnOn);
}

Entry choice(const Head& head, QString Settings::* field, std::vector<Choice> choices, Effect effect = Effect::Now)
{
    Entry e = base(head, Type::Choice);
    e.effect = effect;
    e.choices = std::move(choices);
    e.field = field;
    return e;
}

// A whole number as `field` holds it, between `min` and `max`; with a
// `zeroLabel`, 0 is allowed as well.
Entry integer(const Head& head, int Settings::* field, int min, int max, const char* zeroLabel = nullptr,
    const char* zeroMeaning = nullptr)
{
    Entry e = base(head, Type::Int);
    e.min = min;
    e.max = max;
    e.zeroLabel = zeroLabel;
    if (zeroMeaning)
        e.zeroMeaning = QString::fromUtf8(zeroMeaning); // "only when Win顺 starts"
    e.field = field;
    return e;
}

// A size limit in MB for one kind of file (ContentSizeLimits::Kind).
Entry sizeMB(const Head& head, std::size_t kind)
{
    Entry e = base(head, Type::Number);
    e.min = 1.0 / 1024; // 1 KB
    e.max = 4096;
    e.field = Entry::SizeLimit {kind};
    return e;
}

Entry hotkey(const Head& head, QString Settings::* field)
{
    Entry e = base(head, Type::Hotkey);
    e.field = field;
    return e;
}

Entry readOnlyText(const Head& head, QString Settings::* field)
{
    Entry e = base(head, Type::Text);
    e.access = Access::ReadOnly;
    e.field = field;
    return e;
}

Entry list(const Head& head, QStringList Settings::* field, ListKind kind, Widens widens = Widens::No)
{
    Entry e = base(head, Type::List);
    e.list = kind;
    e.widens = widens;
    e.field = field;
    return e;
}

// A setting AI never sees: kept here so that the tests can say so.
Entry hidden(const char* key)
{
    Entry e;
    e.key = QString::fromLatin1(key);
    e.access = Access::Hidden;
    return e;
}

std::vector<Entry> buildCatalog()
{
    std::vector<Entry> t;
    const auto add = [&t](Entry e) -> Entry& { return t.emplace_back(std::move(e)); };

    // ---- 打开 Win顺: 快捷键 ----
    add(boolean({"Launcher/DoubleCtrl", QT_TRANSLATE_NOOP("AiSettings", "双击 Ctrl、Alt 或 Shift 打开"), "shortcuts", 0,
                    "Tapping Ctrl twice quickly (or Alt or Shift, Launcher/DoubleTapKey) opens the search window. Turn off if it fires by accident or clashes with another program."},
        &Settings::doubleCtrl));
    add(choice({"Launcher/DoubleTapKey", QT_TRANSLATE_NOOP("AiSettings", "双击哪个键"), "shortcuts", 0,
                   "Which key is tapped twice to open the search window. Alt when another program already takes double Ctrl (Listary, PowerToys' Find My Mouse); its first tap may light up the menu bar of the program in front for a moment. Shift when the user wants it: a Chinese input method that switches between Chinese and English on a lone Shift switches twice (back where it was); typing a capital letter never counts."},
        &Settings::doubleTapKey,
        {{u"ctrl"_s, u"the Ctrl key"_s, "Ctrl"}, {u"alt"_s, u"the Alt key"_s, "Alt"}, {u"shift"_s, u"the Shift key"_s, "Shift"}}));
    add(choice({"Launcher/DoubleCtrlKey", QT_TRANSLATE_NOOP("AiSettings", "用哪一边"), "shortcuts", 0,
                   "Which of the two keys (left or right Ctrl, Alt or Shift, as Launcher/DoubleTapKey says) counts for the double tap. Use one side only when another program (PowerToys' Find My Mouse) uses the same double tap."},
        &Settings::doubleCtrlKey,
        {{u"both"_s, u"either key (the same one twice)"_s, QT_TRANSLATE_NOOP("AiSettings", "左右都行")},
            {u"left"_s, u"only the left one"_s, QT_TRANSLATE_NOOP("AiSettings", "左边")},
            {u"right"_s, u"only the right one"_s, QT_TRANSLATE_NOOP("AiSettings", "右边")}}));
    add(boolean({"Launcher/DoubleCtrlPauseInGames", QT_TRANSLATE_NOOP("AiSettings", "玩游戏时不响应双击 Ctrl、Alt 或 Shift"), "shortcuts", 0,
                    "The double tap does nothing while a game has the mouse or runs in exclusive full screen (crouching in a shooter taps Ctrl twice)."},
        &Settings::doubleCtrlPauseInGames));
    add(boolean({"Launcher/DoubleCtrlPauseInFullScreen", QT_TRANSLATE_NOOP("AiSettings", "任何程序全屏时都不响应双击 Ctrl、Alt 或 Shift"), "shortcuts", 0,
                    "The double tap does nothing over any full-screen window (videos, presentations)."},
        &Settings::doubleCtrlPauseInFullScreen));
    add(list({"Launcher/DoubleCtrlExcludedApps", QT_TRANSLATE_NOOP("AiSettings", "在这些程序里不响应双击 Ctrl、Alt 或 Shift"), "shortcuts", 0,
                 "Program files (like TheFinals.exe) in front of which the double tap does nothing."},
        &Settings::doubleCtrlExcludedApps, ListKind::Programs));
    add(hotkey({"Launcher/Hotkey", QT_TRANSLATE_NOOP("AiSettings", "快捷键"), "shortcuts", 0,
                   "A keyboard shortcut that opens the search window, written like Alt+Space or Ctrl+Alt+F. Empty: none. If another program already holds it the change is not kept."},
        &Settings::hotkey));
    add(choice({"Launcher/ShortcutAgain", QT_TRANSLATE_NOOP("AiSettings", "再次按下快捷键时"), "shortcuts", 0,
                   "What the shortcut that opens the search window (the custom key or the double tap) does when the window is already open and in front. Pressing it once more expands the window to the search panel with the typed words kept, and a third press closes it; or the window closes at once. Behind another window it only comes to the front."},
        &Settings::shortcutAgain,
        {{u"close"_s, u"closes the window"_s, QT_TRANSLATE_NOOP("AiSettings", "关闭 Win顺")},
            {u"panel"_s, u"expands the window to the search panel"_s, QT_TRANSLATE_NOOP("AiSettings", "展开为搜索面板")}}));

    // ---- 打开 Win顺: 任务栏 ----
    add(boolean({"Taskbar/SearchBox", QT_TRANSLATE_NOOP("AiSettings", "在任务栏使用 Win顺"), "taskbar", 1,
                    "Shows Win顺's own search box on the taskbar (Windows 11 only), in the place of Windows' search."},
        &Settings::taskbarSearchBox));
    add(choice({"Taskbar/SearchBoxStyle", QT_TRANSLATE_NOOP("AiSettings", "搜索框样式"), "taskbar", 1,
                   "The widest form the taskbar search box takes."},
        &Settings::taskbarSearchBoxStyle,
        {{u"box"_s, u"a search box you can type in"_s, QT_TRANSLATE_NOOP("AiSettings", "搜索框")},
            {u"label"_s, u"an icon with the word 搜索"_s, QT_TRANSLATE_NOOP("AiSettings", "图标和标签")},
            {u"icon"_s, u"a search icon only, like a taskbar button"_s, QT_TRANSLATE_NOOP("AiSettings", "仅图标")}}));
    add(boolean({"Taskbar/WinS", QT_TRANSLATE_NOOP("AiSettings", "用 Win+S 打开 Win顺"), "taskbar", 1,
                    "Win+S opens Win顺 over the taskbar instead of Windows' search. Takes effect when File Explorer restarts."},
        &Settings::taskbarWinS, Effect::ExplorerRestart));
    add(boolean({"Taskbar/StartMenuTyping", QT_TRANSLATE_NOOP("AiSettings", "在开始菜单里打字，也用 Win顺 搜索"), "taskbar", 1,
                    "Typing in the Start menu searches with Win顺 instead of Windows."},
        &Settings::taskbarStartTyping));

    // ---- 打开 Win顺: 文件管理器、资源管理器、对话框 ----
    {
        Entry& e = add(choice({"Launcher/FileManager", QT_TRANSLATE_NOOP("AiSettings", "用哪个文件管理器打开文件夹"), "files_and_dialogs", 2,
                                  "The file manager that opens folders and shows a file in its folder. Only one installed on this PC can be chosen (see facts.file_managers); custom needs the user's own command lines, which only the user can set."},
            &Settings::fileManager,
            {{u"explorer"_s, u"Windows File Explorer"_s, QT_TRANSLATE_NOOP("AiSettings", "资源管理器")},
                {u"totalcmd"_s, u"Total Commander"_s, "Total Commander"},
                {u"dopus"_s, u"Directory Opus"_s, "Directory Opus"},
                {u"xyplorer"_s, u"XYplorer"_s, "XYplorer"},
                {u"pulse"_s, u"Pulse, an open-source file manager"_s, "Pulse"},
                {u"onecommander"_s, u"OneCommander"_s, "OneCommander"},
                {u"custom"_s, u"a program with the user's own command lines"_s, QT_TRANSLATE_NOOP("AiSettings", "其他（自定义）")}}));
        e.check = [](const State& s, const QString& value, const Env& env) -> QString {
            if (value == u"custom" && s.settings.fileManagerOpenCommand.isEmpty())
                return u"'custom' needs the command line that opens a folder, which only the user can set: ask them to fill it in under 设置 → 打开 Win顺 → 其他文件管理器的命令."_s;
            if (env.fileManagerAvailable && !env.fileManagerAvailable(value))
                return u"%1 is not installed on this PC."_s.arg(value);
            return {};
        };
    }
    add(readOnlyText({"Launcher/FileManagerOpenCommand", QT_TRANSLATE_NOOP("AiSettings", "其他文件管理器的命令"), "files_and_dialogs", 2,
                         "The command line that opens a folder with the 'custom' file manager, {path} standing for the folder. A command is a program that runs: AI can read it and not change it."},
        &Settings::fileManagerOpenCommand));
    add(readOnlyText({"Launcher/FileManagerRevealCommand", QT_TRANSLATE_NOOP("AiSettings", "其他文件管理器：显示所在位置的命令"), "files_and_dialogs", 2,
                         "The command line that shows a file selected in its folder with the 'custom' file manager. AI can read it and not change it."},
        &Settings::fileManagerRevealCommand));
    add(boolean({"Launcher/ExplorerMenu", QT_TRANSLATE_NOOP("AiSettings", "资源管理器右键“用 Win顺 搜索此处”"), "files_and_dialogs", 2,
                    "Adds 用 Win顺 搜索此处 to the right-click menu of folders in File Explorer."},
        &Settings::explorerMenu));
    add(boolean({"Launcher/ExplorerTyping", QT_TRANSLATE_NOOP("AiSettings", "在资源管理器里直接打字搜索"), "files_and_dialogs", 2,
                    "Typing in a File Explorer folder window searches that folder with Win顺, instead of Explorer's jump to a name by its first letters."},
        &Settings::explorerTyping));
    add(boolean({"Launcher/OpusTyping", QT_TRANSLATE_NOOP("AiSettings", "在 Directory Opus 里直接打字搜索"),
                    "files_and_dialogs", 2,
                    "In a Directory Opus file display, letters and digits open Win顺 to search that tab's folder. "
                    "Off by default and independent of Explorer typing. Punctuation, shortcuts, menus and edit fields "
                    "stay in Opus."},
        &Settings::opusTyping));
    add(choice({"Launcher/FolderTyping", QT_TRANSLATE_NOOP("AiSettings", "在文件夹里打字时"), "files_and_dialogs", 2,
                   "What typing in a File Explorer or Directory Opus folder window lists. The folder and its subfolders come first and the rest of the PC under them (like Listary), or only that folder. Ctrl+D switches between the two for one search."},
        &Settings::folderTyping,
        {{u"first"_s, u"that folder's results first, then everywhere else"_s, QT_TRANSLATE_NOOP("AiSettings", "当前文件夹优先")},
            {u"only"_s, u"only that folder and its subfolders"_s, QT_TRANSLATE_NOOP("AiSettings", "只搜当前文件夹")}}));
    add(choice({"Launcher/FolderMenu", QT_TRANSLATE_NOOP("AiSettings", "双击空白处弹出文件夹菜单"), "files_and_dialogs", 2,
                   "A double click on blank space in a File Explorer folder or on the desktop brings up a menu of favorite and recent folders (like Listary's). 'auto' leaves it to Listary while Listary runs."},
        &Settings::folderMenu,
        {{u"auto"_s, u"on (not while Listary runs, which does the same)"_s, QT_TRANSLATE_NOOP("AiSettings", "开")},
            {u"on"_s, u"on always, also while Listary runs (both menus may come up)"_s, QT_TRANSLATE_NOOP("AiSettings", "一直开")},
            {u"off"_s, u"off"_s, QT_TRANSLATE_NOOP("AiSettings", "关")}}));
    add(boolean({"Launcher/FolderMenuExplorer", QT_TRANSLATE_NOOP("AiSettings", "在资源管理器的空白处双击"), "files_and_dialogs", 2,
                    "The folder menu comes up on a double click in File Explorer's folders."},
        &Settings::folderMenuExplorer));
    add(boolean({"Launcher/FolderMenuDesktop", QT_TRANSLATE_NOOP("AiSettings", "在桌面的空白处双击"), "files_and_dialogs", 2,
                    "The folder menu comes up on a double click on the desktop."},
        &Settings::folderMenuDesktop));
    add(boolean({"Launcher/FolderMenuFavorites", QT_TRANSLATE_NOOP("AiSettings", "菜单里列出收藏的文件夹"), "files_and_dialogs", 2,
                    "The folder menu lists the favorite folders. At least one of the three groups stays on."},
        &Settings::folderMenuFavorites));
    add(boolean({"Launcher/FolderMenuRecent", QT_TRANSLATE_NOOP("AiSettings", "菜单里列出最近的文件夹"), "files_and_dialogs", 2,
                    "The folder menu lists recently opened folders. At least one of the three groups stays on."},
        &Settings::folderMenuRecent));
    add(boolean({"Launcher/FolderMenuWindows", QT_TRANSLATE_NOOP("AiSettings", "菜单里列出别的窗口里的文件夹"), "files_and_dialogs", 2,
                    "The folder menu lists the folders open in other file manager windows. At least one of the three groups stays on."},
        &Settings::folderMenuWindows));
    add(boolean({"Favorites/DropPie", QT_TRANSLATE_NOOP("AiSettings", "拖文件时按 ` 键，弹出收藏圆盘"), "favorites", -1,
                    "While dragging files or folders (File Explorer, the desktop, file managers), pressing the key left of 1 (` and ~ on most keyboards, · in a Chinese input method; it is the key's place that counts, not what is printed on it) with them held (no need to move) brings up the first eight favorite folders round the pointer. Moving toward one and letting go drops the files there: moved within a drive, copied to another, Ctrl copies and Shift moves. Does nothing while Win顺 runs as administrator."},
        &Settings::dropPie));
    add(hotkey({"Launcher/FolderMenuHotkey", QT_TRANSLATE_NOOP("AiSettings", "用快捷键弹出文件夹菜单"), "files_and_dialogs", 2,
                   "A shortcut that brings up the folder menu in whatever window is in front, like Ctrl+Alt+D. Empty: none."},
        &Settings::folderMenuHotkey));
    add(boolean({"Launcher/DialogJump", QT_TRANSLATE_NOOP("AiSettings", "Ctrl+G 转到文件管理器的文件夹"), "files_and_dialogs", 2,
                    "In an Open or Save dialog, Ctrl+G goes to the folder open in the file manager."},
        &Settings::dialogJump));
    add(boolean({"Launcher/DialogBar", QT_TRANSLATE_NOOP("AiSettings", "对话框旁的搜索框"), "files_and_dialogs", 2,
                    "A search bar beside Open and Save dialogs."},
        &Settings::dialogBar));
    {
        Entry& e = add(choice({"Launcher/DialogBarPlace", QT_TRANSLATE_NOOP("AiSettings", "对话框旁搜索框的位置"), "files_and_dialogs", 2,
                                  "Where the dialog search bar goes."},
            &Settings::dialogBarPlace,
            {{u"auto"_s, u"below the dialog, beside it when there is no room"_s, QT_TRANSLATE_NOOP("AiSettings", "自动")},
                {u"below"_s, u"below the dialog"_s, QT_TRANSLATE_NOOP("AiSettings", "下方")},
                {u"left"_s, u"left of the dialog"_s, QT_TRANSLATE_NOOP("AiSettings", "左边")},
                {u"right"_s, u"right of the dialog"_s, QT_TRANSLATE_NOOP("AiSettings", "右边")}}));
        e.rowTitle = QT_TRANSLATE_NOOP("AiSettings", "搜索框的位置");
    }
    {
        Entry& e = add(boolean({"Launcher/DialogAutoJump", QT_TRANSLATE_NOOP("AiSettings", "对话框自动转到文件管理器的文件夹"), "files_and_dialogs", 2,
                                   "Open and Save dialogs go to the folder open in the file manager by themselves, without Ctrl+G."},
            &Settings::dialogAutoJump));
        e.after = [](State& s) { s.settings.dialogAutoJumpAsked = true; }; // chosen here (a real change): not asked again under the bar
    }
    add(choice({"Launcher/DialogAutoJumpWhen", QT_TRANSLATE_NOOP("AiSettings", "什么时候转"), "files_and_dialogs", 2,
                   "When a dialog goes to the file manager's folder by itself."},
        &Settings::dialogAutoJumpWhen,
        {{u"open"_s, u"as soon as the dialog comes up"_s, QT_TRANSLATE_NOOP("AiSettings", "打开时")},
            {u"back"_s, u"when the user comes back to the dialog from the file manager"_s, QT_TRANSLATE_NOOP("AiSettings", "切回来时")}}));
    add(list({"Launcher/DialogBarExcludedApps", QT_TRANSLATE_NOOP("AiSettings", "不在这些程序的对话框下显示搜索框"), "files_and_dialogs", 2,
                 "Program files (like notepad.exe) whose file dialogs go without the search bar."},
        &Settings::dialogBarExcludedApps, ListKind::Programs));

    // ---- 打开 Win顺: 启动与历史 ----
    {
        Entry e = base({"Launcher/Autostart", QT_TRANSLATE_NOOP("AiSettings", "开机时自动启动"), "startup", 3,
                           "Starts Win顺 when the user signs in to Windows (a task in the Task Scheduler). Not a line of the settings file."},
            Type::Bool);
        e.access = Access::AlwaysAsk;
        e.field = &State::autostart;
        add(std::move(e));
    }
    add(boolean({"Launcher/History", QT_TRANSLATE_NOOP("AiSettings", "记住打开过的项目"), "startup", 3,
                    "Remembers what was opened: listed when nothing is typed, and ranked first among matches."},
        &Settings::recordHistory));

    // ---- 外观 ----
    add(choice({"Appearance/Theme", QT_TRANSLATE_NOOP("AiSettings", "主题"), "appearance", -1, "Light or dark windows."},
        &Settings::theme,
        {{u"system"_s, u"follow Windows' light or dark mode"_s, QT_TRANSLATE_NOOP("AiSettings", "跟随系统")},
            {u"light"_s, u"light"_s, QT_TRANSLATE_NOOP("AiSettings", "浅色")},
            {u"dark"_s, u"dark"_s, QT_TRANSLATE_NOOP("AiSettings", "深色")}}));
    add(choice({"Appearance/Transparency", QT_TRANSLATE_NOOP("AiSettings", "透明效果"), "appearance", -1,
                   "Mica: the window background shows the desktop wallpaper's colour (Windows 11, with the drawing mode on graphics card). Machines with little memory start with it off."},
        &Settings::transparency,
        {{u"on"_s, u"on"_s, QT_TRANSLATE_NOOP("AiSettings", "开")}, {u"off"_s, u"off"_s, QT_TRANSLATE_NOOP("AiSettings", "关")}}));
    add(choice({"Appearance/Corners", QT_TRANSLATE_NOOP("AiSettings", "圆角"), "appearance", -1,
                   "Round corners (Windows 11's look) or square ones (Windows 10's)."},
        &Settings::corners,
        {{u"round"_s, u"round corners"_s, QT_TRANSLATE_NOOP("AiSettings", "圆角")},
            {u"square"_s, u"square corners"_s, QT_TRANSLATE_NOOP("AiSettings", "直角")}}));
    add(choice({"Appearance/UiSize", QT_TRANSLATE_NOOP("AiSettings", "界面大小"), "appearance", -1,
                   "Type and row heights of the search box, the clipboard and the dialog bar (not the settings window)."},
        &Settings::uiSize,
        {{u"compact"_s, u"smaller type and rows"_s, QT_TRANSLATE_NOOP("AiSettings", "紧凑")},
            {u"standard"_s, u"the usual size"_s, QT_TRANSLATE_NOOP("AiSettings", "标准")},
            {u"roomy"_s, u"larger type and rows"_s, QT_TRANSLATE_NOOP("AiSettings", "宽松")}}));
    add(choice({"Appearance/TaskbarIcon", QT_TRANSLATE_NOOP("AiSettings", "任务栏图标"), "appearance", -1,
                   "The icon in the notification area and the taskbar search box: in Win顺's colours, or one colour that follows the taskbar's light or dark."},
        &Settings::taskbarIcon,
        {{u"color"_s, u"in colour"_s, QT_TRANSLATE_NOOP("AiSettings", "彩色")},
            {u"mono"_s, u"one colour, black or white as the taskbar is"_s, QT_TRANSLATE_NOOP("AiSettings", "单色")}}));
    add(boolean({"Appearance/SearchLogo", QT_TRANSLATE_NOOP("AiSettings", "搜索框显示 Logo"), "appearance", -1,
        "The logo peeking out from behind the search box, the clipboard window and the bar by file dialogs."},
        &Settings::searchLogo));
    add(choice({"Appearance/Language", QT_TRANSLATE_NOOP("AiSettings", "语言"), "appearance", -1, "The language Win顺 is shown in."},
        &Settings::language,
        {{u"system"_s, u"the language Windows is shown in"_s, QT_TRANSLATE_NOOP("AiSettings", "跟随系统")},
            {u"zh"_s, u"Simplified Chinese"_s, "简体中文"},
            {u"en"_s, u"English"_s, "English"}}));

    // ---- 搜索范围 ----
    {
        // GitHub #52. Changing it prunes the index or reads whole disks again.
        Entry& e = add(choice({"Index/Scope", QT_TRANSLATE_NOOP("AiSettings", "索引哪些地方"), "scope", 0,
                                  "How much of the PC's disks the index holds. 'folders' indexes only Index/Folders, while the disks are walked (no index service, not as administrator: with either, the disks are read whole all the same; so too while the index service is installed and not disabled but not running for the moment); 'drives' every fixed disk whole. Changing it drops what is outside or reads the disks again, so every change asks the user."},
            &Settings::indexScope,
            {{u"auto"_s, u"only the chosen folders when there is no index service (not installed or disabled) and Win顺 runs without administrator rights, or when Index/Access is walk; else whole disks"_s, QT_TRANSLATE_NOOP("AiSettings", "自动")},
                {u"drives"_s, u"every fixed disk, whole"_s, QT_TRANSLATE_NOOP("AiSettings", "整个磁盘")},
                {u"folders"_s, u"only the folders in Index/Folders (whole disks in administrator mode, and while the index service is in use, or installed and not disabled)"_s, QT_TRANSLATE_NOOP("AiSettings", "只索引选中的文件夹")}}));
        e.access = Access::AlwaysAsk;
    }
    add(list({"Index/Folders", QT_TRANSLATE_NOOP("AiSettings", "索引的文件夹"), "scope", 0,
                 "The folders indexed when Index/Scope comes to folders (full paths on this PC's own disks like D:\\Work; a drive's root like E:\\ is that whole drive). Network folders go in 网络共享 instead. Never set, it is the account's known folders (desktop, documents, downloads and the like) as they are at each start, kept for good the first time only the chosen folders are indexed. Putting one in makes its files searchable, so that asks the user every time."},
        &Settings::indexFolders, ListKind::LocalFolders, Widens::Grow));
    add(integer({"Index/FolderVerifyHours", QT_TRANSLATE_NOOP("AiSettings", "定时核对"), "scope", 0,
                    "Hours between two walks of each chosen folder (Index/Folders), done once the user has been away from the PC for a while; a folder whose walk took long waits longer. 0: only when Win顺 starts."},
        &Settings::folderVerifyHours, 1, 24 * 7, QT_TRANSLATE_NOOP("AiSettings", "只在启动时"), "only when Win顺 starts"));
    add(list({"Index/ExcludedPaths", QT_TRANSLATE_NOOP("AiSettings", "不搜索的文件夹"), "scope", 0,
                 "Folders (full paths like D:\\Games) that are left out of the index: nothing in them is found. Taking one out makes its files searchable again, so that asks the user every time."},
        &Settings::excludedPaths, ListKind::Folders, Widens::Shrink));
    add(list({"Index/ExcludedNames", QT_TRANSLATE_NOOP("AiSettings", "跳过的文件夹名称"), "scope", 1,
                 "Folder names (like node_modules) that are skipped wherever they are. Taking one out makes what it holds searchable again, so that asks the user every time."},
        &Settings::excludedNames, ListKind::Names, Widens::Shrink));
    add(list({"Index/ExcludedTypes", QT_TRANSLATE_NOOP("AiSettings", "跳过的文件类型"), "scope", 1,
                 "File extensions (without dot, like bak) whose files never appear in search results, names or content; the files stay in the index, so it takes effect at once. Taking one out makes those files show again, so that asks the user every time."},
        &Settings::excludedTypes, ListKind::Extensions, Widens::Shrink));
    add(list({"Index/ExcludedFileNames", QT_TRANSLATE_NOOP("AiSettings", "跳过的文件名"), "scope", 1,
                 "File names whose files never appear in search results, names or content, with * and ? as wildcards, any case (like ~$*, the lock files Office and WPS leave next to an open document); folders are not affected, and the files stay in the index, so it takes effect at once. A name typed in the search box that is one of them shows those files all the same. Taking one out makes those files show again, so that asks the user every time."},
        &Settings::excludedFileNames, ListKind::FileNames, Widens::Shrink));
    add(recording({"Index/IncludeRemovableDrives", QT_TRANSLATE_NOOP("AiSettings", "包括 U 盘和移动硬盘"), "scope", 2,
                    "Also indexes USB sticks and external drives while they are plugged in. Turning it on makes their files searchable, so that asks the user."},
        &Settings::includeRemovableDrives));
    add(boolean({"Index/RescanOnStartup", QT_TRANSLATE_NOOP("AiSettings", "启动时检查文件变化"), "scope", 2,
                    "Checks for files changed while Win顺 was not running, each time it starts."},
        &Settings::rescanOnStartup));
    {
        Entry& e = add(choice({"Index/Access", QT_TRANSLATE_NOOP("AiSettings", "索引方式"), "scope", 2,
                                  "How the index reads NTFS drives. 'direct' (administrator mode) restarts Win顺 with other rights and so is set by the user in the settings window only."},
            &Settings::indexAccess,
            {{u"auto"_s, u"directly when running as administrator, else through the index service, else by walking the folders"_s, QT_TRANSLATE_NOOP("AiSettings", "自动")},
                {u"service"_s, u"through the index service (WinShunService.exe)"_s, QT_TRANSLATE_NOOP("AiSettings", "索引服务")},
                {u"walk"_s, u"by walking the folders (slow, no service)"_s, QT_TRANSLATE_NOOP("AiSettings", "遍历文件夹")},
                {u"direct"_s, u"administrator mode (Win顺 runs as administrator): only the user can switch to or from it"_s, QT_TRANSLATE_NOOP("AiSettings", "管理员直读")}}));
        e.access = Access::AlwaysAsk;
        e.check = [](const State& s, const QString& value, const Env& env) -> QString {
            if (value == s.settings.indexAccess)
                return {}; // so already
            // Win顺 started with administrator rights restarts itself without
            // them when the mode is not direct, and the answer to this call
            // would never arrive.
            if (env.isElevated && env.isElevated())
                return u"Win顺 is running with administrator rights just now, and changing how the index reads the drives would restart it: the user changes it in 设置 → 搜索范围 → 磁盘 → 索引方式."_s;
            // Crossing the line to or from administrator mode restarts Win顺
            // (UAC asks), and the answer to this call would never arrive.
            if (value == u"direct" || s.settings.indexAccess == u"direct") {
                QString why = u"Changing to or from 'direct' (administrator mode) restarts Win顺 with other rights, which AI cannot do; the user sets it in 设置 → 搜索范围 → 磁盘 → 索引方式."_s;
                if (value == u"direct" && env.isAdministrator && !env.isAdministrator())
                    why += u" It also needs an administrator account, which this one is not."_s;
                return why;
            }
            return {};
        };
    }

    // ---- 结果列表 ----
    add(choice({"Results/Sort", QT_TRANSLATE_NOOP("AiSettings", "排序"), "results", 0,
                   "How 文件 and 文件夹 order what they find."},
        &Settings::sort,
        {{u"match"_s, u"best match first"_s, QT_TRANSLATE_NOOP("AiSettings", "最匹配的在前")},
            {u"newest"_s, u"most recently written first"_s, QT_TRANSLATE_NOOP("AiSettings", "最新的在前")},
            {u"oldest"_s, u"oldest first"_s, QT_TRANSLATE_NOOP("AiSettings", "最旧的在前")},
            {u"name"_s, u"by file name, A to Z"_s, QT_TRANSLATE_NOOP("AiSettings", "按名称")},
            {u"folder"_s, u"by the folder each is in, A to Z"_s, QT_TRANSLATE_NOOP("AiSettings", "按所在文件夹")},
            {u"type"_s, u"by file type (extension), then name"_s, QT_TRANSLATE_NOOP("AiSettings", "按类型")},
            {u"largest"_s, u"largest files first"_s, QT_TRANSLATE_NOOP("AiSettings", "大的在前")},
            {u"smallest"_s, u"smallest files first"_s, QT_TRANSLATE_NOOP("AiSettings", "小的在前")}}));
    {
        Entry& e = add(boolean({"Results/FoldersFirst", QT_TRANSLATE_NOOP("AiSettings", "文件夹排在文件前面"), "results", 0,
                                   "全部 and 文件 list the folders found before the files (else after)."},
            &Settings::foldersFirst));
        e.rowTitle = QT_TRANSLATE_NOOP("AiSettings", "文件和文件夹的顺序");
    }
    add(list({"Results/PreferredTypes", QT_TRANSLATE_NOOP("AiSettings", "优先显示的文件类型"), "results", 0,
                 "File extensions (without dot, like exe) listed before others that match about as well; the first among those named just what was typed."},
        &Settings::preferredTypes, ListKind::Extensions));
    add(choice({"Results/StartList", QT_TRANSLATE_NOOP("AiSettings", "不输入时显示"), "results", 1,
                   "What the search box lists while nothing is typed."},
        &Settings::startList,
        {{u"opened"_s, u"what was opened last"_s, QT_TRANSLATE_NOOP("AiSettings", "最近打开")},
            {u"modified"_s, u"the files written last"_s, QT_TRANSLATE_NOOP("AiSettings", "最近修改")},
            {u"none"_s, u"nothing but the favorites"_s, QT_TRANSLATE_NOOP("AiSettings", "只有收藏夹")}}));
    add(boolean({"Results/DoubleClickToOpen", QT_TRANSLATE_NOOP("AiSettings", "双击打开结果"), "results", 1,
                    "A click on a result only selects it; a double click (or Enter) opens it. Off: a click opens it."},
        &Settings::doubleClickToOpen));
    add(boolean({"Results/TabEntersFavoriteFolder", QT_TRANSLATE_NOOP("AiSettings", "Tab 进入收藏文件夹"), "results", 1,
                    "In All, Tab browses the selected favorite folder. Off by default: Tab switches search scopes. Shift+Tab and plugin/web completion keep their usual behavior."},
        &Settings::tabEntersFavoriteFolder));
    add(boolean({"Results/ShowModified", QT_TRANSLATE_NOOP("AiSettings", "显示修改日期"), "results", 1,
                    "Each result says when the file was last written."},
        &Settings::showModified));
    add(boolean({"Results/ShowModifiedTime", QT_TRANSLATE_NOOP("AiSettings", "较早的日期也显示时间"), "results", 1,
                    "Include the local modification time on older dates in result rows."},
        &Settings::showModifiedTime));
    add(boolean({"Results/ShowFileSize", QT_TRANSLATE_NOOP("AiSettings", "显示文件大小"), "results", 1,
                    "Show file sizes in compact search results alongside the modification date when enabled. "
                    "Off by default; size sorting always shows sizes. Folders and unknown sizes stay blank."},
        &Settings::showFileSize));
    add(boolean({"Results/Thumbnails", QT_TRANSLATE_NOOP("AiSettings", "显示图片缩略图"), "results", 1,
                    "A picture, video or PDF shows its thumbnail as its icon."},
        &Settings::thumbnails));
    add(choice({"Results/PreviewWith", QT_TRANSLATE_NOOP("AiSettings", "预览文件（Alt+P）"), "results", 1,
                   "What Alt+P on a result shows the file with."},
        &Settings::previewWith,
        {{u"auto"_s, u"QuickLook or Seer when one runs in the background, else Win顺's own preview"_s, QT_TRANSLATE_NOOP("AiSettings", "自动")},
            {u"builtin"_s, u"always Win顺's own preview"_s, QT_TRANSLATE_NOOP("AiSettings", "内置预览")}}));
    add(boolean({"Launcher/ClearOnOpen", QT_TRANSLATE_NOOP("AiSettings", "每次打开时清空搜索"), "results", 2,
                    "Each time the search box opens nothing is typed and the scope is 全部, instead of keeping the last words selected."},
        &Settings::clearOnOpen));
    add(choice({"Results/NumberKeys", QT_TRANSLATE_NOOP("AiSettings", "用数字键直接打开"), "results", 2,
                   "Which modifier with 1…9 opens the nth result."},
        &Settings::numberKeys,
        {{u"ctrl"_s, u"Ctrl+1…9"_s, "Ctrl+1…9"}, {u"alt"_s, u"Alt+1…9"_s, "Alt+1…9"},
            {u"off"_s, u"none"_s, QT_TRANSLATE_NOOP("AiSettings", "不用")}}));
    add(choice({"Results/NumberHints", QT_TRANSLATE_NOOP("AiSettings", "序号提示"), "results", 2,
                   "When the numbers 1…9 show on the rows of the search box and the clipboard."},
        &Settings::numberHints,
        {{u"held"_s, u"while the modifier key is held"_s, QT_TRANSLATE_NOOP("AiSettings", "按住修饰键时")},
            {u"always"_s, u"always"_s, QT_TRANSLATE_NOOP("AiSettings", "一直显示")},
            {u"off"_s, u"never"_s, QT_TRANSLATE_NOOP("AiSettings", "不显示")}}));

    // ---- 文件内容搜索 ----
    add(recording({"Content/Enabled", QT_TRANSLATE_NOOP("AiSettings", "文件内容搜索"), "content", 0,
                    "Searching inside files: the 内容 tab. Off: names only, and no file is read for its text. Turning it on lets the contents of files be searched (also by AI), so that asks the user."},
        &Settings::contentSearch));
    add(recording({"Content/Documents", QT_TRANSLATE_NOOP("AiSettings", "搜索文档"), "content", 0,
                    "Content search also looks in Word, Excel, PowerPoint and PDF files. Turning it on makes their texts searchable, so that asks the user."},
        &Settings::contentDocuments));
    add(list({"Content/Extensions", QT_TRANSLATE_NOOP("AiSettings", "搜索这些类型的文本文件"), "content", 0,
                 "Extensions (without dot, like txt) of the text files content search reads."},
        &Settings::contentExtensions, ListKind::ContentExtensions));
    add(recording({"Content/IncludeSystemFolders", QT_TRANSLATE_NOOP("AiSettings", "也搜索系统和程序文件夹"), "content", 0,
                    "Content search also looks in system, program and tool folders (node_modules, .git…): slower and noisier. Turning it on asks the user."},
        &Settings::contentInLowPriority));
    add(boolean({"Content/Index", QT_TRANSLATE_NOOP("AiSettings", "建立内容索引"), "content", 0,
                    "Builds an index of the files' texts for faster content search. Reading every file takes minutes the first time and some disk space."},
        &Settings::contentIndex));
    add(sizeMB({"Content/MaxFileSizeMB", QT_TRANSLATE_NOOP("AiSettings", "文本和日志的大小上限"), "content", 1,
                   "Plain text and log files larger than this are not searched, in MB (1 KB is 0.001)."},
        ContentSizeLimits::Text));
    add(sizeMB({"Content/MaxCodeFileSizeMB", QT_TRANSLATE_NOOP("AiSettings", "源代码的大小上限"), "content", 1,
                   "Source code files larger than this are not searched, in MB."},
        ContentSizeLimits::Code));
    add(sizeMB({"Content/MaxDataFileSizeMB", QT_TRANSLATE_NOOP("AiSettings", "数据和网页的大小上限"), "content", 1,
                   "Data files and web pages larger than this are not searched, in MB."},
        ContentSizeLimits::Data));
    add(sizeMB({"Content/MaxDocumentFileSizeMB", QT_TRANSLATE_NOOP("AiSettings", "文档的大小上限"), "content", 1,
                   "Word, Excel, PowerPoint and PDF files larger than this are not searched, in MB."},
        ContentSizeLimits::Document));

    // ---- 网页搜索 ----
    add(boolean({"WebSearch/Suggestions", QT_TRANSLATE_NOOP("AiSettings", "网页搜索建议"), "web", -1,
                    "Shows a site's search suggestions under a keyword's search; only the words typed after the keyword go out, to that site."},
        &Settings::webSuggestions));
    {
        Entry e = base({"WebSearch/Shortcuts", QT_TRANSLATE_NOOP("AiSettings", "网页搜索关键词"), "web", -1,
                           "Keywords that open or search a web page from the search box: 'gh' opens GitHub, 'gh WinShun' searches it. Change with add ({keyword, name?, url, home?}; url is an http(s) address, %s where the searched words go) or remove (keywords). A keyword another shortcut or a favorite has is refused."},
            Type::WebShortcuts);
        e.rowTitle = QT_TRANSLATE_NOOP("AiSettings", "关键词");
        e.access = Access::AlwaysAsk;
        e.field = &Settings::webShortcuts;
        add(std::move(e));
    }

    // ---- 剪贴板 ----
    add(recording({"Clipboard/Enabled", QT_TRANSLATE_NOOP("AiSettings", "记录剪贴板历史"), "clipboard", 0,
                    "Keeps a history of what is copied (the Win+V page). Turning it on starts recording what the user copies, so that asks the user."},
        &Settings::clipboard));
    add(recording({"Clipboard/Images", QT_TRANSLATE_NOOP("AiSettings", "记录图片"), "clipboard", 0,
                    "The clipboard history keeps copied pictures too. Turning it on starts recording them, so that asks the user."},
        &Settings::clipboardImages));
    add(boolean({"Clipboard/ClickToPaste", QT_TRANSLATE_NOOP("AiSettings", "单击粘贴"), "clipboard", 0,
                    "A click pastes the row (Windows' way); off: a click picks it and a double click pastes."},
        &Settings::clipboardClickToPaste));
    add(boolean({"Clipboard/KeepPlace", QT_TRANSLATE_NOOP("AiSettings", "粘贴后保持位置"), "clipboard", 0,
                    "Pasting an entry from the history leaves it where it is in the list; off: it moves to the top."},
        &Settings::clipboardKeepPlace));
    add(boolean({"Clipboard/LargePictures", QT_TRANSLATE_NOOP("AiSettings", "大图片缩略图"), "clipboard", 0,
                    "Pictures on taller rows, thumbnails large enough to make out."},
        &Settings::clipboardLargePictures));
    add(boolean({"Clipboard/FlipAbove", QT_TRANSLATE_NOOP("AiSettings", "开在光标上方时翻转"), "clipboard", 0,
                    "Opened above the text caret, the window turns upside down: the newest entry next to the caret."},
        &Settings::clipboardFlipAbove));
    add(choice({"Clipboard/SearchBoxSide", QT_TRANSLATE_NOOP("AiSettings", "搜索框位置"), "clipboard", 0,
                   "Which end of the clipboard window the search box is at."},
        &Settings::clipboardSearchBoxSide,
        {{u"near"_s, u"by the caret"_s, QT_TRANSLATE_NOOP("AiSettings", "靠近光标")},
            {u"far"_s, u"away from the caret"_s, QT_TRANSLATE_NOOP("AiSettings", "远离光标")}}));
    add(list({"Clipboard/ExcludedApps", QT_TRANSLATE_NOOP("AiSettings", "不记录这些程序复制的内容"), "clipboard", 0,
                 "Program files (like KeePass.exe) whose copies are never kept in the history. Taking one out starts recording its copies, so that asks the user every time."},
        &Settings::clipboardExcludedApps, ListKind::Programs, Widens::Shrink));
    add(boolean({"Clipboard/WinV", QT_TRANSLATE_NOOP("AiSettings", "用 Win+V 打开，代替 Windows 自带的剪贴板"), "clipboard", 1,
                    "Win+V opens Win顺's clipboard instead of Windows' own. Takes effect when File Explorer restarts."},
        &Settings::clipboardWinV, Effect::ExplorerRestart));
    add(hotkey({"Clipboard/Hotkey", QT_TRANSLATE_NOOP("AiSettings", "另设快捷键"), "clipboard", 1,
                   "Another shortcut that opens the clipboard history, written like Ctrl+Alt+V. Empty: none."},
        &Settings::clipboardHotkey));
    add(boolean({"Clipboard/HoldToStep", QT_TRANSLATE_NOOP("AiSettings", "按住连按快捷键，松开粘贴"), "clipboard", 1,
                    "As Ditto: with the modifiers of the shortcut that opened the clipboard (Ctrl, Alt, Shift or Win) "
                    "still held, each further press of its key moves the selection one entry down, and letting go pastes "
                    "that entry. Held about a second without a further press, letting go pastes the first entry; Esc "
                    "while holding pastes nothing. On, pressing the shortcut again while holding no longer closes the panel."},
        &Settings::clipboardHoldToStep));
    add(choice({"Clipboard/NumberKeys", QT_TRANSLATE_NOOP("AiSettings", "面板内数字键粘贴"), "clipboard", 1,
                   "Which modifier with 1…9 pastes the nth row while the clipboard panel is open."},
        &Settings::clipboardNumberKeys,
        {{u"alt"_s, u"Alt+1…9"_s, "Alt+1…9"}, {u"ctrl"_s, u"Ctrl+1…9"_s, "Ctrl+1…9"},
            {u"off"_s, u"none"_s, QT_TRANSLATE_NOOP("AiSettings", "不用")}}));
    add(choice({"Clipboard/QuickPaste", QT_TRANSLATE_NOOP("AiSettings", "不打开面板直接粘贴"), "clipboard", 1,
                   "Global 1…9 shortcuts paste the first nine entries of All, respecting pinned-first order, "
                   "regardless of the last search or category. Overrides these shortcuts in other programs "
                   "(such as browser tab switching). Suspended while WinShun has focus or its clipboard panel is open."},
        &Settings::clipboardQuickPaste,
        {{u"off"_s, u"disabled"_s, QT_TRANSLATE_NOOP("AiSettings", "关闭")},
            {u"ctrl"_s, u"Ctrl+1…9"_s, "Ctrl+1…9"}, {u"alt"_s, u"Alt+1…9"_s, "Alt+1…9"}}));
    add(integer({"Clipboard/MaxItems", QT_TRANSLATE_NOOP("AiSettings", "最多保留"), "clipboard", 2,
                    "How many entries the history keeps (pinned and grouped ones aside); the oldest go first."},
        &Settings::clipboardMaxItems, 10, 100000, QT_TRANSLATE_NOOP("AiSettings", "不限"), "no limit"));
    add(integer({"Clipboard/MaxDays", QT_TRANSLATE_NOOP("AiSettings", "保留时间"), "clipboard", 2,
                    "How many days an entry is kept after it was last copied or pasted."},
        &Settings::clipboardMaxDays, 1, 3650, QT_TRANSLATE_NOOP("AiSettings", "不限"), "no limit"));
    add(choice({"Clipboard/MissingFiles", QT_TRANSLATE_NOOP("AiSettings", "已不存在的文件记录"), "clipboard", 2,
                   "History entries of copied files that were deleted or moved since (never pinned or grouped ones): left "
                   "as they are, marked as gone; hidden from the list (a search still finds them, and they still count "
                   "toward the limit); or removed from the history once two checks ten minutes apart find them gone."},
        &Settings::clipboardMissingFiles,
        {{u"keep"_s, u"left as they are"_s, QT_TRANSLATE_NOOP("AiSettings", "不处理")},
            {u"hide"_s, u"hidden"_s, QT_TRANSLATE_NOOP("AiSettings", "自动隐藏")},
            {u"clean"_s, u"removed"_s, QT_TRANSLATE_NOOP("AiSettings", "自动清理")}}));

    // ---- 截图 (the 插件 page's card, on its 已安装 tab, not the one left on) ----
    add(recording({"Capture/Enabled", QT_TRANSLATE_NOOP("AiSettings", "屏幕截图"), "capture", 0,
                    "Win顺's screen capture with its screenshot history. Off until turned on; nothing of it runs while off. Turning it on starts keeping screenshots, so that asks the user."},
        &Settings::captureEnabled));
    add(hotkey({"Capture/Hotkey", QT_TRANSLATE_NOOP("AiSettings", "截图快捷键"), "capture", 0,
                   "The shortcut that starts a capture, written like Ctrl+Alt+A. Empty: none."},
        &Settings::captureHotkey));
    add(boolean({"Capture/PrintScreen", QT_TRANSLATE_NOOP("AiSettings", "PrtScn 键也截图"), "capture", 0,
                    "The PrintScreen key starts a capture too, taken from the Snipping Tool."},
        &Settings::capturePrintScreen));
    add(boolean({"Capture/CopyOnSelect", QT_TRANSLATE_NOOP("AiSettings", "选中后直接复制"), "capture", 0,
                    "Letting go of a drag, or a click on a window, copies the picture at once."},
        &Settings::captureCopyOnSelect));
    add(boolean({"Capture/Magnifier", QT_TRANSLATE_NOOP("AiSettings", "框选时显示放大镜"), "capture", 0,
                    "The magnifier with the colour under the pointer while picking an area; off also turns off C (copy the colour) and Shift (colour format)."},
        &Settings::captureMagnifier));
    add(boolean({"Capture/HdrFile", QT_TRANSLATE_NOOP("AiSettings", "HDR 屏幕上同时保存 HDR 图片"), "capture", 0,
                    "On a monitor with HDR on, saving a screenshot also writes a JPEG XR (.jxr) of the same name beside the PNG, which keeps the bright parts as bright (Windows Photos shows it in HDR); annotations go in it too. Copies, pins, long screenshots and recordings stay as they are. Off by default."},
        &Settings::captureHdrFile));
    {
        // What each mouse button does once a selection is there (issue #46); with an annotation tool on, a stroke
        // or a text under way, the right button steps back as Esc does and the others do nothing.
        const auto mouseChoices = [](bool withBack) {
            std::vector<Choice> list {{u"none"_s, u"nothing"_s, QT_TRANSLATE_NOOP("AiSettings", "无")},
                {u"copy"_s, u"copy the picture and close"_s, QT_TRANSLATE_NOOP("AiSettings", "复制并关闭")},
                {u"save"_s, u"save the picture to a file and close"_s, QT_TRANSLATE_NOOP("AiSettings", "保存")},
                {u"pin"_s, u"pin the picture on the screen"_s, QT_TRANSLATE_NOOP("AiSettings", "贴图")},
                {u"ocr"_s, u"read the words in the picture"_s, QT_TRANSLATE_NOOP("AiSettings", "识别文字")},
                {u"ocrcopy"_s, u"read the words and copy them, then close"_s, QT_TRANSLATE_NOOP("AiSettings", "识别文字并复制")},
                {u"ocrsearch"_s, u"read the words and search them in the launcher"_s, QT_TRANSLATE_NOOP("AiSettings", "识别文字并搜索")},
                {u"ocrweb"_s, u"read the words and search them on the web"_s, QT_TRANSLATE_NOOP("AiSettings", "识别文字并网页搜索")},
                {u"cancel"_s, u"end the capture"_s, QT_TRANSLATE_NOOP("AiSettings", "取消截图")}};
            if (withBack)
                list.insert(list.begin() + 1, {u"back"_s, u"put the selection down to choose again (Esc's step)"_s,
                                                  QT_TRANSLATE_NOOP("AiSettings", "后退（重新框选）")});
            return list;
        };
        add(choice({"Capture/MouseDoubleLeft", QT_TRANSLATE_NOOP("AiSettings", "截图：双击左键"), "capture", 0,
                       "What a double click with the left button inside the selection does."},
            &Settings::captureMouseDoubleLeft, mouseChoices(false)));
        add(choice({"Capture/MouseRight", QT_TRANSLATE_NOOP("AiSettings", "截图：右键"), "capture", 0,
                       "What the right button does once there is a selection. Esc always steps back."},
            &Settings::captureMouseRight, mouseChoices(true)));
        add(choice({"Capture/MouseMiddle", QT_TRANSLATE_NOOP("AiSettings", "截图：中键"), "capture", 0,
                       "What the middle button does once there is a selection."},
            &Settings::captureMouseMiddle, mouseChoices(false)));
        add(choice({"Capture/MouseBack", QT_TRANSLATE_NOOP("AiSettings", "截图：侧键（后退）"), "capture", 0,
                       "What the side button toward the user (back, X1) does once there is a selection."},
            &Settings::captureMouseBack, mouseChoices(false)));
        add(choice({"Capture/MouseForward", QT_TRANSLATE_NOOP("AiSettings", "截图：侧键（前进）"), "capture", 0,
                       "What the side button away from the user (forward, X2) does once there is a selection."},
            &Settings::captureMouseForward, mouseChoices(false)));
    }
    {
        // GitHub #98. "all" puts what is typed, passwords too, into the videos: every change asks the user.
        Entry& e = add(choice({"Capture/RecordingKeys", QT_TRANSLATE_NOOP("AiSettings", "录屏时显示按键"), "capture", 0,
                                  "Screen recordings (MP4 and GIF) show the keys pressed as keycaps in a corner of the video. "
                                  "'shortcuts' shows key combinations with Ctrl, Alt or Win and keys such as Esc, Enter or F1, "
                                  "never typing; 'all' shows typing too, passwords included. Keys typed into Win顺's own windows "
                                  "are not shown."},
            &Settings::captureRecordingKeys,
            {{u"off"_s, u"no keys shown"_s, QT_TRANSLATE_NOOP("AiSettings", "不显示")},
                {u"shortcuts"_s, u"key combinations and command keys, never typing"_s, QT_TRANSLATE_NOOP("AiSettings", "仅快捷键")},
                {u"all"_s, u"every key, typing and passwords included"_s, QT_TRANSLATE_NOOP("AiSettings", "全部按键")}}));
        e.access = Access::AlwaysAsk;
    }
    add(boolean({"Capture/RecordingMouseButtons", QT_TRANSLATE_NOOP("AiSettings", "录屏时显示鼠标按键"), "capture", 0,
                    "Screen recordings show which mouse button was pressed (left, right, middle or a side button) as a "
                    "keycap in a corner of the video, with Ctrl, Shift, Alt or Win when held."},
        &Settings::captureRecordingMouseButtons));
    {
        Entry e = base({"Capture/Keyword", QT_TRANSLATE_NOOP("AiSettings", "截图历史的关键词"), "capture", 0,
                           "The word typed first in the search box (with a space after it) that lists the screenshot history. One word, no spaces or quotes; not one a web search or a favorite has."},
            Type::Text);
        e.field = &Settings::captureKeyword;
        e.check = [](const State& s, const QString& value, const Env& env) -> QString {
            if (value.isEmpty())
                return u"The keyword must not be empty."_s;
            if (!isValidWebKeyword(value))
                return u"The keyword must not have spaces or quotes."_s;
            if (const QString problem = textProblem(value, kMaxNameChars, u"The keyword"_s); !problem.isEmpty())
                return problem;
            const KeywordClaim claim = keywordClaim(value, s.settings.webShortcuts, -1, {}, env.keywordOwner);
            if (claim.by == KeywordClaim::By::WebShortcut)
                return u"The keyword '%1' is already the web search '%2'."_s.arg(value, claim.owner);
            if (claim.by == KeywordClaim::By::Favorite)
                return u"The keyword '%1' is already the favorite '%2'."_s.arg(value, claim.owner);
            return {};
        };
        add(std::move(e));
    }

    // ---- 粘贴为文件 (the 插件 page's card, beside 截图) ----
    add(boolean({"PasteFile/Enabled", QT_TRANSLATE_NOOP("AiSettings", "粘贴为文件"), "paste_file", 0,
                    "Pastes the words or the picture on the clipboard into a folder as a new file (.txt, .csv, .json, .html, .png and so on, told from what it holds), named after its first line or the time, ready to be renamed. Off until turned on."},
        &Settings::pasteFileEnabled));
    {
        // One of the two ways to it stays on (SettingsEditor, Settings::load): off is PasteFile/Enabled's.
        const auto oneStaysOn = [](bool Settings::* other) {
            return [other](const State& s, const QString& value, const Env&) -> QString {
                if (value == u"false" && !(s.settings.*other))
                    return u"Ctrl+V or the menu's verb stays on; to turn 粘贴为文件 off, set PasteFile/Enabled to false."_s;
                return {};
            };
        };
        Entry& ctrlV = add(boolean({"PasteFile/CtrlV", QT_TRANSLATE_NOOP("AiSettings", "在资源管理器和桌面按 Ctrl+V"), "paste_file", 0,
                                       "Ctrl+V in a File Explorer folder or on the desktop pastes words or a picture as a file. Copied files are left to Explorer's own paste. This or the menu's verb stays on."},
            &Settings::pasteFileCtrlV));
        ctrlV.check = oneStaysOn(&Settings::pasteFileMenu);
        Entry& menu = add(boolean({"PasteFile/Menu", QT_TRANSLATE_NOOP("AiSettings", "右键菜单里的“粘贴为文件”"), "paste_file", 0,
                                      "Adds 粘贴为文件 to the right-click menu of a folder's blank space and the desktop's (on Windows 11 under 显示更多选项). This or Ctrl+V stays on."},
            &Settings::pasteFileMenu));
        menu.check = oneStaysOn(&Settings::pasteFileCtrlV);
    }

    // ---- 高级 ----
    add(boolean({"Update/Automatic", QT_TRANSLATE_NOOP("AiSettings", "自动检查更新"), "advanced", -1,
                    "Looks for new versions of Win顺 on GitHub now and then."},
        &Settings::autoUpdate));
    {
        add(choice({"Launcher/Renderer", QT_TRANSLATE_NOOP("AiSettings", "界面绘制方式"), "advanced", -1,
                                  "How the windows are drawn. The graphics card (d3d11) is smoother and costs about 50 MB more memory; software is for machines with little memory. Needs Win顺 to restart."},
            &Settings::renderer,
            {{u"software"_s, u"drawn by the processor, less memory"_s, QT_TRANSLATE_NOOP("AiSettings", "处理器绘制")},
                {u"d3d11"_s, u"drawn by the graphics card, smoother"_s, QT_TRANSLATE_NOOP("AiSettings", "显卡加速")}},
            Effect::AppRestart));
    }

    // ---- never seen: paths and shares (what leaves the PC), AI's own switches, plugins, bookkeeping ----
    for (const char* key : {"Backup/Folder", "Index/Folder", "Capture/PrintScreenBefore", "Launcher/DialogAutoJumpAsked", "Index/FolderHintDismissed",
             "Launcher/FolderMenuOpusBefore", "Mcp/Enabled", "Mcp/AllowOpen", "Mcp/Clipboard", "Mcp/Screenshot",
             "Mcp/ExcludedPaths", "Mcp/Plugins", "Mcp/PluginActions", "Mcp/Settings", "Plugins/DeveloperMode",
             "Proxy/Mode", "Proxy/Address"}) // where every request goes: not for AI to turn
        add(hidden(key));
    return t;
}

// ---- keys ------------------------------------------------------------------------

// Sections and plugins' own sections are never AI's, whatever their keys.
bool neverVisible(const QString& key)
{
    return key.startsWith(u"Plugin.", Qt::CaseInsensitive) || key.startsWith(u"Mcp/", Qt::CaseInsensitive)
        || key.startsWith(u"Shares/", Qt::CaseInsensitive) || key.startsWith(u"Network/", Qt::CaseInsensitive);
}

// ---- values ----------------------------------------------------------------------

constexpr double kMegabyte = 1024.0 * 1024.0;

QJsonObject shortcutJson(const WebShortcut& w)
{
    return {{u"keyword"_s, w.keyword}, {u"name"_s, w.name}, {u"url"_s, w.url}, {u"home"_s, w.home}, {u"suggest"_s, w.suggest},
        {u"icon"_s, w.icon}};
}

WebShortcut shortcutOf(const QJsonObject& o)
{
    WebShortcut w;
    w.keyword = o.value(u"keyword"_s).toString();
    w.name = o.value(u"name"_s).toString();
    w.url = o.value(u"url"_s).toString();
    w.home = o.value(u"home"_s).toString();
    w.suggest = o.value(u"suggest"_s).toString();
    w.icon = o.value(u"icon"_s).toString();
    return w;
}

QStringList stringsOf(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& item : v.toArray())
        out.push_back(item.toString());
    return out;
}

// ---- lists -----------------------------------------------------------------------

QString folderKey(QString path)
{
    path = QDir::toNativeSeparators(path.trimmed());
    while (path.size() > 3 && path.endsWith(u'\\'))
        path.chop(1);
    if (path.size() == 2 && path[1] == u':')
        path += u'\\'; // "D:" is "D:\", the whole drive
    return path.toCaseFolded();
}

bool isFolderList(ListKind kind)
{
    return kind == ListKind::Folders || kind == ListKind::LocalFolders;
}

bool containsItem(const QStringList& list, const QString& item, ListKind kind)
{
    if (isFolderList(kind)) {
        const QString key = folderKey(item);
        return std::ranges::any_of(list, [&](const QString& other) { return folderKey(other) == key; });
    }
    return list.contains(item, Qt::CaseInsensitive);
}

void appendUnique(QStringList& list, const QString& item, ListKind kind)
{
    if (!containsItem(list, item, kind))
        list.push_back(item);
}

// One change as the model gave it: set the value (a list: replace it), or
// add items to a list, or take items out of it.
struct Change {
    enum class Kind { Set, Add, Remove };
    QString key;
    Kind kind = Kind::Set;
    QJsonValue value; // Set: the value; Add and Remove: an item or an array of them
};

// What one written item is as the settings window would keep it (SettingsEditor's
// add functions: their helpers are Settings'), possibly several (extensions);
// empty with `error` set when it is none.
QStringList normalizedItems(ListKind kind, const QString& raw, QString& error)
{
    switch (kind) {
    case ListKind::Programs: {
        static const QRegularExpression notInAFileName(u"[*?<>|\":]"_s);
        const QString app = Settings::programFileName(raw);
        if (app.isEmpty() || app.contains(notInAFileName)) {
            error = u"'%1' is not a program file name; write one like KeePass.exe."_s.arg(raw);
            return {};
        }
        return {app};
    }
    case ListKind::Extensions:
    case ListKind::ContentExtensions: {
        QStringList extensions = kind == ListKind::Extensions ? Settings::fileExtensions(raw) : Settings::contentExtensionList(raw);
        static const QRegularExpression notInAnExtension(u"[\\\\/:*?]"_s);
        extensions.removeIf([](const QString& ext) { return ext.contains(notInAnExtension); });
        if (extensions.isEmpty())
            error = u"'%1' is not a file extension; write one like pdf, without a dot."_s.arg(raw);
        return extensions;
    }
    case ListKind::FileNames: {
        const QStringList patterns = Settings::fileNamePatterns(raw);
        if (patterns.isEmpty())
            error = u"'%1' is not a file name; write one like ~$* or Thumbs.db, without a folder."_s.arg(raw);
        return patterns;
    }
    case ListKind::Names: {
        const QString name = Settings::excludedFolderName(raw);
        if (name.isEmpty()) {
            error = u"A name must not be empty."_s;
            return {};
        }
        return {name};
    }
    case ListKind::Folders:
    case ListKind::LocalFolders: {
        QString path = QDir::toNativeSeparators(raw.trimmed());
        const bool full = path.startsWith(u'%') || path.startsWith(u"\\\\"_s)
            || (path.size() >= 3 && path[0].isLetter() && path[1] == u':' && path[2] == u'\\');
        if (!full || path.contains(u"\\..\\"_s) || path.endsWith(u"\\.."_s)) {
            error = u"'%1' is not a full folder path; write one like D:\\Games."_s.arg(raw);
            return {};
        }
        if (kind == ListKind::LocalFolders && Settings::isNetworkPath(path)) {
            error = u"'%1' is a network place: shares are indexed by 网络共享 (设置 → 搜索范围 → 网络共享), which the user adds in the settings window."_s.arg(raw);
            return {};
        }
        while (path.size() > 3 && path.endsWith(u'\\'))
            path.chop(1);
        return {path};
    }
    }
    return {};
}

// The items as written, each plain, short, and normalised, in order. A list's
// own value (`wholeList`) is an array; add and remove may name one item.
QStringList parsedItems(const Entry& e, const QJsonValue& v, bool wholeList, QString& error)
{
    QStringList raw;
    if (v.isString() && !wholeList) {
        raw.push_back(v.toString());
    } else if (v.isString()) {
        error = u"A list's value is an array of strings; to put in one item and keep the rest use add."_s;
        return {};
    } else if (v.isArray()) {
        if (v.toArray().size() > kMaxItemsPerChange) {
            error = u"At most %1 items in one change."_s.arg(kMaxItemsPerChange);
            return {};
        }
        for (const QJsonValue& item : v.toArray()) {
            if (!item.isString()) {
                error = u"Items must be strings."_s;
                return {};
            }
            raw.push_back(item.toString());
        }
    } else {
        error = wholeList ? u"Must be an array of strings."_s : u"Must be a string or an array of strings."_s;
        return {};
    }
    const qsizetype longest = isFolderList(e.list) ? kMaxPathChars : kMaxNameChars;
    QStringList out;
    for (const QString& r : raw) {
        if (error = textProblem(r, longest, u"An item"_s); !error.isEmpty())
            return {};
        for (const QString& item : normalizedItems(e.list, r, error))
            appendUnique(out, item, e.list);
        if (!error.isEmpty())
            return {};
    }
    return out;
}

// ---- changing one value -------------------------------------------------------------

QString allowedChoices(const Entry& e)
{
    QStringList values;
    for (const Choice& c : e.choices)
        values.push_back(c.value);
    return values.join(u", "_s);
}

QString setList(const Entry& e, State& s, const Change& c)
{
    QString error;
    const QStringList given = parsedItems(e, c.value, c.kind == Change::Kind::Set, error);
    if (!error.isEmpty())
        return error;
    QStringList list = e.items(s);
    switch (c.kind) {
    case Change::Kind::Set:
        list = given;
        break;
    case Change::Kind::Add:
        for (const QString& item : given)
            appendUnique(list, item, e.list);
        break;
    case Change::Kind::Remove:
        list.removeIf([&](const QString& item) { return containsItem(given, item, e.list); });
        break;
    }
    e.setItems(s, list);
    return {};
}

QString addShortcut(State& s, const QJsonObject& o, const Env& env)
{
    WebShortcuts& shortcuts = s.settings.webShortcuts;
    static const QStringList known {u"keyword"_s, u"name"_s, u"url"_s, u"home"_s};
    for (const QString& field : o.keys()) {
        if (!known.contains(field))
            return u"Unknown field '%1' (use keyword, name, url, home)."_s.arg(field);
    }
    for (const QString& field : known) {
        if (o.contains(field) && !o.value(field).isString())
            return u"'%1' must be a string."_s.arg(field);
    }
    WebShortcut w;
    w.keyword = o.value(u"keyword"_s).toString().trimmed();
    if (w.keyword.isEmpty())
        return u"A web shortcut needs a keyword."_s;
    if (!isValidWebKeyword(w.keyword))
        return u"The keyword '%1' must not have spaces or quotes."_s.arg(w.keyword);
    if (const QString problem = textProblem(w.keyword, kMaxNameChars, u"The keyword"_s); !problem.isEmpty())
        return problem;
    const QString name = o.value(u"name"_s).toString().trimmed();
    if (const QString problem = textProblem(name, kMaxNameChars, u"The name"_s); !problem.isEmpty())
        return problem;
    for (const QString& field : {u"url"_s, u"home"_s}) {
        if (const QString problem = textProblem(o.value(field).toString(), kMaxPathChars, u"The "_s + field); !problem.isEmpty())
            return problem;
    }
    const KeywordClaim claim = keywordClaim(w.keyword, shortcuts, -1, s.settings.captureEnabled ? s.settings.captureKeyword : QString(),
        env.keywordOwner);
    switch (claim.by) {
    case KeywordClaim::By::WebShortcut:
        return u"The keyword '%1' is already the web search '%2'; remove that first to replace it."_s.arg(w.keyword, claim.owner);
    case KeywordClaim::By::Favorite:
        return u"The keyword '%1' is already the favorite '%2'."_s.arg(w.keyword, claim.owner);
    case KeywordClaim::By::Capture:
        return u"The keyword '%1' is already the screenshot history's."_s.arg(w.keyword);
    case KeywordClaim::By::Nobody:
        break;
    }
    // Only pages: a link of another scheme could start a program.
    const auto web = [](const QString& text) {
        const QString url = normalizeWebUrl(text);
        return url.startsWith(u"https://", Qt::CaseInsensitive) || url.startsWith(u"http://", Qt::CaseInsensitive) ? url : QString();
    };
    w.url = web(o.value(u"url"_s).toString());
    if (w.url.isEmpty())
        return u"The url must be an http:// or https:// address; put %s where the searched words go, or leave it out for a plain page."_s;
    w.home = web(o.value(u"home"_s).toString());
    if (w.home.isEmpty() && !o.value(u"home"_s).toString().trimmed().isEmpty())
        return u"The home must be an http:// or https:// address."_s;
    w.suggest = knownSuggestUrl(w.url);
    w.name = name.isEmpty() ? defaultWebShortcutName(w) : name; // the site's name, as the settings window does
    shortcuts.push_back(w);
    return {};
}

QString setShortcuts(State& s, const Change& c, const Env& env)
{
    if (c.kind == Change::Kind::Set)
        return u"Web shortcuts are changed with add or remove, not value."_s;
    const QJsonArray given = c.value.isArray() ? c.value.toArray() : QJsonArray {c.value};
    if (given.size() > kMaxItemsPerChange)
        return u"At most %1 shortcuts in one change."_s.arg(kMaxItemsPerChange);
    for (const QJsonValue& item : given) {
        if (c.kind == Change::Kind::Add) {
            if (!item.isObject())
                return u"Each shortcut to add is an object {keyword, name?, url, home?}."_s;
            if (const QString error = addShortcut(s, item.toObject(), env); !error.isEmpty())
                return error;
        } else {
            const QString keyword = (item.isObject() ? item.toObject().value(u"keyword"_s) : item).toString().trimmed();
            if (keyword.isEmpty())
                return u"Each shortcut to remove is its keyword."_s;
            if (const QString problem = textProblem(keyword, kMaxNameChars, u"The keyword"_s); !problem.isEmpty())
                return problem;
            s.settings.webShortcuts.removeIf([&](const WebShortcut& w) { return w.keyword.compare(keyword, Qt::CaseInsensitive) == 0; });
        }
    }
    return {};
}

// `value` is the new value, or what is added or taken out; empty `error`
// when it is applied.
QString applyChange(const Entry& e, State& s, const Change& c, const Env& env)
{
    if (e.type == Type::WebShortcuts)
        return setShortcuts(s, c, env);
    if (e.type == Type::List)
        return setList(e, s, c);
    if (c.kind != Change::Kind::Set)
        return u"This setting takes a value, not add or remove."_s;
    const QJsonValue& v = c.value;
    switch (e.type) {
    case Type::Bool:
        if (!v.isBool())
            return u"Must be true or false."_s;
        if (e.check) {
            if (const QString error = e.check(s, v.toBool() ? u"true"_s : u"false"_s, env); !error.isEmpty())
                return error;
        }
        e.setFlag(s, v.toBool());
        break;
    case Type::Choice: {
        if (!v.isString())
            return u"Must be one of: %1."_s.arg(allowedChoices(e));
        const QString given = v.toString().trimmed().toLower();
        const auto it = std::ranges::find_if(e.choices, [&](const Choice& ch) { return ch.value == given; });
        if (it == e.choices.end())
            return u"'%1' is not one of: %2."_s.arg(v.toString(), allowedChoices(e));
        if (e.check) {
            if (const QString error = e.check(s, it->value, env); !error.isEmpty())
                return error;
        }
        e.setText(s, it->value);
        break;
    }
    case Type::Int: {
        const QString range = e.zeroLabel ? u"0 (%1) or a whole number from %2 to %3"_s.arg(e.zeroMeaning).arg(int(e.min)).arg(int(e.max))
                                          : u"a whole number from %1 to %2"_s.arg(int(e.min)).arg(int(e.max));
        if (!v.isDouble() || v.toDouble() != std::floor(v.toDouble()))
            return u"Must be %1."_s.arg(range);
        const double d = v.toDouble();
        if (!((e.zeroLabel && d == 0) || (d >= e.min && d <= e.max)))
            return u"%1 is out of range: must be %2."_s.arg(d).arg(range);
        e.setInteger(s, static_cast<int>(d));
        break;
    }
    case Type::Number: {
        const double d = v.toDouble(std::nan(""));
        if (!v.isDouble() || !std::isfinite(d) || d < e.min || d > e.max)
            return u"Must be a number of MB from 0.001 (1 KB) to 4096."_s;
        e.setNumber(s, roundedSizeMB(d)); // to whole KB under 1 MB, else hundredths of a MB: what the settings window keeps
        break;
    }
    case Type::Text:
    case Type::Hotkey: {
        if (!v.isString())
            return u"Must be a string."_s;
        const QString text = v.toString().trimmed();
        if (e.type == Type::Hotkey && !text.isEmpty()) {
            // The settings window's recorder takes no more than this.
            if (!Settings::hotkeyAllowed(text))
                return u"'%1' is not a shortcut AI may set: hold Ctrl, Alt or Win with a key (like Alt+Space, Ctrl+Alt+D or Win+V), or use a function key F1 to F24. Bare keys and Shift with a letter are refused."_s.arg(text);
            if (env.hotkeyValid && !env.hotkeyValid(text))
                return u"'%1' is not a shortcut Win顺 can use; write one like Alt+Space, Ctrl+Alt+D or Win+V."_s.arg(text);
        }
        if (e.check) {
            if (const QString error = e.check(s, text, env); !error.isEmpty())
                return error;
        }
        e.setText(s, text);
        break;
    }
    case Type::List:
    case Type::WebShortcuts:
        break; // above
    }
    return {};
}

// ---- how a value reads to the user ------------------------------------------------

QString sizeText(double mb)
{
    if (mb < 1)
        return u"%1 KB"_s.arg(std::llround(mb * 1024));
    return u"%1 MB"_s.arg(QString::number(mb, 'f', mb == std::floor(mb) ? 0 : 2));
}

QString none()
{
    return QCoreApplication::translate("AiSettings", "（无）");
}

QString textOf(const Entry& e, const QJsonValue& v)
{
    switch (e.type) {
    case Type::Bool:
        return v.toBool() ? QCoreApplication::translate("AiSettings", "开") : QCoreApplication::translate("AiSettings", "关");
    case Type::Choice: {
        const auto it = std::ranges::find_if(e.choices, [&](const Choice& c) { return c.value == v.toString(); });
        return it != e.choices.end() ? translated(it->label) : v.toString();
    }
    case Type::Int:
        return e.zeroLabel && v.toInt() == 0 ? translated(e.zeroLabel) : QString::number(v.toInt());
    case Type::Number:
        return sizeText(v.toDouble());
    case Type::Text:
    case Type::Hotkey:
        return v.toString().isEmpty() ? none() : v.toString();
    case Type::List: {
        const QStringList list = stringsOf(v);
        return list.isEmpty() ? none() : list.join(u", "_s);
    }
    case Type::WebShortcuts: {
        QStringList keywords;
        for (const QJsonValue& w : v.toArray())
            keywords.push_back(w.toObject().value(u"keyword"_s).toString());
        return keywords.isEmpty() ? none() : keywords.join(u", "_s);
    }
    }
    return {};
}

// "+ a, b" or "− c, 还有 2 项": the items while they fit a few lines, then
// how many more; nothing for none.
QString summary(const QString& mark, const QStringList& items)
{
    if (items.isEmpty())
        return {};
    constexpr qsizetype budget = 160;
    QStringList shown;
    qsizetype used = 0;
    for (const QString& item : items) {
        const QString text = item.size() > budget ? item.left(budget - 1) + u"…"_s : item;
        if (!shown.isEmpty() && used + text.size() > budget)
            break;
        shown.push_back(text);
        used += text.size() + 2;
    }
    QString out = mark + u' ' + shown.join(u", "_s);
    if (const qsizetype rest = items.size() - shown.size(); rest > 0)
        out += u"，"_s + QCoreApplication::translate("AiSettings", "还有 %n 项", nullptr, static_cast<int>(rest));
    return out;
}

// What a web shortcut says of itself: its keyword, name and addresses, all of
// what is applied, so that the card hides nothing.
QString shortcutText(const QJsonObject& o)
{
    QString text = u"%1 (%2) → %3"_s.arg(o.value(u"keyword"_s).toString(), o.value(u"name"_s).toString(), o.value(u"url"_s).toString());
    if (const QString home = o.value(u"home"_s).toString(); !home.isEmpty())
        text += u" · "_s + QCoreApplication::translate("AiSettings", "主页 %1").arg(home);
    return text;
}

// What a list or the web shortcuts lost and gained, as two short lines (items
// whose name is there before and after are neither); empty for the rest.
void fillDiff(Item& item)
{
    const Entry& e = *item.entry;
    QStringList added;
    QStringList removed;
    if (e.type == Type::List) {
        const QStringList was = stringsOf(item.oldValue);
        const QStringList now = stringsOf(item.newValue);
        for (const QString& entry : now) {
            if (!was.contains(entry))
                added.push_back(entry);
        }
        for (const QString& entry : was) {
            if (!now.contains(entry))
                removed.push_back(entry);
        }
    } else if (e.type == Type::WebShortcuts) {
        const auto objects = [](const QJsonValue& v) {
            QList<QJsonObject> out;
            for (const QJsonValue& w : v.toArray())
                out.push_back(w.toObject());
            return out;
        };
        const auto has = [](const QList<QJsonObject>& list, const QString& keyword) {
            return std::ranges::any_of(list, [&](const QJsonObject& o) { return o.value(u"keyword"_s).toString() == keyword; });
        };
        const QList<QJsonObject> was = objects(item.oldValue);
        const QList<QJsonObject> now = objects(item.newValue);
        for (const QJsonObject& o : now) {
            if (!has(was, o.value(u"keyword"_s).toString()))
                added.push_back(shortcutText(o));
        }
        for (const QJsonObject& o : was) {
            if (!has(now, o.value(u"keyword"_s).toString()))
                removed.push_back(u"%1 (%2)"_s.arg(o.value(u"keyword"_s).toString(), o.value(u"name"_s).toString()));
        }
    } else {
        return;
    }
    item.removedText = summary(u"−"_s, removed);
    item.addedText = summary(u"+"_s, added);
    item.diffText = QStringList {item.removedText, item.addedText}.join(u'\n').trimmed();
}

// Whether `after` shows AI more than `before` did.
bool widensAccess(const Entry& e, const QJsonValue& before, const QJsonValue& after)
{
    switch (e.widens) {
    case Widens::Shrink: {
        const QStringList was = stringsOf(before);
        const QStringList now = stringsOf(after);
        return std::ranges::any_of(was, [&](const QString& item) { return !containsItem(now, item, e.list); });
    }
    case Widens::TurnOn:
        return !before.toBool() && after.toBool();
    case Widens::Grow: {
        const QStringList was = stringsOf(before);
        const QStringList now = stringsOf(after);
        return std::ranges::any_of(now, [&](const QString& item) { return !containsItem(was, item, e.list); });
    }
    case Widens::No:
        break;
    }
    return false;
}

QString typeName(Type t)
{
    switch (t) {
    case Type::Bool:
        return u"bool"_s;
    case Type::Choice:
        return u"choice"_s;
    case Type::Int:
        return u"integer"_s;
    case Type::Number:
        return u"number"_s;
    case Type::Text:
        return u"text"_s;
    case Type::Hotkey:
        return u"hotkey"_s;
    case Type::List:
        return u"list"_s;
    case Type::WebShortcuts:
        return u"web_shortcuts"_s;
    }
    return {};
}

QString listItemsName(ListKind k)
{
    switch (k) {
    case ListKind::Programs:
        return u"program file names such as KeePass.exe"_s;
    case ListKind::Extensions:
    case ListKind::ContentExtensions:
        return u"file extensions without the dot, such as pdf"_s;
    case ListKind::Names:
        return u"folder names such as node_modules"_s;
    case ListKind::FileNames:
        return u"file names or patterns with * and ? such as ~$*"_s;
    case ListKind::Folders:
        return u"full folder paths such as D:\\Games"_s;
    case ListKind::LocalFolders:
        return u"full paths of folders on this PC's own disks such as D:\\Work (no network paths)"_s;
    }
    return {};
}

} // namespace

// ---- the table's public face ---------------------------------------------------------

bool Entry::flag(const State& s) const
{
    if (const auto* member = std::get_if<bool Settings::*>(&field))
        return s.settings.*(*member);
    return s.*std::get<bool State::*>(field);
}

const QString& Entry::text(const State& s) const
{
    return s.settings.*std::get<QString Settings::*>(field);
}

int Entry::integer(const State& s) const
{
    return s.settings.*std::get<int Settings::*>(field);
}

double Entry::number(const State& s) const
{
    return s.settings.contentMaxSizeMB[std::get<SizeLimit>(field).kind];
}

const QStringList& Entry::items(const State& s) const
{
    return s.settings.*std::get<QStringList Settings::*>(field);
}

const WebShortcuts& Entry::shortcuts(const State& s) const
{
    return s.settings.*std::get<WebShortcuts Settings::*>(field);
}

void Entry::setFlag(State& s, bool value) const
{
    if (const auto* member = std::get_if<bool Settings::*>(&field))
        s.settings.*(*member) = value;
    else
        s.*std::get<bool State::*>(field) = value;
}

void Entry::setText(State& s, const QString& value) const
{
    s.settings.*std::get<QString Settings::*>(field) = value;
}

void Entry::setInteger(State& s, int value) const
{
    s.settings.*std::get<int Settings::*>(field) = value;
}

void Entry::setNumber(State& s, double value) const
{
    s.settings.contentMaxSizeMB[std::get<SizeLimit>(field).kind] = value;
}

void Entry::setItems(State& s, const QStringList& value) const
{
    const auto member = std::get<QStringList Settings::*>(field);
    if (member == &Settings::indexFolders && s.settings.*member != value)
        s.settings.indexFoldersChosen = true; // set now, as the file will have it
    s.settings.*member = value;
}

void Entry::setShortcuts(State& s, const WebShortcuts& value) const
{
    s.settings.*std::get<WebShortcuts Settings::*>(field) = value;
}

QString Entry::titleText() const
{
    return title ? translated(title) : key;
}

QString Entry::rowTitleText() const
{
    if (rowTitle)
        return *rowTitle ? translated(rowTitle) : QString();
    return title ? translated(title) : QString();
}

QString Section::titleText() const
{
    return translated(title);
}

const std::vector<Section>& sections()
{
    static const std::vector<Section> all {
        {u"shortcuts"_s, QT_TRANSLATE_NOOP("AiSettings", "快捷键与双击 Ctrl、Alt 或 Shift"), 0},
        {u"taskbar"_s, QT_TRANSLATE_NOOP("AiSettings", "任务栏"), 0},
        {u"files_and_dialogs"_s, QT_TRANSLATE_NOOP("AiSettings", "文件管理器与对话框"), 0},
        {u"startup"_s, QT_TRANSLATE_NOOP("AiSettings", "启动与历史"), 0},
        {u"appearance"_s, QT_TRANSLATE_NOOP("AiSettings", "外观"), 1},
        {u"scope"_s, QT_TRANSLATE_NOOP("AiSettings", "搜索范围"), 2},
        {u"results"_s, QT_TRANSLATE_NOOP("AiSettings", "结果列表"), 3},
        {u"content"_s, QT_TRANSLATE_NOOP("AiSettings", "文件内容搜索"), 4},
        {u"web"_s, QT_TRANSLATE_NOOP("AiSettings", "网页搜索"), 5},
        {u"favorites"_s, QT_TRANSLATE_NOOP("AiSettings", "收藏夹"), 6},
        {u"clipboard"_s, QT_TRANSLATE_NOOP("AiSettings", "剪贴板"), 7},
        {u"advanced"_s, QT_TRANSLATE_NOOP("AiSettings", "高级"), 8},
        {u"capture"_s, QT_TRANSLATE_NOOP("AiSettings", "截图"), 10},
        {u"paste_file"_s, QT_TRANSLATE_NOOP("AiSettings", "粘贴为文件"), 10},
    };
    return all;
}

const Section* sectionOf(const QString& id)
{
    for (const Section& s : sections()) {
        if (s.id.compare(id, Qt::CaseInsensitive) == 0)
            return &s;
    }
    return nullptr;
}

const std::vector<Entry>& catalog()
{
    static const std::vector<Entry> all = buildCatalog();
    return all;
}

const Entry* find(const QString& key)
{
    const QString wanted = key.trimmed();
    for (const Entry& e : catalog()) {
        if (e.key.compare(wanted, Qt::CaseInsensitive) == 0)
            return &e;
    }
    return nullptr;
}

const char* effectName(Effect effect)
{
    switch (effect) {
    case Effect::Now:
        return "now";
    case Effect::ExplorerRestart:
        return "explorer_restart";
    case Effect::AppRestart:
        return "app_restart";
    }
    return "now";
}

QJsonValue valueOf(const Entry& e, const State& s)
{
    switch (e.type) {
    case Type::Bool:
        return e.flag(s);
    case Type::Choice:
    case Type::Text:
    case Type::Hotkey:
        return e.text(s);
    case Type::Int:
        return e.integer(s);
    case Type::Number:
        return e.number(s);
    case Type::List:
        return QJsonArray::fromStringList(e.items(s));
    case Type::WebShortcuts: {
        QJsonArray out;
        for (const WebShortcut& w : e.shortcuts(s))
            out.push_back(shortcutJson(w));
        return out;
    }
    }
    return {};
}

void restore(const Entry& e, State& s, const QJsonValue& v)
{
    switch (e.type) {
    case Type::Bool:
        e.setFlag(s, v.toBool());
        break;
    case Type::Choice:
    case Type::Text:
    case Type::Hotkey:
        e.setText(s, v.toString());
        break;
    case Type::Int:
        e.setInteger(s, v.toInt());
        break;
    case Type::Number:
        e.setNumber(s, v.toDouble());
        break;
    case Type::List:
        e.setItems(s, stringsOf(v));
        break;
    case Type::WebShortcuts: {
        WebShortcuts list;
        for (const QJsonValue& w : v.toArray())
            list.push_back(shortcutOf(w.toObject()));
        e.setShortcuts(s, list);
        break;
    }
    }
}

// ---- reading ---------------------------------------------------------------------------

namespace {

QJsonObject entryJson(const Entry& e, const State& current, const State& defaults, bool hasDefault)
{
    QJsonObject o {{u"key"_s, e.key}, {u"title"_s, e.titleText()}, {u"section"_s, e.section}, {u"type"_s, typeName(e.type)},
        {u"value"_s, valueOf(e, current)}, {u"writable"_s, e.writable()},
        {u"effect"_s, QString::fromLatin1(effectName(e.effect))}, {u"about"_s, e.about}};
    if (hasDefault)
        o.insert(u"default"_s, valueOf(e, defaults));
    if (e.access == Access::AlwaysAsk || e.widens != Widens::No) {
        o.insert(u"ask_always"_s, e.access == Access::AlwaysAsk ? u"every change asks the user, even when AI may change settings by itself"_s
                : e.widens == Widens::Shrink                    ? u"taking items out asks the user, even when AI may change settings by itself"_s
                : e.widens == Widens::Grow                      ? u"putting items in asks the user, even when AI may change settings by itself"_s
                                                                : u"turning it on asks the user, even when AI may change settings by itself"_s);
    }
    if (e.type == Type::Choice) {
        QJsonArray choices;
        for (const Choice& c : e.choices)
            choices.push_back(QJsonObject {{u"value"_s, c.value}, {u"meaning"_s, c.meaning}, {u"label"_s, translated(c.label)}});
        o.insert(u"choices"_s, choices);
    }
    if (e.type == Type::Int || e.type == Type::Number) {
        o.insert(u"min"_s, e.min);
        o.insert(u"max"_s, e.max);
        if (e.type == Type::Number)
            o.insert(u"unit"_s, u"MB"_s);
        if (!e.zeroMeaning.isEmpty())
            o.insert(u"zero_means"_s, e.zeroMeaning);
    }
    if (e.type == Type::List)
        o.insert(u"items_are"_s, listItemsName(e.list));
    return o;
}

// Every word in the haystack (case folded): the key, titles in both
// languages, section, description, choices.
bool matchesWords(const Entry& e, const QStringList& words)
{
    QString hay = e.key + u' ' + QString::fromUtf8(e.title ? e.title : "") + u' ' + e.titleText() + u' ' + e.section + u' ' + e.about;
    for (const Choice& c : e.choices)
        hay += u' ' + c.value + u' ' + c.meaning + u' ' + QString::fromUtf8(c.label) + u' ' + translated(c.label);
    hay = hay.toCaseFolded();
    return std::ranges::all_of(words, [&](const QString& w) { return hay.contains(w); });
}

bool inSections(const Entry& e, const QStringList& wanted)
{
    if (wanted.isEmpty())
        return true;
    const Section* section = sectionOf(e.section);
    return std::ranges::any_of(wanted, [&](const QString& w) {
        const QString t = w.trimmed();
        return e.section.compare(t, Qt::CaseInsensitive) == 0
            || (section && (QString::fromUtf8(section->title).contains(t) || section->titleText().contains(t, Qt::CaseInsensitive)));
    });
}

} // namespace

QJsonObject describe(const State& current, const Query& query)
{
    State defaults;
    defaults.settings = Settings::defaults();
    QStringList words;
    for (const QString& w : query.words.toCaseFolded().split(QRegularExpression(u"\\s+"_s), Qt::SkipEmptyParts))
        words.push_back(w);
    QStringList keys;
    for (const QString& k : query.keys)
        keys.push_back(k.trimmed().toCaseFolded());

    QJsonArray list;
    QStringList used;
    for (const Entry& e : catalog()) {
        if (!visible(e) || neverVisible(e.key))
            continue;
        if (!keys.isEmpty() && !keys.contains(e.key.toCaseFolded()))
            continue;
        if (!inSections(e, query.sections) || !matchesWords(e, words))
            continue;
        // Autostart is no setting of the file: no default of Settings to compare with.
        const bool hasDefault = e.key != u"Launcher/Autostart";
        const QJsonObject o = entryJson(e, current, defaults, hasDefault);
        if (query.changedOnly && (!hasDefault || o.value(u"value"_s) == o.value(u"default"_s)))
            continue;
        list.push_back(o);
        if (!used.contains(e.section))
            used.push_back(e.section);
    }
    QJsonArray sectionList;
    for (const Section& s : sections()) {
        if (used.contains(s.id))
            sectionList.push_back(QJsonObject {{u"id"_s, s.id}, {u"title"_s, s.titleText()}});
    }
    return {{u"settings"_s, list}, {u"sections"_s, sectionList}};
}

// ---- changing ----------------------------------------------------------------------------

bool Plan::changed() const
{
    return std::ranges::any_of(items, [](const Item& i) { return i.changed; });
}

bool Plan::askAlways() const
{
    return std::ranges::any_of(items, [](const Item& i) { return i.changed && i.askAlways; });
}

QJsonObject itemJson(const Item& item)
{
    const Entry& e = *item.entry;
    const Section* section = sectionOf(e.section);
    return {{u"key"_s, e.key}, {u"title"_s, e.titleText()}, {u"row_title"_s, e.rowTitleText()}, {u"section"_s, e.section},
        {u"page"_s, section ? section->page : -1}, {u"tab"_s, e.tab}, {u"old"_s, item.oldValue}, {u"new"_s, item.newValue},
        {u"old_text"_s, item.oldText}, {u"new_text"_s, item.newText}, {u"removed_text"_s, item.removedText},
        {u"added_text"_s, item.addedText}, {u"diff_text"_s, item.diffText}, {u"changed"_s, item.changed},
        {u"ask_always"_s, item.askAlways}, {u"effect"_s, QString::fromLatin1(effectName(e.effect))},
        {u"is_list"_s, e.type == Type::List || e.type == Type::WebShortcuts}};
}

namespace {

// {key, value | add | remove} as the model wrote it.
QString parseChange(const QJsonObject& o, Change& change)
{
    static const QStringList fields {u"key"_s, u"value"_s, u"add"_s, u"remove"_s};
    for (const QString& f : o.keys()) {
        if (!fields.contains(f))
            return u"Unknown field '%1': a change is {key, value} or {key, add} or {key, remove}."_s.arg(f);
    }
    if (!o.value(u"key"_s).isString() || o.value(u"key"_s).toString().trimmed().isEmpty())
        return u"Every change needs a 'key' (from get_settings)."_s;
    change.key = o.value(u"key"_s).toString().trimmed();
    const int given = int(o.contains(u"value"_s)) + int(o.contains(u"add"_s)) + int(o.contains(u"remove"_s));
    if (given != 1)
        return u"A change has exactly one of 'value', 'add' or 'remove'."_s;
    if (o.contains(u"value"_s)) {
        change.kind = Change::Kind::Set;
        change.value = o.value(u"value"_s);
    } else if (o.contains(u"add"_s)) {
        change.kind = Change::Kind::Add;
        change.value = o.value(u"add"_s);
    } else {
        change.kind = Change::Kind::Remove;
        change.value = o.value(u"remove"_s);
    }
    return {};
}

} // namespace

namespace {

// Win顺's own shortcuts, as they are after the batch, are all different: a key
// taken twice would be held by the first only, and the other setting would
// look set and do nothing.
void checkHotkeysDiffer(Plan& plan)
{
    static const QStringList own {u"Launcher/Hotkey"_s, u"Launcher/FolderMenuHotkey"_s, u"Clipboard/Hotkey"_s, u"Capture/Hotkey"_s};
    for (const Item& item : plan.items) {
        const QString wanted = Settings::normalizedHotkey(item.newValue.toString());
        if (!item.changed || item.entry->type != Type::Hotkey || wanted.isEmpty())
            continue;
        for (const QString& key : own) {
            const Entry* other = find(key);
            if (!other || key.compare(item.entry->key, Qt::CaseInsensitive) == 0)
                continue;
            if (key == u"Capture/Hotkey" && !plan.result.settings.captureEnabled)
                continue; // not in use
            if (Settings::normalizedHotkey(other->text(plan.result)) == wanted) {
                plan.problems.push_back(u"%1: '%2' is already the shortcut of '%3' (%4); a shortcut can have one use."_s.arg(
                    item.entry->key, item.newValue.toString(), other->titleText(), key));
                break;
            }
        }
    }
}

} // namespace

Plan makePlan(const State& current, const QJsonArray& changes, const Env& env)
{
    Plan plan;
    plan.result = current;
    if (changes.isEmpty())
        plan.problems.push_back(u"No changes given."_s);
    QStringList seen;
    int n = 0;
    for (const QJsonValue& value : changes) {
        const QString at = u"change %1"_s.arg(++n);
        if (!value.isObject()) {
            plan.problems.push_back(u"%1: must be an object {key, value}."_s.arg(at));
            continue;
        }
        Change change;
        if (const QString error = parseChange(value.toObject(), change); !error.isEmpty()) {
            plan.problems.push_back(u"%1: %2"_s.arg(at, error));
            continue;
        }
        const Entry* e = find(change.key);
        if (!e || !visible(*e) || neverVisible(e->key)) { // a hidden key is told as one that does not exist
            plan.problems.push_back(u"%1: unknown setting '%2'; keys come from get_settings."_s.arg(at, change.key));
            continue;
        }
        if (!e->writable()) {
            plan.problems.push_back(u"%1 (%2): can only be read, not changed by AI; the user changes it in the settings window."_s.arg(at, e->key));
            continue;
        }
        if (seen.contains(e->key, Qt::CaseInsensitive)) {
            plan.problems.push_back(u"%1 (%2): the same setting twice in one call; combine them."_s.arg(at, e->key));
            continue;
        }
        seen.push_back(e->key);
        Item item;
        item.entry = e;
        item.oldValue = valueOf(*e, current);
        if (const QString error = applyChange(*e, plan.result, change, env); !error.isEmpty()) {
            plan.problems.push_back(u"%1 (%2): %3"_s.arg(at, e->key, error));
            continue;
        }
        item.newValue = valueOf(*e, plan.result);
        item.changed = item.oldValue != item.newValue;
        if (item.changed && e->after)
            e->after(plan.result); // what the settings window's own setter does besides
        item.oldText = textOf(*e, item.oldValue);
        item.newText = textOf(*e, item.newValue);
        fillDiff(item);
        item.askAlways = e->access == Access::AlwaysAsk || (item.changed && widensAccess(*e, item.oldValue, item.newValue));
        plan.items.push_back(std::move(item));
    }
    checkHotkeysDiffer(plan);
    return plan;
}

} // namespace ws::aisettings
