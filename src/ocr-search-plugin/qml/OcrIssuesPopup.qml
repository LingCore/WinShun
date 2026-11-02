pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

FocusScope {
    id: popup
    required property Controller editor
    required property WindowFrame frame
    property bool open: false
    anchors.fill: parent
    visible: open
    z: 100
    Rectangle { anchors.fill: parent; color: "#60000000" }
    MouseArea { anchors.fill: parent; onClicked: popup.open = false }
    Rectangle {
        anchors.centerIn: parent
        width: Math.min(640, popup.width - 48)
        height: Math.min(460, popup.height - 64)
        radius: Theme.largeCardRadius
        color: Theme.page
        border.color: Theme.windowEdge
        MouseArea { anchors.fill: parent }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: qsTr("未完成项"); color: Theme.text; font.pixelSize: Theme.fontTitle }
                FlatButton { id: closeButton; text: qsTr("关闭"); onClicked: popup.open = false }
            }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
            Flickable {
                id: scroll
                anchors.fill: parent
                contentWidth: width
                contentHeight: details.height
                clip: true
                TextEdit {
                    id: details
                    width: scroll.width - 12
                    text: popup.editor.errors
                    readOnly: true
                    activeFocusOnTab: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                ListWheelScroll { view: scroll; animate: SystemTheme.animations }
            }
            ListScrollBar { anchors.top: scroll.top; anchors.bottom: scroll.bottom; anchors.right: scroll.right; flickable: scroll }
            }
        }
    }
    Keys.onEscapePressed: popup.open = false
    Shortcut { sequence: "Escape"; enabled: popup.open; onActivated: popup.open = false }
    Shortcut { sequence: "Tab"; enabled: popup.open; onActivated: details.activeFocus ? closeButton.forceActiveFocus() : details.forceActiveFocus() }
    Shortcut { sequence: "Shift+Tab"; enabled: popup.open; onActivated: details.activeFocus ? closeButton.forceActiveFocus() : details.forceActiveFocus() }
    onOpenChanged: {
        if (open) {
            closeButton.forceActiveFocus()
            popup.frame.addPopup(popup)
        }
    }
}
