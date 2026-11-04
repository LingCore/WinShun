pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

FocusScope {
    id: popup
    required property Controller editor
    required property WindowFrame frame
    property bool open: false
    readonly property bool hasUnfinished: editor.errors.length > 0
    readonly property bool hasTextless: editor.textless.length > 0
    // Section titles only when both lists are shown; the window title names a single one.
    readonly property bool both: hasUnfinished && hasTextless
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
                Text {
                    Layout.fillWidth: true
                    text: popup.both ? qsTr("未完成和没有文字的文件") : popup.hasUnfinished ? qsTr("未完成项") : qsTr("没有文字的文件")
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                }
                FlatButton { id: closeButton; text: qsTr("关闭"); onClicked: popup.open = false }
            }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
            Flickable {
                id: scroll
                anchors.fill: parent
                contentWidth: width
                contentHeight: sections.implicitHeight
                clip: true
                ColumnLayout {
                    id: sections
                    width: scroll.width - 12
                    spacing: 8
                    Text {
                        visible: popup.both
                        text: qsTr("未完成（%n 个文件）", "", popup.editor.statistics.unfinished ?? 0)
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Theme.fontStrong
                    }
                    TextEdit {
                        id: details
                        Layout.fillWidth: true
                        visible: popup.hasUnfinished
                        text: popup.editor.errors
                        readOnly: true
                        activeFocusOnTab: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        Layout.topMargin: 12
                        visible: popup.both
                        text: qsTr("没有识别到文字（%n 个文件）", "", popup.editor.statistics.textless ?? 0)
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Theme.fontStrong
                    }
                    TextEdit {
                        id: textlessDetails
                        Layout.fillWidth: true
                        visible: popup.hasTextless
                        text: popup.editor.textless
                        readOnly: true
                        activeFocusOnTab: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                }
                ListWheelScroll { view: scroll; animate: SystemTheme.animations }
            }
            ListScrollBar { anchors.top: scroll.top; anchors.bottom: scroll.bottom; anchors.right: scroll.right; flickable: scroll }
            }
        }
    }
    // Tab and Shift+Tab go round the close button and the shown lists.
    function moveFocus(step: int) {
        const shown = [closeButton, details, textlessDetails].filter(item => item.visible)
        const at = shown.findIndex(item => item.activeFocus)
        const next = at < 0 ? (step > 0 ? 0 : shown.length - 1) : (at + step + shown.length) % shown.length
        shown[next].forceActiveFocus()
    }
    Keys.onEscapePressed: popup.open = false
    Shortcut { sequence: "Escape"; enabled: popup.open; onActivated: popup.open = false }
    Shortcut { sequence: "Tab"; enabled: popup.open; onActivated: popup.moveFocus(1) }
    Shortcut { sequence: "Shift+Tab"; enabled: popup.open; onActivated: popup.moveFocus(-1) }
    onOpenChanged: {
        if (open) {
            closeButton.forceActiveFocus()
            popup.frame.addPopup(popup)
        }
    }
}
