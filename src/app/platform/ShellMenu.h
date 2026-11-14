#pragma once

#include <QList>
#include <QString>

// Explorer's right-click verbs, run by WinShunSearch.exe (src/stub):
// "用 Win顺 搜索此处" (GitHub #20) on a folder, in a folder's empty space,
// and on a drive (--search-here "<folder>", which hands the folder to
// Win顺); "粘贴为文件" in a folder's empty space and the desktop's
// (--paste-file "<folder>", which pastes the clipboard there as a file
// without Win顺). Classic verbs under HKCU\Software\Classes: no
// administrator needed, the same for an installed and a portable copy; on
// Windows 11 they are under "显示更多选项" (Shift+right-click). They name the
// copy that set them; a copy moved or updated sets them again at its next
// start.
namespace ws::shellmenu {

// Sets the verbs, titled in the language Windows is shown in (not Win顺's own:
// they stand among Explorer's, core/ExplorerLanguage.h), pointing at the
// WinShunSearch.exe beside this program (`on`), or removes the ones this copy
// set (`!on`; another copy's stay). Writes only what differs; Explorer is told
// when anything did.
void apply(bool on);

// The same for 粘贴为文件.
void applyPasteFile(bool on);

// 贴到屏幕（Win顺）on a picture file (GitHub #128; --pin-picture "<file>", which hands the file
// to Win顺 for the screenshot plugin to pin). Under SystemFileAssociations\image: every type
// Windows knows as a picture, whichever program opens it. With several chosen Explorer runs it
// once a file, and each is pinned. There while a plugin pins pictures.
void applyPinPicture(bool on);

// Win顺 命令 › on a file or a folder (GitHub #61): the user's own commands that ask to be there
// (Favorite::fileMenu), each run with the path as its {query}: WinShunSearch.exe --run-command
// <id> "<path>", which hands both to Win顺. A cascade under *\shell and Directory\shell, made
// again when the commands differ from what is written; none: the entry goes.
struct MenuCommand {
    QString id; // what follows "winshun-command:" in the favorite's path
    QString title; // its name
    friend bool operator==(const MenuCommand&, const MenuCommand&) = default;
};
void applyCommands(const QList<MenuCommand>& commands);

// 发送到 › 局域网传输（Win顺）(GitHub #180): a shortcut in the user's SendTo folder to this copy's
// WinShunSearch.exe --lan-send, which Explorer runs once with every file chosen (a verb would run
// once a file). There while the LAN transfer plugin is on; `!on` removes this copy's, and one
// whose program is gone.
void applyLanSend(bool on);

} // namespace ws::shellmenu
