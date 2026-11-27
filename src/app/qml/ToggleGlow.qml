import QtQuick
import WinShun

// A ToggleSwitch asked for what it already is (pulse()): a light over its
// track that comes and goes in 160 ms. Made by the switch the first time
// that happens.
Rectangle {
    id: light

    property bool lit: false // the track's: on, the light is the colour of the words on the accent

    function restart() { glow.restart() }

    anchors.fill: parent
    color: lit ? Theme.accentText : Theme.text
    opacity: 0

    SequentialAnimation {
        id: glow
        NumberAnimation { target: light; property: "opacity"; to: 0.3; duration: 80 }
        NumberAnimation { target: light; property: "opacity"; to: 0; duration: 80 }
    }
}
