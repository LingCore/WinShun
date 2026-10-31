pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import WinShun

ScrollView {
    id: root
    required property ContentPane pane
    required property Launcher launcher
    contentWidth: availableWidth
    ColumnLayout {
        width: root.availableWidth
        spacing: 10
        Text {
            Layout.fillWidth: true; Layout.margins: 12
            text: root.pane.summary
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.text; font.pixelSize: Theme.uiBody
            lineHeight: 1.4
        }
        FlatButton {
            Layout.leftMargin: 12
            text: root.pane.reporting ? qsTr("停止统计") : qsTr("统计全部匹配")
            onClicked: root.pane.reporting ? root.pane.cancelReport() : root.pane.prepareReport(false, true, true, false, false)
            Accessible.role: Accessible.Button; Accessible.name: text
        }
        Text {
            Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
            visible: text.length > 0
            text: root.pane.reportReady ? root.pane.reportPreview : root.pane.reportStatus
            textFormat: Text.PlainText; wrapMode: Text.Wrap
            color: Theme.subtext; font.pixelSize: Theme.uiCaption
        }
        Repeater {
            model: root.launcher.leftOutHints
            delegate: ColumnLayout {
                id: hintRow
                required property var modelData
                Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
                Text {
                    Layout.fillWidth: true
                    text: hintRow.modelData.text
                    textFormat: Text.PlainText; wrapMode: Text.Wrap
                    color: Theme.subtext; font.pixelSize: Theme.uiBody
                }
                FlatButton {
                    text: hintRow.modelData.action
                    onClicked: root.launcher.searchLeftOut(hintRow.modelData.kind)
                    Accessible.role: Accessible.Button; Accessible.name: text
                }
            }
        }
        Repeater {
            model: root.pane.groups
            delegate: ColumnLayout {
                id: group
                required property var modelData
                Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
                spacing: 3
                Text { text: group.modelData.kind; color: Theme.subtext; font.pixelSize: Theme.uiCaption }
                Text {
                    Layout.fillWidth: true
                    text: group.modelData.name + " · " + group.modelData.count
                    textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere
                    color: Theme.text; font.pixelSize: Theme.uiBody
                }
            }
        }
        Item { Layout.preferredHeight: 12 }
    }
}
