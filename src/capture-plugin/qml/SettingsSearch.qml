import QtQuick

// Win顺's settings search (src/app/SettingsSearch.h) is not in the plugin:
// its settings window has no search box. SettingsSection and SettingRow,
// shared with Win顺's settings window, name the type; with nothing to
// search, they never get one (their column has no `search`), so this only
// has to be a type by that name that is never searching.
QtObject {
    readonly property bool active: false
    readonly property int revision: 0
    readonly property int total: 0
    readonly property QtObject current: null
}
