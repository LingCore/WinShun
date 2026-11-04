import QtQuick
import WinShun

// One page of the settings window, its sections' column (made when first
// needed: PageSlot). The sections read the same from it as from any column
// of pages (SettingsSection.qml): search, currentPage, currentTab, pages,
// pageKeywords, openPage(), holdInView(); all of it the window's (host).
Column {
    id: settingsPage

    required property SettingsContext host
    // Its rows and sections the window reaches by name (SettingsWindow.part).
    property var parts: ({})
    // One of its hotkey recorders is recording.
    property bool recordingHotkey: false

    // The window hidden: back as a page made anew would be.
    signal resetForReopen()
    // After the first frame of each opening: what may have changed while hidden, read again.
    signal opened()

    readonly property SettingsSearch search: host.search
    readonly property real dpr: host.dpr
    readonly property int currentPage: host.currentPage
    readonly property int currentTab: host.currentTab
    readonly property var pages: host.pages
    readonly property var pageKeywords: host.pageKeywords

    function openPage(index: int, tab: var): void { settingsPage.host.openRequested(index, tab) }
    function holdInView(item: Item, change: var): void { settingsPage.host.holdRequested(item, change) }
    // Positions the sections and rows now, rather than before the next frame.
    function layoutNow(): void {
        for (let i = 0; i < settingsPage.children.length; ++i) {
            const child = settingsPage.children[i]
            if (child.layoutNow)
                child.layoutNow()
        }
        settingsPage.forceLayout()
    }

    width: parent ? parent.width : 0
    spacing: 20
}
