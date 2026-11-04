import QtQuick
import WinShun

// Windows 11 style overlay scroll bar: a thin line that widens when the mouse
// comes near. Drag the thumb to scroll; click the track to move a page.
Item {
    id: bar

    required property Flickable flickable

    readonly property bool expanded: area.containsMouse || area.pressed || collapseDelay.running
    readonly property real minY: flickable.originY - flickable.topMargin
    readonly property real maxY: flickable.originY + flickable.contentHeight + flickable.bottomMargin - flickable.height
    readonly property real position: maxY > minY ? Math.max(0, Math.min(1, (flickable.contentY - minY) / (maxY - minY))) : 0

    // A size in logical pixels as whole device pixels (3 is 4.5 at 150 %).
    function pixels(v) { return Math.max(1, Math.round(v * Screen.devicePixelRatio)) / Screen.devicePixelRatio }

    function scrollTo(fraction) {
        flickable.cancelFlick()
        flickable.contentY = minY + Math.max(0, Math.min(1, fraction)) * (maxY - minY)
    }

    function page(direction) {
        flickable.cancelFlick()
        flickable.contentY = Math.max(minY, Math.min(maxY, flickable.contentY + direction * flickable.height))
    }

    implicitWidth: 14
    visible: flickable.contentHeight > flickable.height

    Timer { // stay wide for a moment after the mouse leaves, as Windows does
        id: collapseDelay
        interval: 600
    }

    Hook { // the style's track and thumb (Theme.scrollBar)
        id: track
        anchors.right: parent.right
        anchors.rightMargin: bar.pixels(3)
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 4
        anchors.bottomMargin: 4
        width: bar.expanded ? bar.pixels(8) : bar.pixels(3)
        part: Theme.scrollBar
        active: bar.expanded
        hovered: area.overThumb
        pressed: area.pressed
        position: thumb.y - track.y
        extent: thumb.height
        dpr: Screen.devicePixelRatio

        Behavior on width { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
    }

    Item { // where the thumb is (drawn by the part)
        id: thumb
        anchors.right: track.right
        width: track.width
        height: Math.max(24, track.height * bar.flickable.height / Math.max(1, bar.maxY - bar.minY + bar.flickable.height))
        y: track.y + bar.position * (track.height - height)
    }

    MouseArea {
        id: area

        property real grab: -1 // where in the thumb the drag started; -1 = not dragging
        readonly property bool overThumb: containsMouse && mouseY >= thumb.y && mouseY <= thumb.y + thumb.height

        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true

        onContainsMouseChanged: if (!containsMouse && !pressed) collapseDelay.restart()
        onPressed: (mouse) => {
            if (mouse.y >= thumb.y && mouse.y <= thumb.y + thumb.height)
                grab = mouse.y - thumb.y
            else
                bar.page(mouse.y < thumb.y ? -1 : 1)
        }
        onPositionChanged: (mouse) => {
            if (pressed && grab >= 0 && track.height > thumb.height)
                bar.scrollTo((mouse.y - grab - track.y) / (track.height - thumb.height))
        }
        onReleased: {
            grab = -1
            if (!containsMouse)
                collapseDelay.restart()
        }
    }
}
