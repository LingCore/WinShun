#pragma once

#include <QString>
#include <QStringList>

#include <windows.h>

#include <functional>
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
void openUrl(const QString& url); // https:, mailto: ...
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

    static MenuItem separator() { return {}; }
};

// Native popup menu at a screen position (physical pixels). Returns the
// chosen id, or 0 when dismissed. `shown` runs once the menu is on screen.
int popupMenu(HWND owner, const std::vector<MenuItem>& items, POINT screenPos, std::function<void()> shown = {});

// The standard "select folder" dialog (modal). Returns an empty string when cancelled.
QString pickFolder(HWND owner, const QString& title);

} // namespace ws::shell

namespace ws::autostart {

// From what was last read: at once, without asking Task Scheduler again.
bool isEnabled();
void refresh(); // reads the task again, in the background (changed outside WinShun?)
bool isSetUp(); // enabled for any copy of WinShun, this one or another
void setEnabled(bool enabled);
// The task still starts a program that is gone (the portable copy, after
// installing with Setup): point it at this one. A copy that still exists
// elsewhere keeps it.
void adoptIfOrphaned();
void migrateFromQuickFind(); // the autostart of versions still called QuickFind (see Migration.h)

} // namespace ws::autostart
