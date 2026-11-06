pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import WinShun

Rectangle {
    id: root
    required property ContentPane pane
    required property Launcher launcher
    property bool compact: false
    signal searchFocusRequested()
    color: Theme.background
    objectName: "contentPaneView"

    function handleKey(event) {
        const mods = event.modifiers & ~Qt.KeypadModifier
        if (mods === (Qt.ControlModifier | Qt.ShiftModifier) && event.key >= Qt.Key_1 && event.key <= Qt.Key_5) {
            pane.tab = event.key - Qt.Key_1
        } else if (event.key === Qt.Key_F3 && (mods === Qt.NoModifier || mods === Qt.ShiftModifier)) {
            if (pane.tab !== 1 && pane.tab !== 2) pane.tab = 1
            pane.nextMatch(mods === Qt.ShiftModifier ? -1 : 1)
        } else if (event.key === Qt.Key_F && mods === Qt.ControlModifier) {
            pane.tab = 1
            Qt.callLater(() => { if (page.status === Loader.Ready && page.item.focusFilter) page.item.focusFilter() })
        } else return false
        event.accepted = true
        return true
    }
    Keys.onPressed: event => {
        if (root.handleKey(event)) return
        if (event.key === Qt.Key_Escape) { root.searchFocusRequested(); event.accepted = true }
    }
    Binding { target: root.pane; property: "highlightColor"; value: Theme.accent }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            spacing: 6
            Text {
                Layout.fillWidth: true
                text: root.pane.tab === 0 || root.pane.tab === 4 ? qsTr("本次内容搜索")
                    : root.pane.title.length > 0 ? root.pane.title : qsTr("内容详情")
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: Theme.uiBody
                font.weight: Theme.fontStrong
            }
            FlatButton {
                Layout.preferredHeight: 30
                flat: true
                text: qsTr("打开")
                visible: root.pane.tab > 0 && root.pane.tab < 4
                enabled: root.pane.title.length > 0 && !root.pane.busy
                onClicked: root.pane.openCurrent()
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("打开当前文件")
            }
            FlatButton {
                Layout.preferredHeight: 30
                Layout.preferredWidth: root.compact ? implicitWidth : 30
                flat: true
                glyph: root.compact ? "" : ""
                text: root.compact ? qsTr("返回列表") : ""
                onClicked: { root.pane.opened = false; root.searchFocusRequested() }
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("收起内容详情")
            }
        }
        TabBar {
            id: tabs
            implicitWidth: 320
            implicitHeight: 36
            objectName: "contentPaneTabs"
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            spacing: 0
            currentIndex: root.pane.tab
            background: Rectangle { color: "transparent" }
            Repeater {
                model: [qsTr("摘要"), qsTr("匹配"), qsTr("文本"), qsTr("预览"), qsTr("报告")]
                delegate: TabButton {
                    id: tab
                    required property int index
                    required property string modelData
                    width: tabs.availableWidth / 5
                    implicitWidth: 64
                    height: 36
                    leftPadding: 4
                    rightPadding: 4
                    text: modelData
                    onClicked: root.pane.tab = index
                    Accessible.name: modelData
                    contentItem: Text {
                        text: tab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        color: tab.checked ? Theme.text : Theme.subtext
                        font.pixelSize: Theme.uiBody
                        font.weight: tab.checked ? Theme.fontStrong : Font.Normal
                    }
                    background: Rectangle {
                        color: tab.hovered ? Theme.hover : "transparent"
                        border.width: 0
                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 20; height: 3; radius: Theme.smallRadius
                            color: Theme.accent
                            visible: tab.checked
                        }
                    }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: Theme.edge(Screen.devicePixelRatio); color: Theme.divider }
        Loader {
            id: page
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: root.pane.tab === 0 ? summaryPage : root.pane.tab === 1 ? hitsPage
                : root.pane.tab === 2 ? textPage : root.pane.tab === 3 ? previewPage : reportPage
        }
    }
    Component { id: summaryPage; ContentSummaryView { pane: root.pane; launcher: root.launcher } }
    Component { id: hitsPage; ContentMatchesView { pane: root.pane } }
    Component { id: textPage; ContentMatchesView { pane: root.pane; textView: true } }
    Component { id: previewPage; ContentPreviewView { pane: root.pane } }
    Component { id: reportPage; ContentReportView { pane: root.pane } }
}
