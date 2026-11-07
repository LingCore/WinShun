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
    property real dpr: 0 // the window's, passed by a host that has it (GitHub #111)
    // Screen.* only when no host passed it (one attached object for all its
    // items then): a default binding to it would make one even where the
    // host's value replaces it.
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio
    signal toggled(bool on)

    // Taken for another switch (a recycled row), it is that switch's from the first frame.
    // (The part's settle(): its animations to their end.)
    function settle() {
        toggleHook.call("settle", [], null)
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
    opacity: !enabled ? toggleHook.out("disabledOpacity", 0.4) : toggle.busy && toggle.dimmed ? 0.5 : 1
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
        y: Theme.snap((parent.height - height) / 2, toggle.pixelRatio) // whole device pixels, so that the track's edges are crisp
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

        Item {
            id: track
            readonly property bool lit: toggle.checked && !toggle.mixed && !toggle.unknown
            readonly property real radius: Theme.pillRadius(height)
            y: Theme.snap((parent.height - height) / 2, toggle.pixelRatio)
            width: 40
            height: 20

            Hook { // the style's track and knob (Theme.toggle)
                id: toggleHook
                anchors.fill: parent
                part: Theme.toggle
                extra: ({ animated: toggle.animated })
                checked: toggle.checked
                mixed: toggle.mixed
                active: !toggle.unknown
                hovered: area.containsMouse
                pressed: area.pressed
                dpr: toggle.pixelRatio
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
                radius: track.radius > 0 ? track.radius + 3 : 0 // concentric with the track
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
