pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A clipboard row's buttons (ClipRow): its handle to drag (in its group's
// own order), paste (or, with clickToPaste, the preview), pin, delete. Not
// made in every row: the list keeps a few sets (RowActionsPool) and lends one
// to a row while it shows them (see ResultActions).
Item {
    id: bar

    property ClipRow host: null // the row it is lent to; null: put away
    readonly property real dpr: Screen.devicePixelRatio

    // For the row: the room the buttons take and the button the mouse rests
    // on (its HoverTip); its handle held, it tells the row (ClipRow.handleHeld).
    readonly property real actionsWidth: actions.width
    readonly property RowAction hovered: moveAction.tipWanted ? moveAction
                                       : pasteAction.tipWanted ? pasteAction
                                       : pinAction.tipWanted ? pinAction
                                       : removeAction.tipWanted ? removeAction : null

    // Put in the row's mouse area by the pool, so that hovering a button still hovers the row.
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    visible: host !== null

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        RowAction { // dragged up or down: to another place in its group
            id: moveAction
            dpr: bar.dpr
            shared: true
            small: bar.host !== null && bar.host.metrics.dense
            visible: bar.host !== null && bar.host.movable
            draggable: true
            glyph: "" // GripperBarHorizontal
            tip: qsTranslate("ClipRow", "拖动调整顺序")
            tipShortcut: "Ctrl+↑/↓"
            onHeldChanged: {
                const row = bar.host // let go off the row, it gives this set back at once
                if (!row)
                    return
                row.handleHeld = held
                row.moveHeld(held)
            }
            onDragMoved: (position) => {
                if (bar.host)
                    bar.host.handleDragged(moveAction, position)
            }
        }
        RowAction { // a click pastes already (clickToPaste): the preview here instead
            id: pasteAction
            readonly property ClipRow host: bar.host
            dpr: bar.dpr
            shared: true
            small: host !== null && host.metrics.dense
            glyph: host !== null && host.clickToPaste ? "" : "" // View, Paste
            tip: host === null ? "" : host.clickToPaste ? qsTranslate("ClipRow", "预览") : host.pasteTip
            tipShortcut: host !== null && host.clickToPaste ? "Alt+P" : "Enter"
            onClicked: host.clickToPaste ? host.previewRequested(host.index) : host.pasteRequested(host.index)
        }
        RowAction {
            id: pinAction
            readonly property ClipRow host: bar.host
            dpr: bar.dpr
            shared: true
            small: host !== null && host.metrics.dense
            glyph: host !== null && host.pinned ? "" : "" // Unpin, Pin
            tip: host !== null && host.pinned ? qsTranslate("ClipRow", "取消固定") : qsTranslate("ClipRow", "固定（不会过期）")
            tipShortcut: "Ctrl+P"
            onClicked: host.actionRequested(host.index, Clipboard.Pin)
        }
        RowAction {
            id: removeAction
            dpr: bar.dpr
            shared: true
            small: bar.host !== null && bar.host.metrics.dense
            glyph: "" // Delete
            tip: qsTranslate("ClipRow", "删除")
            tipShortcut: "Delete"
            onClicked: bar.host.actionRequested(bar.host.index, Clipboard.Remove)
        }
    }
}
