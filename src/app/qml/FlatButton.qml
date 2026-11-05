import QtQuick
import WinShun

// A Windows 11 style push button, optionally with an icon glyph.
Rectangle {
    id: button

    property string text
    property string glyph
    property string trailingGlyph // after the text (a drop-down's chevron)
    property real contentSpacing: 6 // icon to text; a round icon (arrow-clockwise) fills its box and wants a bit more
    property bool highlighted: false
    property bool flat: false // no background until hovered
    // false: Tab skips it and Space and Enter do nothing; only a click works
    // (for a button that a key meant for another window must not press).
    property bool keyboardActivation: true
    readonly property color foreground: highlighted ? Theme.accentText : Theme.text

    signal clicked()

    implicitWidth: Math.max(32, content.implicitWidth + (text.length > 0 ? 24 : 16))
    implicitHeight: 36
    radius: Theme.controlRadius
    opacity: enabled ? 1 : 0.45
    activeFocusOnTab: keyboardActivation
    color: highlighted
           ? (area.pressed ? Qt.darker(Theme.accentFill, 1.15) : area.containsMouse ? Qt.lighter(Theme.accentFill, 1.08) : Theme.accentFill)
           : (area.pressed ? Theme.controlPressed : area.containsMouse ? Theme.controlHover : flat ? "transparent" : Theme.control)
    border.width: highlighted || (flat && !area.containsMouse) ? 0 : 1
    border.color: Theme.controlBorder

    Keys.onSpacePressed: if (keyboardActivation) button.clicked()
    Keys.onReturnPressed: if (keyboardActivation) button.clicked()
    Keys.onEnterPressed: if (keyboardActivation) button.clicked()

    Row {
        id: content
        anchors.centerIn: parent
        spacing: button.contentSpacing

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.glyph.length > 0
            glyph: button.glyph
            size: 14
            color: button.foreground
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.text.length > 0
            text: button.text
            color: button.foreground
            font.pixelSize: Theme.fontBody
        }
        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.trailingGlyph.length > 0
            glyph: button.trailingGlyph
            size: 10
            color: button.foreground
        }
    }

    Rectangle { // keyboard focus
        visible: button.activeFocus
        anchors.fill: parent
        anchors.margins: -3
        radius: Theme.largeControlRadius
        color: "transparent"
        border.width: 2
        border.color: Theme.text
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
