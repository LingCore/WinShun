#pragma once

#include <QString>
#include <QStringList>

#include <windows.h>

#include <functional>
#include <vector>

// Thin wrappers over Explorer/Shell behaviour.
namespace qf::shell {

// Both run on a short-lived worker thread: a slow shell extension or an
// unreachable drive must never freeze the launcher.
void open(const QString& path, bool asAdministrator = false);
void reveal(const QString& path); // open the folder and select the item
void openUrl(const QString& url); // https:, mailto: ... with the user's normal rights
// An installed app by its launch path, "shell:AppsFolder\<id>" (see AppCatalog).
void launchApp(const QString& launchPath, bool asAdministrator = false);
// Moves the item to the Recycle Bin (asks before deleting permanently when it
// cannot be recycled). `done` runs on the worker thread with whether the shell
// reported success; `owner` parents any shell dialog.
void recycle(const QString& path, HWND owner, std::function<void(bool)> done);

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
// chosen id, or 0 when dismissed.
int popupMenu(HWND owner, const std::vector<MenuItem>& items, POINT screenPos);

// The standard "select folder" dialog (modal). Returns an empty string when cancelled.
QString pickFolder(HWND owner, const QString& title);

} // namespace qf::shell

namespace qf::autostart {

bool isEnabled();
void setEnabled(bool enabled);
void migrate(); // from the Run key of older versions, which cannot start an elevated program

} // namespace qf::autostart
