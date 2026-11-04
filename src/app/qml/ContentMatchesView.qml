pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import WinShun

Item {
    id: root
    property real dpr: 0 // the window's, passed by the host (GitHub #111)
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio // Screen only without one
    required property ContentPane pane
    property bool textView: false
    readonly property bool matchCase: pane.matchCase
    readonly property bool wholeWord: pane.wholeWord
    readonly property bool regex: pane.regex
    function focusFilter() { filter.takeFocus() }
    function applyFilter() { pane.setFilter(filter.text, matchCase, wholeWord, regex) }
    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 10
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            spacing: 4
            InputBox {
                dpr: root.pixelRatio
                id: filter
                objectName: "contentHitsFilter"
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                placeholder: qsTr("在匹配中筛选")
                Accessible.name: placeholder
                maximumLength: 2048
                text: root.pane.filterText
                onTextChanged: root.applyFilter()
                onAccepted: root.pane.nextMatch(1)
            }
            FlatButton {
                dpr: root.pixelRatio
                text: "Aa"; Layout.preferredWidth: 32; Layout.preferredHeight: 32
                highlighted: root.matchCase; flat: true
                onClicked: root.pane.setFilter(filter.text, !root.matchCase, root.wholeWord, root.regex)
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("区分大小写")
                Accessible.checked: root.matchCase
            }
            FlatButton {
                dpr: root.pixelRatio
                text: qsTr("词"); Layout.preferredWidth: 32; Layout.preferredHeight: 32
                highlighted: root.wholeWord; flat: true
                onClicked: root.pane.setFilter(filter.text, root.matchCase, !root.wholeWord, root.regex)
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("全词匹配")
                Accessible.checked: root.wholeWord
            }
            FlatButton {
                dpr: root.pixelRatio
                text: ".*"; Layout.preferredWidth: 32; Layout.preferredHeight: 32
                highlighted: root.regex; flat: true
                onClicked: root.pane.setFilter(filter.text, root.matchCase, root.wholeWord, !root.regex)
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("正则表达式")
                Accessible.checked: root.regex
            }
        }
        Flow {
            id: termsFlow
            Layout.fillWidth: true
            Layout.leftMargin: 10; Layout.rightMargin: 10
            spacing: 4
            Repeater {
                model: root.pane.terms
                delegate: ContentChip {
                    required property int index
                    required property var modelData
                    text: modelData.text + " · " + modelData.count
                    maximumWidth: termsFlow.width
                    checked: modelData.enabled
                    onClicked: root.pane.toggleTerm(index)
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: text
                    Accessible.checked: modelData.enabled
                }
            }
        }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            Layout.leftMargin: 10; Layout.rightMargin: 10
            visible: root.textView && root.pane.sheets.length > 0
            orientation: ListView.Horizontal
            clip: true
            reuseItems: true
            spacing: 4
            header: ContentChip {
                text: qsTr("全部工作表")
                checked: root.pane.sheet === -1
                onClicked: root.pane.sheet = -1
                Accessible.role: Accessible.RadioButton
                Accessible.name: text
                Accessible.checked: root.pane.sheet === -1
            }
            model: root.pane.sheets
            delegate: ContentChip {
                required property int index
                required property var modelData
                text: modelData.text + " · " + modelData.count
                checked: root.pane.sheet === index
                onClicked: root.pane.sheet = index
                Accessible.role: Accessible.RadioButton
                Accessible.name: text
                Accessible.checked: root.pane.sheet === index
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 10; Layout.rightMargin: 10
            Text {
                Layout.fillWidth: true
                text: root.pane.matchStatus
                color: Theme.subtext; font.pixelSize: Theme.uiCaption
                wrapMode: Text.Wrap
            }
            FlatButton {
                dpr: root.pixelRatio
                visible: !root.textView
                height: 28; flat: true
                text: qsTr("上下文 %1").arg(root.pane.context)
                onClicked: root.pane.context = (root.pane.context + 1) % 4
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("调整上下文行数，当前 %1 行").arg(root.pane.context)
            }
            FlatButton {
                dpr: root.pixelRatio
                text: "↑"; height: 28; width: 28; flat: true
                onClicked: root.pane.nextMatch(-1)
                Accessible.role: Accessible.Button; Accessible.name: qsTr("上一处匹配，Shift+F3")
            }
            FlatButton {
                dpr: root.pixelRatio
                text: "↓"; height: 28; width: 28; flat: true
                onClicked: root.pane.nextMatch(1)
                Accessible.role: Accessible.Button; Accessible.name: qsTr("下一处匹配，F3")
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 10; Layout.rightMargin: 10
            visible: text.length > 0
            text: root.pane.message
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.subtext; font.pixelSize: Theme.uiCaption
        }
        ContentLinesView {
            dpr: root.pixelRatio
            Layout.fillWidth: true
            Layout.fillHeight: true
            pane: root.pane
            textView: root.textView
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            FlatButton {
                dpr: root.pixelRatio
                text: root.textView ? qsTr("复制文本") : qsTr("复制匹配")
                Layout.preferredHeight: 30
                enabled: !root.pane.busy && root.pane.path.length > 0
                onClicked: root.pane.copyText(!root.textView)
                Accessible.role: Accessible.Button; Accessible.name: text
            }
            Item { Layout.fillWidth: true }
            FlatButton {
                dpr: root.pixelRatio
                text: qsTr("刷新"); flat: true
                Layout.preferredHeight: 30
                onClicked: root.pane.refresh()
                Accessible.role: Accessible.Button; Accessible.name: text
            }
        }
    }
}
