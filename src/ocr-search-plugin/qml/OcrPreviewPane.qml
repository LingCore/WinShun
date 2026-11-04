pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

Rectangle {
    id: pane
    required property Controller editor
    property real zoom: 1
    readonly property real dpr: Screen.devicePixelRatio
    color: Theme.background
    Hook { // the style's line between the list and the picture (Theme.separator)
        id: seam
        height: parent.height
        width: seam.out("thickness", Theme.edge(pane.dpr))
        part: Theme.separator
        orientation: Qt.Vertical
        dpr: pane.dpr
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: pane.editor.preview.location ?? qsTr("命中位置"); color: Theme.text; font.pixelSize: Theme.fontBody; font.weight: Theme.fontStrong; elide: Text.ElideRight }
            FlatButton { glyph: "\uE738"; flat: true; role: "toolbar"; enabled: pane.zoom > 1; Accessible.name: qsTr("缩小"); onClicked: pane.zoom = Math.max(1, pane.zoom - 0.5) }
            FlatButton { text: qsTr("适合"); flat: true; role: "toolbar"; onClicked: pane.zoom = 1 }
            FlatButton { glyph: "\uE710"; flat: true; role: "toolbar"; enabled: pane.zoom < 4; Accessible.name: qsTr("放大"); onClicked: pane.zoom = Math.min(4, pane.zoom + 0.5) }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Flickable {
                id: viewport
                anchors.fill: parent
                contentWidth: Math.max(width, image.width)
                contentHeight: Math.max(height, image.height)
                boundsBehavior: Flickable.StopAtBounds
                clip: true
            Image {
                id: image
                objectName: "ocrPreviewImage"
                readonly property real ratio: (pane.editor.preview.width ?? 1) / Math.max(1, pane.editor.preview.height ?? 1)
                width: Math.min(viewport.width, viewport.height * ratio) * pane.zoom
                height: width / Math.max(0.001, ratio)
                x: Math.max(0, (viewport.width - width) / 2)
                y: Math.max(0, (viewport.height - height) / 2)
                source: pane.editor.preview.url ?? ""
                sourceSize: Qt.size(pane.editor.preview.width ?? 0, pane.editor.preview.height ?? 0)
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                Item {
                    x: (image.width - image.paintedWidth) / 2
                    y: (image.height - image.paintedHeight) / 2
                    width: image.paintedWidth
                    height: image.paintedHeight
                    Repeater {
                        model: pane.editor.preview.boxes ?? []
                        delegate: Item { // where the words are: a light accent wash and a thin accent line
                            required property var modelData
                            x: modelData.x * parent.width
                            y: modelData.y * parent.height
                            width: modelData.width * parent.width
                            height: modelData.height * parent.height
                            Accessible.ignored: true
                            Rectangle { anchors.fill: parent; color: Theme.accent; opacity: 0.18 }
                            Rectangle { anchors.fill: parent; color: "transparent"; border.color: Theme.accent; border.width: Theme.edge(pane.dpr) }
                        }
                    }
                }
            }
                ListWheelScroll { view: viewport; animate: SystemTheme.animations }
            }
            ListScrollBar { anchors.top: viewport.top; anchors.bottom: viewport.bottom; anchors.right: viewport.right; flickable: viewport; z: 2 }
            Text {
                anchors.centerIn: parent
                width: parent.width - 32
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: pane.editor.preview.loading ? qsTr("正在读取原图…") : pane.editor.preview.error ?? qsTr("选择结果，查看原图和命中文字")
                visible: !image.source.toString().length || pane.editor.preview.loading || image.status === Image.Error
                color: Theme.subtext
                font.pixelSize: Theme.fontBody
            }
        }
        Text {
            id: sourcePath
            Layout.fillWidth: true
            text: pane.editor.preview.path ?? ""
            elide: Text.ElideMiddle
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            HoverHandler { id: sourceHover }
            HoverTip { target: sourceHover.hovered ? sourcePath : null; text: pane.editor.preview.path ?? "" }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: (pane.editor.preview.path ?? "").length > 0
            FlatButton { text: qsTr("打开文件"); onClicked: pane.editor.openFile(pane.editor.preview.path) }
            FlatButton { text: qsTr("所在文件夹"); onClicked: pane.editor.showFolder(pane.editor.preview.path) }
            FlatButton { text: qsTr("复制文字"); enabled: (pane.editor.preview.text ?? "").length > 0; onClicked: pane.editor.copyText() }
        }
    }
    Connections {
        target: pane.editor
        function onPreviewChanged() {
            if (pane.editor.preview.loading) { pane.zoom = 1; viewport.contentX = 0; viewport.contentY = 0 }
        }
    }
}
