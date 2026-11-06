pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

ColumnLayout {
    id: pane
    required property Controller editor
    readonly property real dpr: Screen.devicePixelRatio
    spacing: 0

    // The style's search box (Theme.searchField, boxed): a magnifier, the words, the placeholder.
    component SearchBox: Item {
        id: box
        property alias text: input.text
        property string placeholder
        readonly property real dpr: Screen.devicePixelRatio
        signal accepted()

        implicitWidth: 220
        implicitHeight: 36

        Hook {
            id: field
            anchors.fill: parent
            part: Theme.searchField
            flat: false
            focused: input.activeFocus
            hovered: hover.hovered
            corner: Theme.fieldRadius
            dpr: box.dpr
        }
        HoverHandler { id: hover }
        MouseArea { // the margins beside the text take the focus too
            anchors.fill: parent
            cursorShape: Qt.IBeamCursor
            onPressed: input.forceActiveFocus(Qt.MouseFocusReason)
        }
        Glyph {
            id: magnifier
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            glyph: ""
            size: 16
            color: field.out("iconColor", Theme.subtext)
        }
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 36
            anchors.rightMargin: 12
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            color: field.out("textColor", Theme.text)
            font.pixelSize: Theme.fontBody
            selectByMouse: true
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            // The style's caret colour, where it has one (Qt's own caret otherwise).
            cursorDelegate: field.out("caretColor", "transparent").a > 0 ? caretPart : null
            Component {
                id: caretPart
                Caret { color: field.out("caretColor", Theme.text); dpr: box.dpr }
            }
            onAccepted: box.accepted()

            Text {
                anchors.fill: parent
                verticalAlignment: Text.AlignVCenter
                text: box.placeholder
                color: field.out("placeholderColor", Theme.faint)
                font: input.font
                elide: Text.ElideRight
                visible: input.text.length === 0 && input.preeditText.length === 0
            }
        }
    }

    // The style's line between two parts (Theme.separator), as thick as the style wants it.
    component Divider: Hook {
        id: seam
        height: seam.out("thickness", Theme.edge(seam.dpr))
        part: Theme.separator
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.margins: 16
        spacing: 12
        SearchBox {
            id: query
            objectName: "ocrSearchInput"
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
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
    Divider { Layout.fillWidth: true; Layout.preferredHeight: out("thickness", Theme.edge(pane.dpr)); dpr: pane.dpr }
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
            ListScrollBar { dpr: pane.dpr; anchors.top: list.top; anchors.bottom: list.bottom; anchors.right: list.right; flickable: list; z: 2 }
            Text {
                anchors.centerIn: parent
                width: parent.width - 40
                horizontalAlignment: Text.AlignHCenter
                text: query.text.length === 0 ? qsTr("输入关键词即可搜索\n在启动器里直接输入图片里的文字也能搜到；输入 ocr 关键词只看图片结果") : qsTr("没有找到匹配的文字\n可检查识别范围和未完成项")
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
