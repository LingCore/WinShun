#pragma once

#include "FileManagers.h"
#include "Win32Util.h"

#include <QString>
#include <QStringList>

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Thin wrappers over Explorer/Shell behaviour.
namespace ws::shell {

// These run on a short-lived worker thread: a slow shell extension or an
// unreachable drive must never freeze the launcher. Without
// `asAdministrator`, targets start with the user's normal rights, never
// elevated: when that is impossible (no shell to start them), they are not
// started at all. `done` runs on the worker thread with whether it worked.
void open(const QString& path, bool asAdministrator = false, std::function<void(bool)> done = {});
void reveal(const QString& path); // open the folder and select the item
// The same for several items: one window per folder, with all of its items selected.
void reveal(const QStringList& paths);
// Always in Explorer, whatever setFileManager() chose: for its context menu
// (Pin to taskbar).
void revealInExplorer(const QString& path);
// The file manager open() takes folders to and reveal() shows items in
// (Settings::fileManager): Explorer at first. Explorer takes over when the
// one chosen does not start.
void setFileManager(filemanager::Kind kind);
filemanager::Kind fileManager(); // the one set
// Starts `program` with the user's normal rights (the desktop shell's while
// Win顺 is elevated, its own otherwise), never elevated, for a program to
// wait for (see filemanager); the others start through open() and run().
// `cwd`: its working folder; empty for ours. Invalid when Win顺 is elevated
// and no shell runs to take them from.
win32::UniqueHandle startUnelevated(const std::wstring& program, const std::wstring& args, const std::wstring& cwd = {});
// Starts `program` as administrator (UAC asks, unless Win顺 is elevated
// already), without waiting: false when it did not start (the prompt
// declined, say). `cwd` as above.
bool startElevated(const std::wstring& program, const std::wstring& args, const std::wstring& cwd = {});
// The same, waited for on a worker thread: `done` runs there with its exit
// code, or nothing when it did not start (WinShunService.exe --install).
void runElevated(const std::wstring& program, const std::wstring& args,
    std::function<void(std::optional<unsigned long>)> done);
void openUrl(const QString& url); // https:, mailto: ...
// A command prompt (cmd.exe; in Windows Terminal when that is the default)
// in `folder`, with the user's normal rights. On a network share, pushd
// gives it a drive letter: cmd cannot start in a UNC path.
void openTerminal(const QString& folder, std::function<void(bool)> done = {});
// A command or shell location ("%windir%\system32\control.exe /name X",
// "ms-settings:display"): a place in Windows (see SystemCatalog).
void run(const QString& command, std::function<void(bool)> done = {});
// User-defined command. Expands environment variables before inserting the
// query as one argument; never implicitly starts cmd.exe or PowerShell.
void runCustom(const QString& command, const QString& directory, const QString& query,
    std::function<void(bool)> done = {});
// An installed app by its launch path, "shell:AppsFolder\<id>" (see AppCatalog).
void launchApp(const QString& launchPath, bool asAdministrator = false, std::function<void(bool)> done = {});
// Moves the items to the Recycle Bin, in one operation (asks before deleting
// permanently what cannot be recycled). `done` runs on the worker thread with
// whether the shell reported success; `owner` parents any shell dialog.
void recycle(const QStringList& paths, HWND owner, std::function<void(bool)> done);

void copyText(const QString& text);
void copyFiles(const QStringList& paths); // paste-able in Explorer

bool canRunAsAdministrator(const QString& path);

struct MenuItem {
    int id = 0; // 0 with empty text = separator
    QString text;
    bool checked = false;
    bool enabled = true;
    bool isDefault = false;
    std::vector<MenuItem> children; // a submenu: `text` opens it, `id` is not chosen

    static MenuItem separator() { return {}; }
};

// Native popup menu at a screen position (physical pixels). Returns the
// chosen id, or 0 when dismissed. `shown` runs once the menu is on screen.
int popupMenu(HWND owner, const std::vector<MenuItem>& items, POINT screenPos, std::function<void()> shown = {});
// Shell extensions that come in with the first shell icon may hook popup
// menus and set themselves up on the first one (Nilesoft Shell: ~350 ms).
// Makes that first call now, on a thread of its own, and one Windows refuses:
// nothing shows. Once per process; later calls return at once.
void prepareMenuHooks();

// The standard "select folder" dialog (modal). Returns an empty string when cancelled.
QString pickFolder(HWND owner, const QString& title);
// The standard "open file" dialog (modal), for one file of the kinds in
// `filters` ({"插件压缩包", "*.zip;*.wsplugin"} pairs). Empty when cancelled.
QString pickFile(HWND owner, const QString& title, const std::vector<std::pair<QString, QString>>& filters);
// The standard "save as" dialog (modal), starting at `fileName`, for a file
// of one kind ("网页搜索", "json"); asks before replacing one. Empty when cancelled.
QString pickSaveFile(HWND owner, const QString& title, const QString& fileName, const QString& filterName,
    const QString& extension);

} // namespace ws::shell

namespace ws::autostart {

// From what was last read: at once, without asking Task Scheduler again.
bool isEnabled();
void refresh(); // reads the task again, in the background (changed outside WinShun?)
bool isSetUp(); // enabled for any copy of WinShun, this one or another
void setEnabled(bool enabled);
// Whether the task written from now on starts Win顺 elevated (the 管理员直读
// mode on an administrator's account, GitHub #21) or with the user's normal
// rights (at first). Only an elevated process can register the former:
// Windows refuses it to the others, as it would be a way around UAC.
void setElevated(bool elevated);
bool elevated();
// Whether the task that starts this copy runs it elevated; nothing without
// an enabled one of this copy's (App::start compares it with the mode).
std::optional<bool> taskElevated();
// The task still starts a program that is gone (the portable copy, after
// installing with Setup): point it at this one. A copy that still exists
// elsewhere keeps it.
void adoptIfOrphaned();
void migrateFromQuickFind(); // the autostart of versions still called QuickFind (see Migration.h)

} // namespace ws::autostart
