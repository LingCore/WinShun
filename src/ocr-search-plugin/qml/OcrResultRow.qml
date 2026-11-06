pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

Item {
    id: row
    required property var item
    property bool selected: false
    readonly property real dpr: Screen.devicePixelRatio
    signal chosen()
    signal opened()
    height: 96
    Accessible.role: Accessible.ListItem
    Accessible.name: item.name + " · " + item.location
    Accessible.selected: selected
    Loader { // the style's row highlight (Theme.rowHighlight), made only while wanted
        anchors.fill: parent
        anchors.leftMargin: Theme.rowInset
        anchors.rightMargin: Theme.rowInset
        anchors.topMargin: Theme.edge(row.dpr)
        anchors.bottomMargin: Theme.edge(row.dpr)
        active: row.selected || mouse.containsMouse
        sourceComponent: Hook {
            part: Theme.rowHighlight
            selected: row.selected
            hovered: mouse.containsMouse
            role: "list"
            dpr: row.dpr
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        anchors.leftMargin: 12 + Theme.rowInset
        anchors.rightMargin: 12 + Theme.rowInset
        spacing: 4
        Text { Layout.fillWidth: true; text: row.item.name; color: Theme.text; font.pixelSize: Theme.fontBody; elide: Text.ElideMiddle; font.weight: Theme.fontStrong }
        Text { Layout.fillWidth: true; text: row.item.location; color: Theme.accent; font.pixelSize: Theme.fontCaption; elide: Text.ElideRight }
        Text { Layout.fillWidth: true; text: row.item.snippet; color: Theme.subtext; font.pixelSize: Theme.fontCaption; maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight }
    }
    MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; onClicked: row.chosen(); onDoubleClicked: row.opened() }
    HoverTip { target: mouse.containsMouse ? row : null; text: row.item.path }
}
