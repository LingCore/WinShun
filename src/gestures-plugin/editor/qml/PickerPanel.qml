pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A panel dropped from a button, made as DropMenu is (its box under the
// button, or over it when there is no room below, on the window's content
// item so that nothing in the page covers it; a press anywhere else, or the
// wheel, closes it) but holding whatever it is given: the action picker's
// categories and key box, the program picker's list.
Item {
    id: panel

    property Item anchor // the button it drops from
    property bool open: false
    property real boxWidth: 320
    property real boxHeight: 200
    default property alias contents: body.data
    readonly property real dpr: Screen.devicePixelRatio
    // The window's WindowFrame (ours keep it as `frame`): the box is a popup to it, not a drag area.
    readonly property var hostWindow: anchor ? anchor.Window.window : null
    readonly property WindowFrame frame: hostWindow ? (hostWindow.frame ?? null) : null

    signal closed()

    function close() { open = false }

    function place() {
        if (!parent || !anchor)
            return
        const p = anchor.mapToItem(parent, 0, 0)
        const below = p.y + anchor.height + 4
        box.x = Math.max(8, Math.min(p.x, parent.width - box.width - 8))
        box.y = below + box.height <= parent.height - 8 ? below : Math.max(8, Math.min(p.y - box.height - 4, parent.height - box.height - 8))
    }

    parent: anchor ? anchor.Window.contentItem : null
    anchors.fill: parent // the whole window: a press outside the box closes it
    z: 1000
    visible: open

    onOpenChanged: {
        if (open) {
            if (frame)
                frame.addPopup(box)
            place()
            box.forceActiveFocus()
        } else {
            closed()
        }
    }
    onBoxHeightChanged: if (open) place()
    onBoxWidthChanged: if (open) place()

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        // The press goes on to what is there; on the button itself, its click decides (a toggle).
        onPressed: (mouse) => {
            const p = mapToItem(panel.anchor, mouse.x, mouse.y)
            if (!panel.anchor.contains(p))
                panel.open = false
            mouse.accepted = false
        }
        onWheel: (wheel) => {
            panel.open = false // the page scrolls away from under it
            wheel.accepted = false
        }
    }

    FocusScope {
        id: box
        width: panel.boxWidth
        height: panel.boxHeight
        Keys.onEscapePressed: panel.open = false

        Hook { // the style's box (Theme.menuFrame): body, edge and shadow
            anchors.fill: parent
            lazy: true
            part: Theme.menuFrame
            dpr: panel.dpr
        }
        MouseArea { // the edge and the gaps: a press there is the panel's, and goes no further
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }
        Item {
            id: body
            anchors.fill: parent
        }
    }
}
