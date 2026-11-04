pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import WinShun

Rectangle {
    id: root
    property real dpr: 0 // the window's, passed by the host (GitHub #111)
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio // Screen only without one
    required property ContentPane pane
    required property Launcher launcher
    property bool compact: false
    property int hoveredTab: -1
    property int pressedTab: -1
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
    Binding { target: root.pane; property: "highlightColor"; value: Theme.matchColor }

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
                    : root.pane.path.length > 0 ? root.pane.path.split(/[\\/]/).pop() : qsTr("内容详情")
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: Theme.uiBody
                font.weight: Theme.fontStrong
            }
            FlatButton {
                dpr: root.pixelRatio
                Layout.preferredHeight: 30
                flat: true
                text: qsTr("打开")
                visible: root.pane.tab > 0 && root.pane.tab < 4
                enabled: root.pane.path.length > 0 && !root.pane.busy
                onClicked: root.pane.openCurrent()
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("打开当前文件")
            }
            FlatButton {
                dpr: root.pixelRatio
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
            // The style's tabs (Theme.segmented, role "tabs"): the hover, and the chosen one's mark.
            background: Hook {
                id: tabsPart
                part: Theme.segmented
                count: 5
                currentIndex: tabs.currentIndex
                hoveredIndex: root.hoveredTab
                pressedIndex: root.pressedTab
                segments: [0, 1, 2, 3, 4].map(i => ({ x: i * tabs.width / 5, width: tabs.width / 5 }))
                role: "tabs"
                dpr: root.pixelRatio
            }
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
                    onHoveredChanged: root.hoveredTab = hovered ? index : root.hoveredTab === index ? -1 : root.hoveredTab
                    onPressedChanged: root.pressedTab = pressed ? index : root.pressedTab === index ? -1 : root.pressedTab
                    Accessible.name: modelData
                    contentItem: Text {
                        text: tab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        readonly property real shift: tabsPart.call("labelOffsetY", [tab.index], 0) // the style's nudge of its labels
                        topPadding: shift > 0 ? 2 * shift : 0
                        bottomPadding: shift < 0 ? -2 * shift : 0
                        elide: Text.ElideRight
                        color: tabsPart.call("labelColor", [tab.index], tab.checked ? Theme.text : Theme.subtext)
                        font.pixelSize: Theme.uiBody
                        font.weight: tabsPart.call("labelWeight", [tab.index], tab.checked ? Theme.fontStrong : Font.Normal)
                    }
                    background: Item {} // the style's, under all of them (above)
                }
            }
        }
        Hook { // the style's seam
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.edge(root.pixelRatio)
            part: Theme.separator
            dpr: root.pixelRatio
        }
        Loader {
            id: page
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: root.pane.tab === 0 ? summaryPage : root.pane.tab === 1 ? hitsPage
                : root.pane.tab === 2 ? textPage : root.pane.tab === 3 ? previewPage : reportPage
        }
    }
    Component { id: summaryPage; ContentSummaryView { pane: root.pane; launcher: root.launcher } }
    Component { id: hitsPage; ContentMatchesView { pane: root.pane; dpr: root.pixelRatio } }
    Component { id: textPage; ContentMatchesView { pane: root.pane; textView: true; dpr: root.pixelRatio } }
    Component { id: previewPage; ContentPreviewView { pane: root.pane } }
    Component { id: reportPage; ContentReportView { pane: root.pane; dpr: root.pixelRatio } }
}
