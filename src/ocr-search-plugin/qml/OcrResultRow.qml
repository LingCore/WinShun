pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

Rectangle {
    id: row
    required property var item
    property bool selected: false
    signal chosen()
    signal opened()
    height: 96
    color: selected ? Theme.selection : mouse.containsMouse ? Theme.controlHover : "transparent"
    Accessible.role: Accessible.ListItem
    Accessible.name: item.name + " · " + item.location
    Accessible.selected: selected
    Rectangle { visible: row.selected; width: 3; height: 28; anchors.verticalCenter: parent.verticalCenter; color: Theme.accent; radius: 1 }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 4
        Text { Layout.fillWidth: true; text: row.item.name; color: Theme.text; font.pixelSize: Theme.fontBody; elide: Text.ElideMiddle; font.weight: Theme.fontStrong }
        Text { Layout.fillWidth: true; text: row.item.location; color: Theme.accent; font.pixelSize: Theme.fontCaption; elide: Text.ElideRight }
        Text { Layout.fillWidth: true; text: row.item.snippet; color: Theme.subtext; font.pixelSize: Theme.fontCaption; maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight }
    }
    MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; onClicked: row.chosen(); onDoubleClicked: row.opened() }
    HoverTip { target: mouse.containsMouse ? row : null; text: row.item.path }
}
