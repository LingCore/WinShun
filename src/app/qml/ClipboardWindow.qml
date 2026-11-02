pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The clipboard history (Win+V) in a window of its own, ClipboardPage.qml in
// it. Opened from a search box of Win顺's, it drops down under that box and
// pastes into it; from anywhere else, it drops down under the text caret of
// the program that was in front, or else at the mouse pointer, and pastes
// into that program (App, Placement). Moved, it stays there until hidden.
// Over another program it does not take the focus, as Windows' own does not:
// that program stays in front with its text field (VS Code's command palette
// stays open), the keys come through KeyRouter (clipboard.keysRouted).
// The list alone; an entry's preview beside it when asked for, the window
// growing by it to the right: as wide as there is room for there, so that
// the list stays where it is under the mouse; only with less room than that
// the window moves left for it.
Window {
    id: window

    required property var clipboard
    property bool controllerEventsEnabled: true
    required property Placement placement
    required property WindowFrame frame

    // A temporary limit beside native Start/Search; the saved list width stays.
    property int shellWidth: 0
    readonly property int minimumListWidth: shellWidth > 0 ? Math.min(360, shellWidth) : 360
    readonly property int previewExtent: page.previewOpen ? 1 + previewWidth : 0
    readonly property int listWidth: Math.max(minimumListWidth,
                                            Math.min(placement.windowSizing ? width - previewExtent : clipboard.listWidth,
                                                     maximumWidth - previewExtent, shellWidth > 0 ? shellWidth : 1600))
    readonly property int defaultPreviewWidth: 560
    readonly property int minPreviewWidth: 280
    property int requestedPreviewWidth: defaultPreviewWidth // set as it opens, and as it is dragged
    readonly property int previewWidth: Math.max(minPreviewWidth, Math.min(requestedPreviewWidth,
                                                                          Screen.desktopAvailableWidth - 32 - 360 - 1))
    readonly property int maxRows: 8
    readonly property int minRows: 3 // it settles no lower than these fit below
    // As many rows as fit below where it is put (above, over the caret or a
    // search box: placement.above).
    readonly property int rows: Math.max(minRows, Math.min(maxRows,
                                         Math.floor((placement.room - page.chromeHeight) / page.rowHeight)))

    minimumWidth: minimumListWidth + previewExtent
    maximumWidth: Math.max(minimumWidth, Math.min(1600 + previewExtent, Screen.desktopAvailableWidth - 32))
    minimumHeight: page.chromeHeight + minRows * page.rowHeight
    maximumHeight: Math.max(minimumHeight, Screen.desktopAvailableHeight)
    readonly property int wantedHeight: clipboard.windowHeight > 0
        ? Math.max(minimumHeight, clipboard.windowHeight) : page.chromeHeight + maxRows * page.rowHeight
    readonly property int fittedHeight: clipboard.windowHeight > 0
        ? Math.max(minimumHeight, Math.min(wantedHeight, maximumHeight, placement.room > 0 ? placement.room : maximumHeight))
        : page.implicitHeight
    // The native size loop owns the geometry while an edge is held. The
    // page follows that actual size, and preferences are saved on release.
    Binding {
        target: window
        property: "width"
        when: !window.placement.windowSizing
        value: window.listWidth + window.previewExtent
        restoreMode: Binding.RestoreNone
    }
    Binding {
        target: window
        property: "height"
        when: !window.placement.windowSizing
        value: window.fittedHeight
        restoreMode: Binding.RestoreNone
    }
    Binding {
        target: window.placement
        property: "contentSize"
        value: Qt.size(window.listWidth + window.previewExtent, window.fittedHeight)
    }
    // See-through to Mica, as the launcher; drawn as active also while the
    // program it pastes into keeps the focus (WindowFrame::setNoActivate),
    // routed or pinned.
    color: Theme.backdrop && SystemTheme.materials && (window.active || clipboard.keysRouted || clipboard.pinned)
           ? "transparent" : Theme.background
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    title: qsTr("剪贴板") // Qt adds " - Win顺"

    Binding {
        target: window.clipboard.items
        property: "highlightColor"
        value: Theme.accent
    }
    Binding { // with the fewest rows: kept room for below the window
        target: window.placement
        property: "roomNeeded"
        value: page.chromeHeight + window.minRows * page.rowHeight
    }
    // With the most, and a row: by the caret or a search box it opens on the
    // side where more whole rows fit, under it when all of them do (#38).
    Binding {
        target: window.placement
        property: "roomWanted"
        value: window.wantedHeight
    }
    Binding {
        target: window.placement
        property: "roomStep"
        value: window.clipboard.windowHeight > 0 ? 0 : page.rowHeight
    }
    Binding { // the list's part: the preview makes it wider to the right (roomRight)
        target: window.placement
        property: "anchorWidth"
        value: window.listWidth
    }

    // The preview as wide as asked, or as the room to the right allows (at
    // least the least): the window grows to the right, the list stays.
    function fitPreview(wanted: int): int {
        return Math.max(minPreviewWidth, Math.min(wanted, placement.roomRight() - 1,
                                                  Screen.desktopAvailableWidth - 32 - 360 - 1))
    }

    // Hidden: also free the menu window until it is needed again.
    onVisibleChanged: {
        if (!visible)
            page.windowHidden()
    }

    Connections {
        target: window.placement
        function onMovingChanged() { // the menu would stay behind
            if (window.placement.moving)
                page.closeContextMenu()
        }
        function onWindowResized(previousSize: size, currentSize: size) {
            if (currentSize.width !== previousSize.width)
                window.clipboard.listWidth = currentSize.width - window.previewExtent
            if (currentSize.height !== previousSize.height)
                window.clipboard.windowHeight = currentSize.height
        }
        function onSizeResetRequested() {
            window.clipboard.listWidth = 480
            window.clipboard.windowHeight = 0
        }
    }

    ClipboardPage {
        id: page
        objectName: "clipboardPage"
        width: window.width
        height: window.height
        listWidth: window.listWidth
        clipboard: window.clipboard
        controllerEventsEnabled: window.controllerEventsEnabled
        frame: window.frame
        rows: window.rows
        above: window.placement.above // over the caret or box: maybe upside down (clipboard.flipAbove)
        previewWidth: window.previewWidth
        // Less room than remembered: narrower for now, the remembered width stays.
        onPreviewOpening: window.requestedPreviewWidth = window.fitPreview(window.clipboard.previewWidth > 0
                                                                  ? window.clipboard.previewWidth : window.defaultPreviewWidth)
        onPreviewResizing: (wanted) => window.requestedPreviewWidth = window.fitPreview(wanted)
        onPreviewResized: window.clipboard.previewWidth = window.previewWidth // once, as it is let go
        onPreviewWidthReset: {
            window.requestedPreviewWidth = window.fitPreview(window.defaultPreviewWidth)
            window.clipboard.previewWidth = 0
        }
    }

    WindowEdge {}
}
