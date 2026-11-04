pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

Item {
    id: pane
    required property Controller editor
    required property WindowFrame frame
    readonly property real dpr: Screen.devicePixelRatio
    // The style's line between two parts (Theme.separator), as thick as the style wants it.
    component Divider: Hook {
        id: seam
        height: seam.out("thickness", Theme.edge(seam.dpr))
        part: Theme.separator
    }
    readonly property real indexBytes: editor.statistics.bytes ?? 0
    readonly property string indexSize: indexBytes < 1048576 ? qsTr("%1 KB").arg(Math.ceil(indexBytes / 1024)) : qsTr("%1 MB").arg((indexBytes / 1048576).toFixed(1))
    Flickable {
        id: scroll
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: indexActions.top
        anchors.bottomMargin: 12
        contentWidth: width
        contentHeight: content.implicitHeight + 32
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ListWheelScroll { view: scroll; animate: SystemTheme.animations }
        ColumnLayout {
            id: content
            x: 16; y: 16
            width: scroll.width - 32
            spacing: 12
            Text { text: qsTr("识别范围"); color: Theme.text; font.pixelSize: Theme.fontTitle; font.weight: Theme.fontStrong }
            Text {
                Layout.fillWidth: true
                text: qsTr("搜索图片、PDF 和 Office 图片中的文字。选择目录后，在后台识别一次。索引建好后，在搜索框直接输入图片里的文字就能搜到；输入 ocr 关键词只看图片和扫描件结果。")
                wrapMode: Text.Wrap
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
            FlatButton {
                dpr: pane.dpr
                objectName: "addOcrFolder"
                Layout.fillWidth: true
                text: qsTr("添加文件夹")
                glyph: "\uE710"
                enabled: !pane.editor.busy
                onClicked: pane.editor.addFolder()
            }
            Repeater {
                model: pane.editor.folders
                delegate: OcrFolderRow {
                    required property int index
                    required property string modelData
                    Layout.fillWidth: true
                    path: modelData
                    enabled: !pane.editor.busy
                    onRemoved: pane.editor.removeFolder(index)
                }
            }
            Text {
                Layout.fillWidth: true
                visible: pane.editor.folders.length === 0
                text: qsTr("从工作文档、扫描件或截图目录开始。")
                wrapMode: Text.Wrap
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
            FlatButton {
                dpr: pane.dpr
                Layout.fillWidth: true
                text: qsTr("排除文件夹…")
                flat: true
                enabled: !pane.editor.busy
                onClicked: pane.editor.addFolder(true)
            }
            Repeater {
                model: pane.editor.excluded
                delegate: OcrFolderRow {
                    required property int index
                    required property string modelData
                    Layout.fillWidth: true
                    path: modelData
                    enabled: !pane.editor.busy
                    onRemoved: pane.editor.removeFolder(index, true)
                }
            }
            Divider { Layout.fillWidth: true; Layout.preferredHeight: out("thickness", Theme.edge(pane.dpr)); dpr: pane.dpr }
            Text { text: qsTr("索引"); color: Theme.text; font.pixelSize: Theme.fontBody; font.weight: Theme.fontStrong }
            Text {
                Layout.fillWidth: true
                text: qsTr("%1 个文件 · %2 页／张\n%3 / %4 MB")
                    .arg(pane.editor.statistics.files ?? 0).arg(pane.editor.statistics.pages ?? 0)
                    .arg(pane.indexSize).arg(pane.editor.capacityMB)
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                lineHeight: 1.4
            }
            FlatButton {
                dpr: pane.dpr
                id: capacity
                Layout.fillWidth: true
                text: qsTr("容量上限：%1 MB").arg(pane.editor.capacityMB)
                trailingGlyph: "\uE70D"
                enabled: !pane.editor.busy
                onClicked: capacityMenu.open = !capacityMenu.open
            }
            DropMenu {
                id: capacityMenu
                anchor: capacity
                entries: [{text: "256 MB", glyph: ""}, {text: "512 MB", glyph: ""}, {text: "1024 MB", glyph: ""}, {text: "2048 MB", glyph: ""}]
                onActivated: (index) => pane.editor.capacityMB = [256, 512, 1024, 2048][index]
            }
            FlatButton {
                dpr: pane.dpr
                Layout.fillWidth: true
                visible: pane.editor.setupNeeded
                text: qsTr("安装增强识别组件…")
                onClicked: pane.editor.setupRequested()
            }
            FlatButton {
                dpr: pane.dpr
                Layout.fillWidth: true
                text: qsTr("重新检查所有文件")
                flat: true
                enabled: !pane.editor.busy && !pane.editor.setupNeeded && pane.editor.folders.length > 0
                onClicked: pane.editor.start(true)
            }
            FlatButton {
                dpr: pane.dpr
                readonly property bool unfinished: pane.editor.errors.length > 0
                readonly property bool textless: pane.editor.textless.length > 0
                Layout.fillWidth: true
                text: unfinished && textless ? qsTr("查看未完成和没有文字的文件")
                    : unfinished ? qsTr("查看未完成项") : qsTr("查看没有文字的文件")
                flat: true
                visible: unfinished || textless
                onClicked: issues.open = !issues.open
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("仅识别本机文件。加密、损坏和暂不支持的格式会列入未完成项。")
                wrapMode: Text.Wrap
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
            FlatButton {
                dpr: pane.dpr
                id: clearButton
                Layout.fillWidth: true
                text: qsTr("清空索引…")
                flat: true
                enabled: !pane.editor.busy
                onClicked: confirm.open = !confirm.open
            }
            DropMenu {
                id: confirm
                anchor: clearButton
                entries: [{text: qsTr("清空识别数据，保留原文件"), glyph: "", danger: true}]
                onActivated: pane.editor.clearIndex()
            }
        }
    }
    ListScrollBar { anchors.top: scroll.top; anchors.bottom: scroll.bottom; anchors.right: scroll.right; flickable: scroll; z: 2 }
    ColumnLayout {
        id: indexActions
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: qsTr("自动更新"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
            ToggleSwitch {
                checked: pane.editor.autoUpdate
                word: false
                enabled: pane.editor.folders.length > 0 && !pane.editor.setupNeeded
                Accessible.name: qsTr("每 5 分钟检查变化")
                onToggled: (on) => pane.editor.setAutoUpdate(on)
            }
        }
        Text {
            // After an upgrade: the files the new rules read again, counting down.
            readonly property int remaining: pane.editor.statistics.recheck ?? 0
            Layout.fillWidth: true
            visible: remaining > 0
            text: pane.editor.busy
                ? qsTr("识别规则已更新：正在补认以前跳过的 PDF 页、超长图片和放反的页面（还剩 %n 个文件），期间照常可以搜索。", "", remaining)
                : pane.editor.autoUpdate
                ? qsTr("识别规则已更新：以前跳过的 PDF 页、超长图片和放反的页面要补认（%n 个文件），稍后自动进行，期间照常可以搜索。", "", remaining)
                : qsTr("识别规则已更新：以前跳过的 PDF 页、超长图片和放反的页面要补认（%n 个文件），点“开始更新”即可，期间照常可以搜索。", "", remaining)
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }
        Text {
            Layout.fillWidth: true
            visible: pane.editor.busy
            text: pane.editor.progressText
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: pane.editor.busy
            text: pane.editor.progressFile
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideMiddle
        }
        FlatButton {
            dpr: pane.dpr
            objectName: "startOcrIndex"
            Layout.fillWidth: true
            highlighted: true
            text: pane.editor.busy ? qsTr("暂停识别") : qsTr("开始更新")
            enabled: pane.editor.folders.length > 0 && !pane.editor.setupNeeded
            onClicked: pane.editor.busy ? pane.editor.pause() : pane.editor.start()
        }
    }
    OcrIssuesPopup { id: issues; editor: pane.editor; frame: pane.frame; parent: pane.Window.contentItem }
}
