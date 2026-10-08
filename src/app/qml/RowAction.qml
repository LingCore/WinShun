import QtQuick
import WinShun

// A small icon button at the end of a result row (reveal, copy, delete).
// `danger` paints it red, with an optional label: the armed delete button.
// `tip` (and `tipShortcut`) are for the HoverTip the row shares among its
// buttons; `tipWanted` says when the mouse rests on this one.
Rectangle {
    id: button

    property string glyph
    property string text
    property bool danger: false
    property string tip
    property string tipShortcut
    readonly property bool tipWanted: area.containsMouse && !area.used && tip.length > 0
    readonly property color foreground: danger ? Theme.accentText : area.containsMouse ? Theme.text : Theme.subtext

    signal clicked()

    implicitWidth: text.length > 0 ? content.implicitWidth + 20 : 32
    implicitHeight: 32
    radius: 4
    color: danger
           ? (area.pressed ? Qt.darker(Theme.danger, 1.15) : area.containsMouse ? Qt.lighter(Theme.danger, 1.08) : Theme.danger)
           : (area.pressed ? Theme.controlPressed : area.containsMouse ? Theme.controlHover : "transparent")

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 6

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            glyph: button.glyph
            size: 16
            color: button.foreground
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.text.length > 0
            text: button.text
            color: button.foreground
            font.pixelSize: Theme.fontCaption
        }
    }

    MouseArea {
        id: area

        property bool used: false // clicked since the mouse came: no tooltip until it leaves

        anchors.fill: parent
        hoverEnabled: true
        onPressed: used = true
        onContainsMouseChanged: if (!containsMouse) used = false
        onClicked: button.clicked()
    }
}
