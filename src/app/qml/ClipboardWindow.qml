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

    required property Clipboard clipboard
    required property Placement placement
    required property WindowFrame frame

    readonly property int listWidth: 480
    property int previewWidth: 400 // set as it opens
    readonly property int maxRows: 8
    readonly property int minRows: 3 // it settles no lower than these fit below
    // As many rows as fit below where it is put (above, over a search box).
    readonly property int rows: Math.max(minRows, Math.min(maxRows,
                                         Math.floor((placement.room - page.chromeHeight) / page.rowHeight)))

    width: listWidth + (page.previewOpen ? 1 + previewWidth : 0)
    height: page.implicitHeight
    // See-through to Mica, as the launcher; drawn as active also while the
    // program it pastes into keeps the focus (WindowFrame::setNoActivate).
    color: Theme.backdrop && SystemTheme.materials && (window.active || clipboard.keysRouted)
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
    Binding { // the list's part: the preview makes it wider to the right (roomRight)
        target: window.placement
        property: "anchorWidth"
        value: window.listWidth
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
    }

    ClipboardPage {
        id: page
        width: window.width
        listWidth: window.listWidth
        clipboard: window.clipboard
        frame: window.frame
        rows: window.rows
        onPreviewOpening: window.previewWidth = Math.max(280, Math.min(400, window.placement.roomRight() - 1))
    }

    WindowEdge {}
}
