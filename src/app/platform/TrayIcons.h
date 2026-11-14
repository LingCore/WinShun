#pragma once

#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ws::traysearch {

// One icon of the notification area (Win11's XAML taskbar: the buttons of
// "SystemTray.NormalButton", read through UI Automation). The name is the
// first line of its tooltip, the subtitle the rest.
struct TrayItem {
    QString name;
    QString subtitle;
    QString key; // the name's first word before "：" ("Filee Filee：…" → "Filee"), for windows and programs
    QString iconFile; // a PNG of the icon as the taskbar draws it, for the tile
    QRect rect; // screen coordinates, physical pixels
};

// The key of an icon's name: its first word before "：" ("Filee Filee：…" →
// "Filee"), for windows and programs.
QString trayKey(const QString& name);

// The notification-area icons of this desktop. When the taskbar's overflow
// panel is open its hidden icons are included too.
QVector<TrayItem> snapshot();

// Switch to the window the icon belongs to: a running window of the icon's
// own program first, the icon's own click otherwise. `program`, when known,
// is the icon's executable: a window whose title merely holds the icon's
// name ("微信 · (myscoop) …" from another program) is then not its window.
// `doubleClick`: the tray is known to open only on a double click, so skip
// the single-click try; `usedDouble`, when given, says a double click was
// needed (the caller remembers that for next time).
bool activate(const TrayItem& item, bool doubleClick = false, bool* usedDouble = nullptr,
    const QString& program = {});

// Show the icon's own context menu: a real right click at its place.
bool showMenu(const TrayItem& item);

// The entries of the icon's own context menu, read from the menu it opens.
// Empty when the application draws a menu no reader can enumerate; then the
// menu is left open on the taskbar (the tray's own, as with showMenu).
struct TrayMenuItem {
    QString name;
    bool enabled = true;
};
QVector<TrayMenuItem> openMenu(const TrayItem& item);

// Open the icon's menu again and click the entry `names[index]` (by name, or
// its place when the names changed).
bool chooseMenuItem(const TrayItem& item, const QStringList& names, int index);

} // namespace ws::traysearch
