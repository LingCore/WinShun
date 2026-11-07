pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A Windows 11 style tooltip for whichever item `target` names (null: none).
// It appears after the mouse rests on a target for a moment, above it (below
// when there is no room) and kept inside the window. One tip can serve a group
// of neighbours: when `target` moves to another item while the tip is up, it
// glides over and takes the new text instead of hiding and showing again. It
// lives on the window's content item, so a clipping list cannot cut it off.
//
// The bubble is made the first time the tip shows, then kept: most tips (one
// in every result and clipboard row, one per button) never show, and until
// then a tip is only this item and its timer.
Item {
    id: tip

    property Item target
    property string text
    property string shortcut
    property int delay: 250
    property int linger: 150 // after the mouse leaves: time to reach a neighbour
    property real dpr: 0 // the window's, passed by a host that has it (GitHub #111)

    property bool shown: false

    // What is on screen. Kept while the tip fades out so it does not go blank.
    property Item anchor: null
    property string shownText
    property string shownShortcut

    // Where the bubble settles (it glides there from the last target).
    property real bubbleX: 0
    property real bubbleY: 0
    readonly property Item bubble: body.item
    readonly property real fullWidth: bubble ? bubble.fullWidth : 0

    parent: anchor ? anchor.Window.contentItem : null
    z: 1000
    visible: bubble !== null && bubble.opacity > 0

    // `target` and the texts usually change together; settle once they all have.
    onTargetChanged: Qt.callLater(sync)
    onTextChanged: Qt.callLater(sync)
    onShortcutChanged: Qt.callLater(sync)
    onFullWidthChanged: if (shown) place()

    function sync() {
        if (target) {
            if (timer.hiding)
                timer.stop()
            if (shown) {
                latch()
                place()
            } else if (!timer.running) {
                timer.hiding = false
                timer.restart()
            }
        } else {
            if (!timer.hiding)
                timer.stop()
            if (shown && !timer.running) {
                timer.hiding = true
                timer.restart()
            }
        }
    }

    function latch() {
        anchor = target
        shownText = text
        shownShortcut = shortcut
        body.active = true // made now with the texts, measured by place()
    }

    function place() {
        if (!parent || !anchor || !bubble)
            return
        const gap = 6
        const p = anchor.mapToItem(parent, 0, 0)
        bubbleX = Math.max(8, Math.min(p.x + (anchor.width - fullWidth) / 2, parent.width - fullWidth - 8))
        bubbleY = p.y - bubble.height - gap >= 4 ? p.y - bubble.height - gap : p.y + anchor.height + gap
    }

    // The wait before showing, or after the mouse left before hiding.
    Timer {
        id: timer
        property bool hiding: false
        interval: hiding ? tip.linger : tip.delay
        onTriggered: {
            if (hiding) {
                tip.shown = false
                return
            }
            if (!tip.target)
                return
            tip.latch()
            tip.place()
            tip.shown = true
        }
    }

    Loader {
        id: body
        active: false
        sourceComponent: Item {
            id: face

            // The size it settles at; `width` follows it with an animation.
            readonly property real keysWidth: tip.shownShortcut.length > 0 ? 10 + keys.implicitWidth : 0
            // The label: its one-line width, but no wider than the window leaves (8 px each side, 10 px padding each side); beyond that it wraps.
            readonly property real labelWidth: Math.min(probe.implicitWidth, Math.max(40, (tip.parent ? tip.parent.width : 100000) - 16 - 20 - keysWidth))
            readonly property real fullWidth: labelWidth + keysWidth + 20

            x: tip.bubbleX
            y: tip.bubbleY
            width: box.width
            height: Math.max(label.implicitHeight, keys.implicitHeight) + 12
            opacity: tip.shown ? 1 : 0

            // Glide only between targets; the first placement jumps straight there.
            Behavior on x {
                enabled: tip.shown
                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
            }
            Behavior on y {
                enabled: tip.shown
                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
            }
            Behavior on opacity { NumberAnimation { duration: 120 } }

            transform: Translate {
                y: tip.shown ? 0 : 4
                Behavior on y { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
            }

            Text { // the text on one line, only to measure it: wrapping the label does not feed back
                id: probe
                visible: false
                text: tip.shownText
                font.pixelSize: Theme.fontCaption
            }

            Item {
                id: box
                width: face.fullWidth
                height: face.height

                Hook { // the style's box (Theme.menuFrame): body, edge and shadow
                    anchors.fill: parent
                    lazy: true
                    part: Theme.menuFrame
                    // Screen.* only when no host passed it: each item that reads it has an object of its own.
                    dpr: tip.dpr > 0 ? tip.dpr : Screen.devicePixelRatio
                }

                Behavior on width {
                    enabled: tip.shown
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }

                Row {
                    anchors.centerIn: parent
                    spacing: 10

                    Text {
                        id: label
                        width: face.labelWidth
                        text: tip.shownText
                        wrapMode: Text.Wrap
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                    Text {
                        id: keys
                        visible: tip.shownShortcut.length > 0
                        text: tip.shownShortcut
                        color: Theme.faint
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }
        }
    }
}
