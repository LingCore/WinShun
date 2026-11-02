import QtQuick
import WinShun

// Windows 11 style on/off switch with an "开 / 关" label. It never changes
// `checked` itself: bind it and handle toggled().
// A system switch's row (ResultRow) draws more of it: `mixed` (part on, part
// off: "自定义", the knob in the middle), `unknown` (not read yet: just the
// track), `busy` (being written: dimmed if that takes over 150 ms), and
// pulse() (asked for what it already is: the track lights up).
Item {
    id: toggle

    property bool checked
    property bool mixed: false
    property bool unknown: false
    property bool busy: false
    property bool word: true // false: just the track (a label of its own beside it)
    property bool mixable: false // room for "自定义" too, so that the switch stays put whatever its state
    property int wordAlign: Text.AlignRight
    property bool animated: true // false: the colours and the knob jump (a row taken for another switch)
    readonly property bool hovered: area.containsMouse
    signal toggled(bool on)

    // Taken for another switch (a recycled row), it is that switch's from the first frame.
    function settle() {
        if (slide.running)
            slide.complete()
        if (tint.running)
            tint.complete()
        if (knobTint.running)
            knobTint.complete()
        dimmed = false
    }
    // Asked for what it already is: the track lights up for 160 ms.
    function pulse() {
        glow.restart()
    }

    implicitWidth: row.implicitWidth
    implicitHeight: 32
    activeFocusOnTab: true

    property bool dimmed: false // busy for over 150 ms
    opacity: toggle.busy && toggle.dimmed ? 0.5 : 1
    Behavior on opacity { NumberAnimation { duration: 80 } }
    onBusyChanged: if (!busy) dimmed = false

    Timer {
        interval: 150
        running: toggle.busy
        onTriggered: toggle.dimmed = true
    }

    // Hidden and shown again, nothing is left half way (no frame was drawn to carry it on).
    Connections {
        target: toggle.Window.window
        function onVisibleChanged() {
            if (toggle.Window.window && toggle.Window.window.visible)
                toggle.settle()
        }
    }

    Keys.onSpacePressed: toggle.toggled(!toggle.checked)

    Row {
        id: row
        y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio) // whole device pixels, so that the track's edges are crisp
        spacing: 10

        Text { // as wide as the longest word, so the switch stays put ("On" / "Off")
            visible: toggle.word // hidden: no width, no spacing in the Row
            anchors.verticalCenter: parent.verticalCenter
            width: Math.ceil(Math.max(onWord.advanceWidth, offWord.advanceWidth, toggle.mixable ? mixedWord.advanceWidth : 0))
            horizontalAlignment: toggle.wordAlign
            text: toggle.unknown ? "" : toggle.mixed ? mixedWord.text : toggle.checked ? onWord.text : offWord.text
            color: Theme.text
            font.pixelSize: Theme.fontBody

            TextMetrics { id: onWord; text: qsTr("开"); font.pixelSize: Theme.fontBody }
            TextMetrics { id: offWord; text: qsTr("关"); font.pixelSize: Theme.fontBody }
            TextMetrics { id: mixedWord; text: qsTr("自定义"); font.pixelSize: Theme.fontBody }
        }

        Rectangle {
            id: track
            readonly property bool lit: toggle.checked && !toggle.mixed && !toggle.unknown
            y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio)
            width: 40
            height: 20
            radius: 10
            // Fading to or from "transparent" would pass through black: from the fill's own colour instead.
            color: lit ? Theme.accentFill : area.containsMouse ? Theme.controlHover : Qt.rgba(Theme.controlHover.r, Theme.controlHover.g, Theme.controlHover.b, 0)
            border.width: lit ? 0 : 1
            border.color: Theme.subtext

            Behavior on color { enabled: toggle.animated; ColorAnimation { id: tint; duration: 120 } }

            Rectangle {
                id: knob
                readonly property real size: area.pressed ? 14 : area.containsMouse ? 14 : 12
                visible: !toggle.unknown
                anchors.verticalCenter: parent.verticalCenter
                x: toggle.mixed ? (parent.width - width) / 2 : toggle.checked ? parent.width - width - 4 : 4
                width: area.pressed ? 17 : size
                height: size
                radius: size / 2
                color: track.lit ? Theme.accentText : Theme.subtext

                Behavior on x { enabled: toggle.animated; NumberAnimation { id: slide; duration: 120; easing.type: Easing.OutCubic } }
                Behavior on color { enabled: toggle.animated; ColorAnimation { id: knobTint; duration: 120 } }
            }

            Rectangle { // asked for what it already is: a light that comes and goes
                id: light
                anchors.fill: parent
                radius: parent.radius
                color: track.lit ? Theme.accentText : Theme.text
                opacity: 0

                SequentialAnimation {
                    id: glow
                    NumberAnimation { target: light; property: "opacity"; to: 0.3; duration: 80 }
                    NumberAnimation { target: light; property: "opacity"; to: 0; duration: 80 }
                }
            }

            Rectangle { // keyboard focus
                visible: toggle.activeFocus
                anchors.fill: parent
                anchors.margins: -3
                radius: 13
                color: "transparent"
                border.width: 2
                border.color: Theme.text
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: toggle.toggled(!toggle.checked)
    }
}
