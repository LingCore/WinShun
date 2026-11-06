import QtQuick
import WinShun

// The volume on its row (ResultRow), left of its switch: the number, then a
// 64×4 bar. The number takes the width of "100", right-aligned against the
// bar, so that neither moves as it changes; not known yet, just the track.
// The bar grows and shrinks in 120 ms; a row taken for another switch, or a
// window shown again, has it at its value at once.
Item {
    id: level

    property int value: -1 // 0–100; -1 not known
    property bool muted: false // the level stays, greyed: nothing is heard at it
    property bool animated: true // false: the bar jumps

    property real dpr: Screen.devicePixelRatio // the row passes the window's (GitHub #111)
    function onPixel(v) { return Math.round(v * dpr) / dpr }
    function settle() {
        if (grow.running)
            grow.complete()
    }

    implicitWidth: number.width + 8 + bar.width

    // Hidden and shown again, nothing is left half way (no frame was drawn to carry it on).
    Connections {
        target: level.Window.window
        function onVisibleChanged() {
            if (level.Window.window && level.Window.window.visible)
                level.settle()
        }
    }

    Text { // centred as the switch's word is: on the same baseline
        id: number
        anchors.verticalCenter: parent.verticalCenter
        width: Math.ceil(widest.advanceWidth)
        horizontalAlignment: Text.AlignRight
        text: level.value >= 0 ? level.value : ""
        color: Theme.text
        font.pixelSize: Theme.fontBody

        TextMetrics { id: widest; text: "100"; font.pixelSize: Theme.fontBody }
    }

    Rectangle {
        id: bar
        x: number.width + 8
        y: level.onPixel((level.height - height) / 2)
        width: 64
        height: 4
        radius: Theme.progressRadius
        color: Theme.controlBorder
        Accessible.ignored: true

        Rectangle {
            width: level.onPixel(bar.width * Math.max(0, Math.min(100, level.value)) / 100)
            height: parent.height
            radius: parent.radius
            color: level.muted ? Theme.subtext : Theme.accent

            Behavior on color { ColorAnimation { duration: 120 } }
            Behavior on width { enabled: level.animated; NumberAnimation { id: grow; duration: 120; easing.type: Easing.OutCubic } }
        }
    }
}
