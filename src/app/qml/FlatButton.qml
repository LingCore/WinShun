import QtQuick
import WinShun

// A Windows 11 style push button, optionally with an icon glyph.
Rectangle {
    id: button

    property string text
    property string glyph
    property bool highlighted: false
    property bool flat: false // no background until hovered
    readonly property color foreground: highlighted ? Theme.accentText : Theme.text

    signal clicked()

    implicitWidth: Math.max(32, content.implicitWidth + (text.length > 0 ? 24 : 16))
    implicitHeight: 36
    radius: 4
    opacity: enabled ? 1 : 0.45
    activeFocusOnTab: true
    color: highlighted
           ? (area.pressed ? Qt.darker(Theme.accent, 1.15) : area.containsMouse ? Qt.lighter(Theme.accent, 1.08) : Theme.accent)
           : (area.pressed ? Theme.controlPressed : area.containsMouse ? Theme.controlHover : flat ? "transparent" : Theme.control)
    border.width: highlighted || (flat && !area.containsMouse) ? 0 : 1
    border.color: Theme.controlBorder

    Keys.onSpacePressed: button.clicked()
    Keys.onReturnPressed: button.clicked()
    Keys.onEnterPressed: button.clicked()

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 6

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
    }

    Rectangle { // keyboard focus
        visible: button.activeFocus
        anchors.fill: parent
        anchors.margins: -3
        radius: 6
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
