import QtQuick
import WinShun

// Where a gesture counts: "任何位置", or only when it is started in one corner
// of the screen. A line of small words with a chevron, as a link: the
// menu under it offers the places it can be moved to (those another rule for
// the same gesture has are left out).
Item {
    id: zone

    required property GestureEditor editor
    required property int ruleRow // the rule's row in the rules model
    required property string whereTitle
    property string app // the program whose copy of the rule it is
    property real dpr: 0

    implicitWidth: words.implicitWidth + 4 + chevron.width
    implicitHeight: 20
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: whereTitle

    Keys.onSpacePressed: menu.open = !menu.open
    Keys.onReturnPressed: menu.open = !menu.open

    Text {
        id: words
        anchors.verticalCenter: parent.verticalCenter
        text: zone.whereTitle
        color: area.containsMouse || menu.open ? Theme.text : Theme.subtext
        font.pixelSize: Theme.fontCaption
    }
    Glyph {
        id: chevron
        anchors.left: words.right
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        glyph: "ph:caret-down"
        size: 12
        color: words.color
    }
    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -4
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: menu.open = !menu.open
    }
    DropMenu {
        id: menu
        anchor: zone
        menuWidth: 168
        entries: {
            zone.editor.revision // read again when the rules change
            return zone.editor.zoneChoices(zone.ruleRow)
        }
        onActivated: (index) => zone.editor.setWhere(zone.ruleRow, entries[index].zone, zone.app)
    }
}
