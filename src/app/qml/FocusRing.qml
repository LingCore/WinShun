import QtQuick
import WinShun

// The keyboard focus around a control: a ring 3 px outside what it is made
// in, shown while `host` has the focus. Made by the control the first time it
// has it (FlatButton, ToggleSwitch): a window has hundreds of buttons and
// switches, and Tab reaches a few of them in a session, if any.
Rectangle {
    required property Item host
    property real corner: Theme.largeControlRadius

    visible: host.activeFocus
    anchors.fill: parent
    anchors.margins: -3
    radius: corner
    color: "transparent"
    border.width: 2
    border.color: Theme.text
}
