#pragma once

#include <QStringList>

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

// The shell's own context menu for files and folders (IContextMenu: what
// Explorer shows, with every shell extension; on Windows 11 the one under
// "显示更多选项"), for Shift+right-click on a result (GitHub #26). Not
// ShellMenu.h, which is Explorer's "用 Win顺 搜索此处" verb.
namespace ws::shellcontext {

struct Choice {
    bool chosen = false; // an item was picked (else the menu was dismissed: Esc, a click elsewhere)
    bool invoked = false; // ... and its command ran (InvokeCommand succeeded)
    // Its canonical verb, lower case ("open", "copy", "delete", "properties");
    // empty when the handler names none (most third-party items).
    std::wstring verb;
    bool shift = false; // held as it was picked (Shift+删除 deletes for good)
    bool ctrl = false;
    // True when `paths` had several items and the shell gave no menu for
    // them together: the menu was the first item's alone.
    bool singleFallback = false;
};

// Shows the menu for `paths` (in one folder or several, as Explorer's
// search results have one for items of several folders: 比较 of a diff
// tool, 复制, 删除, 属性 are for all of them, GitHub #464; the first the one
// asked on, which alone gets a menu should the shell give none for them
// together) at `pos` (on the screen, physical pixels) and runs its loop
// here, on the GUI thread (the
// handlers want the apartment the shell gave this thread, and the owner
// must be this thread's). `owner`: the launcher's window, in front; the
// menu neither activates nor takes the focus from it, and its submenus
// (发送到, 打开方式) fill in through messages to it. `extended`: with the
// verbs Explorer adds for Shift+right-click. `picked` runs once an item was
// chosen and the menu is gone, before its command: the caller decides
// there what becomes of the launcher meanwhile (the command may show
// dialogs). Returns once the command, if any, was run.
Choice showMenu(const QStringList& paths, HWND owner, POINT pos, bool extended,
    const std::function<void(const Choice&)>& picked = {});

// Without the menu on screen (the tests): the verbs the items of the menu
// for `paths` name, lower case, in its order (most extensions' items name
// none), and whether it is the first item's alone; and one of them run
// with no questions asked, `shift` held (删除 then deletes for good).
struct Offer {
    QStringList verbs;
    bool singleFallback = false;
};
Offer offered(const QStringList& paths);
bool invokeQuietly(const QStringList& paths, std::wstring_view verb, bool shift = false);

// From the keyboard, the shell's menu is Shift+Menu or Ctrl+Shift+F10, and
// Win顺's own menu the Menu key or Shift+F10. Qt tells the launcher of the
// Menu key and of Shift+F10 only as a context-menu event, which does not say
// which key it was; and with a Chinese IME in its Chinese mode the Menu key's
// press reaches Qt as VK_PROCESSKEY (the IME's), so no key event comes at
// all, only the release's context-menu event. watchKeys subclasses the
// launcher's window (Qt's native event filters are not given key messages)
// and keeps the last key pressed, the IME's seen through (by its scan code);
// lastKeyPressed is its virtual key (VK_APPS, VK_F10), 0 before any. The
// window is also the one a command's dialogs give the focus back to.
void watchKeys(HWND window);
UINT lastKeyPressed();

// Whether the launcher stays open after a command with this verb: those
// that leave the user where they were (copying, cutting, deleting, pinning),
// as Win顺's own menu does; everything else opens something (a program,
// a dialog, a window), and the launcher goes, as for its own 打开.
bool keepsLauncher(std::wstring_view verb);

} // namespace ws::shellcontext
