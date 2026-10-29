pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A menu dropped from a button (添加插件, a plugin card's "…"): its entries
// under the button, right edges lined up, or over it when there is no room
// below. While open it lives on the window's content item, on top of
// everything (as HoverTip does): left inside the button, the rows and cards
// that come after it in the page would cover it, whatever its z (z orders
// siblings only). A press anywhere else, or the wheel, closes it; a press on
// the menu itself stays with it.
Item {
    id: dropMenu

    required property Item anchor // the button it drops from
    // {text, glyph, danger}, in order; activated(index) when one is chosen.
    property var entries: []
    property real menuWidth: 200
    property bool open: false

    // The window's WindowFrame, where it has one (ours keep it as `frame`; a
    // Window says nothing of it, hence the untyped step). Where the open box
    // lies over a drag area (the launcher's footer, a title bar), the frame
    // would take the presses there for a drag of the window: the box is a
    // popup to it.
    readonly property var hostWindow: anchor ? anchor.Window.window : null
    readonly property WindowFrame frame: hostWindow ? (hostWindow.frame ?? null) : null

    signal activated(int index)

    parent: anchor ? anchor.Window.contentItem : null
    anchors.fill: parent // the whole window: a press outside the menu closes it
    z: 1000
    visible: open

    onOpenChanged: {
        if (!open)
            return
        if (frame)
            frame.addPopup(box)
        place()
    }

    function place() {
        if (!parent || !anchor)
            return
        const p = anchor.mapToItem(parent, 0, 0)
        const below = p.y + anchor.height + 4
        box.x = Math.max(8, Math.min(p.x + anchor.width - box.width, parent.width - box.width - 8))
        box.y = below + box.height <= parent.height - 8 ? below : Math.max(8, p.y - box.height - 4)
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        // The press goes on to what is there; on the button itself, its click
        // closes the menu (a toggle), so it is left to that.
        onPressed: (mouse) => {
            const p = mapToItem(dropMenu.anchor, mouse.x, mouse.y)
            if (!dropMenu.anchor.contains(p))
                dropMenu.open = false
            mouse.accepted = false
        }
        onWheel: (wheel) => {
            dropMenu.open = false // the page scrolls away from under it
            wheel.accepted = false
        }
    }

    Rectangle {
        id: box
        // At least menuWidth, wider for a longer entry (an English one).
        width: Math.max(dropMenu.menuWidth, Math.ceil(repeater.widest) + 8)
        height: column.implicitHeight + 8
        radius: Theme.cardRadius
        color: Theme.menuBackground
        border.width: 1
        border.color: Theme.windowEdge

        MouseArea { // the edge and gaps around the entries: a press there is the menu's, and goes no further
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }

        Column {
            id: column
            x: 4
            y: 4
            width: parent.width - 8

            Repeater {
                id: repeater

                property real widest: 0 // the widest entry's natural width

                function fit() {
                    let w = 0
                    for (let i = 0; i < count; ++i) {
                        const item = itemAt(i)
                        if (item)
                            w = Math.max(w, item.naturalWidth)
                    }
                    widest = w
                }

                model: dropMenu.entries
                onItemAdded: fit()
                onItemRemoved: fit()

                delegate: Rectangle {
                    id: entry

                    required property var modelData
                    required property int index
                    readonly property real naturalWidth: label.x + label.implicitWidth + 14

                    width: parent.width
                    onNaturalWidthChanged: repeater.fit()
                    height: 34
                    radius: Theme.controlRadius
                    color: entryArea.containsMouse ? Theme.menuHover : "transparent"

                    Glyph {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: entry.modelData.glyph
                        size: 14
                        color: entry.modelData.danger ? Theme.danger : Theme.text
                    }
                    Text {
                        id: label
                        x: 36
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.text
                        color: entry.modelData.danger ? Theme.danger : Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: entryArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            dropMenu.open = false
                            dropMenu.activated(entry.index)
                        }
                    }
                }
            }
        }
    }
}
