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
    property int hovered: -1 // the entry under the pointer
    // Its entries are made the first time it opens, then kept: most menus
    // (the sort menu, every plugin card's) are never opened.
    property bool made: false
    readonly property real dpr: Screen.devicePixelRatio

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
        hovered = -1
        if (!open)
            return
        if (!made) {
            made = true // the entries now, laid out for place() to measure
            column.forceLayout()
        }
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

    Item {
        id: box
        // At least menuWidth, wider for a longer entry (an English one).
        width: Math.max(dropMenu.menuWidth, Math.ceil(repeater.widest) + 2 * Theme.menuPadding)
        height: column.implicitHeight + 2 * Theme.menuPadding

        Hook { // the style's box (Theme.menuFrame): body, edge and shadow
            anchors.fill: parent
            lazy: true
            part: Theme.menuFrame
            dpr: dropMenu.dpr
        }

        MouseArea { // the edge and gaps around the entries: a press there is the menu's, and goes no further
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }

        // The highlight under the entry pointed at: one for the menu (the
        // style's rowHighlight, role "menu"), jumping or sliding as the
        // style says (Theme.rowSlide).
        Hook {
            id: highlight

            readonly property real wantedY: dropMenu.hovered >= 0 ? column.y + dropMenu.hovered * Theme.menuItemHeight : 0
            property bool placed: false // it stood under an entry before: slides from there
            property real shownY: 0
            // A part that knows the menu (declares `role`, or `inMenu` as
            // brand's does) draws the fill; one that does not leaves it to
            // the menu's own.
            readonly property bool inMenu: item !== null && ("role" in item || "inMenu" in item)

            visible: dropMenu.hovered >= 0
            lazy: true
            x: column.x
            y: shownY
            width: column.width
            height: Theme.menuItemHeight
            part: Theme.rowHighlight
            hovered: inMenu
            position: shownY
            role: "menu"
            dpr: dropMenu.dpr

            Rectangle {
                anchors.fill: parent
                visible: !highlight.inMenu
                radius: Theme.menuItemRadius
                color: Theme.menuHover
            }

            onWantedYChanged: {
                const had = placed
                placed = dropMenu.hovered >= 0
                if (had && placed && Theme.rowSlide > 0 && SystemTheme.animations) {
                    slide.restart()
                } else {
                    slide.stop()
                    shownY = wantedY
                }
            }
            NumberAnimation {
                id: slide
                target: highlight
                property: "shownY"
                to: highlight.wantedY
                duration: Theme.rowSlide
                easing.type: Easing.OutCubic
            }
        }

        Column {
            id: column
            x: Theme.menuPadding
            y: Theme.menuPadding
            width: parent.width - 2 * Theme.menuPadding

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

                model: dropMenu.made ? dropMenu.entries : []
                onItemAdded: fit()
                onItemRemoved: fit()

                delegate: Item {
                    id: entry

                    required property var modelData
                    required property int index
                    readonly property real naturalWidth: label.x + label.implicitWidth + 14

                    width: column.width
                    onNaturalWidthChanged: repeater.fit()
                    height: Theme.menuItemHeight

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
                        font.pixelSize: Theme.menuCaptionSize
                    }
                    MouseArea {
                        id: entryArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onContainsMouseChanged: {
                            if (containsMouse)
                                dropMenu.hovered = entry.index
                            else if (dropMenu.hovered === entry.index)
                                dropMenu.hovered = -1
                        }
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
