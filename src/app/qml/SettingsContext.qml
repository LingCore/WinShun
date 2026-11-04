import QtQuick
import WinShun

// What every page of the settings window reads from the window (one for the
// window): the search, where it is, the pages' names; and the two things a
// page asks the window to do (SettingsPage).
QtObject {
    property SettingsSearch search: null
    property real dpr: 1
    property int currentPage: 0
    property int currentTab: 0
    property var pages: []
    property var pageKeywords: []

    signal openRequested(int index, var tab)
    // The handler calls change() (SettingsWindow.holdInView).
    signal holdRequested(Item item, var change)
}
