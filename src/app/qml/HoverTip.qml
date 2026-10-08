pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A Windows 11 style tooltip for whichever item `target` names (null: none).
// It appears after the mouse rests on a target for a moment, above it (below
// when there is no room) and kept inside the window. One tip can serve a group
// of neighbours: when `target` moves to another item while the tip is up, it
// glides over and takes the new text instead of hiding and showing again. It
// lives on the window's content item, so a clipping list cannot cut it off.
Item {
    id: tip

    property Item target
    property string text
    property string shortcut
    property int delay: 250
    property int linger: 150 // after the mouse leaves: time to reach a neighbour

    property bool shown: false

    // What is on screen. Kept while the tip fades out so it does not go blank.
    property Item anchor: null
    property string shownText
    property string shownShortcut

    parent: anchor ? anchor.Window.contentItem : null
    z: 1000
    // The size it settles at; `width` follows it with an animation.
    readonly property real fullWidth: label.implicitWidth + (shownShortcut.length > 0 ? 10 + keys.implicitWidth : 0) + 20

    width: bubble.width
    height: Math.max(label.implicitHeight, keys.implicitHeight) + 12
    opacity: shown ? 1 : 0
    visible: opacity > 0

    // `target` and the texts usually change together; settle once they all have.
    onTargetChanged: Qt.callLater(sync)
    onTextChanged: Qt.callLater(sync)
    onShortcutChanged: Qt.callLater(sync)
    onFullWidthChanged: if (shown) place()

    function sync() {
        if (target) {
            hideTimer.stop()
            if (shown) {
                latch()
                place()
            } else if (!delayTimer.running) {
                delayTimer.restart()
            }
        } else {
            delayTimer.stop()
            if (shown && !hideTimer.running)
                hideTimer.restart()
        }
    }

    function latch() {
        anchor = target
        shownText = text
        shownShortcut = shortcut
    }

    function place() {
        if (!parent || !anchor)
            return
        const gap = 6
        const p = anchor.mapToItem(parent, 0, 0)
        x = Math.max(8, Math.min(p.x + (anchor.width - fullWidth) / 2, parent.width - fullWidth - 8))
        y = p.y - height - gap >= 4 ? p.y - height - gap : p.y + anchor.height + gap
    }

    Timer {
        id: delayTimer
        interval: tip.delay
        onTriggered: {
            if (!tip.target)
                return
            tip.latch()
            tip.place()
            tip.shown = true
        }
    }

    Timer {
        id: hideTimer
        interval: tip.linger
        onTriggered: tip.shown = false
    }

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

    Rectangle {
        id: bubble
        width: tip.fullWidth
        height: tip.height
        radius: 6
        clip: true
        color: Theme.menuBackground
        border.width: 1
        border.color: Theme.controlBorder

        Behavior on width {
            enabled: tip.shown
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }

        Row {
            anchors.centerIn: parent
            spacing: 10

            Text {
                id: label
                text: tip.shownText
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
