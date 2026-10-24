#pragma once

// What a second start of Win顺 sends the running one for `--settings`
// (main.cpp builds it, App::handleCommand reads it), over WM_COPYDATA
// (platform/MessageWindow):
//
//   "settings"            open the settings window
//   "settings:<place>"    open it there: `--settings-at <place>`
//
// A place is "<page>,<tab>" (both from 0: the page's number in the window's list
// of categories, SettingsWindow.qml pageTabs; the tab among that page's tabs,
// 0 when it has none) or "indexAccess" (the 索引方式 row). SettingsWindow.showPlace
// reads it. `--settings-at` without `--settings` does nothing.

#include <QString>

namespace ws::settingscommand {

inline QString make(const QString& place)
{
    return place.isEmpty() ? QStringLiteral("settings") : QStringLiteral("settings:") + place;
}

// Whether `command` opens the settings; `place` gets where (empty: just open them).
inline bool parse(const QString& command, QString* place)
{
    if (command == QStringLiteral("settings")) {
        if (place)
            place->clear();
        return true;
    }
    if (command.startsWith(QStringLiteral("settings:"))) {
        if (place)
            *place = command.mid(9);
        return true;
    }
    return false;
}

} // namespace ws::settingscommand
