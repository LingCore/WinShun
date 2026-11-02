pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

ColumnLayout {
    id: pane
    required property Controller editor
    spacing: 0
    RowLayout {
        Layout.fillWidth: true
        Layout.margins: 16
        spacing: 12
        InputBox {
            id: query
            objectName: "ocrSearchInput"
            Layout.fillWidth: true
            placeholder: qsTr("搜索图片中的文字…")
            onTextChanged: debounce.restart()
            onAccepted: { debounce.stop(); pane.editor.search(text) }
        }
        Text {
            text: pane.editor.searching ? qsTr("搜索中…") : qsTr("%1 个结果").arg(pane.editor.results.length)
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }
        Timer { id: debounce; interval: 150; onTriggered: pane.editor.search(query.text) }
    }
    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
    GridLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        columns: width > 500 ? 2 : 1
        rowSpacing: 0
        columnSpacing: 0
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 240
            Layout.maximumHeight: parent.columns === 1 ? 210 : 10000
            ListView {
                id: list
                objectName: "ocrSearchResults"
                anchors.fill: parent
                model: pane.editor.results
                currentIndex: 0
                clip: true
                reuseItems: true
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                onCurrentIndexChanged: pane.editor.select(currentIndex)
                Keys.onReturnPressed: {
                    if (currentIndex >= 0 && currentIndex < pane.editor.results.length)
                        pane.editor.openFile(pane.editor.results[currentIndex].path)
                }
                delegate: OcrResultRow {
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    item: modelData
                    selected: ListView.isCurrentItem
                    onChosen: { list.currentIndex = index; pane.editor.select(index) }
                    onOpened: pane.editor.openFile(modelData.path)
                }
                ListWheelScroll { view: list; animate: SystemTheme.animations }
            }
            ListScrollBar { anchors.top: list.top; anchors.bottom: list.bottom; anchors.right: list.right; flickable: list; z: 2 }
            Text {
                anchors.centerIn: parent
                width: parent.width - 40
                horizontalAlignment: Text.AlignHCenter
                text: query.text.length === 0 ? qsTr("输入关键词即可搜索\n也可以在启动器输入 ocr 关键词") : qsTr("没有找到匹配的文字\n可检查识别范围和未完成项")
                visible: pane.editor.results.length === 0 && !pane.editor.searching
                wrapMode: Text.Wrap
                color: Theme.subtext
                font.pixelSize: Theme.fontBody
                lineHeight: 1.5
            }
        }
        OcrPreviewPane {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 480
            editor: pane.editor
        }
    }
}
