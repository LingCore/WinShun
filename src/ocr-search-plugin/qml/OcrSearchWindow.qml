pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

Window {
    id: window
    required property Controller editor
    required property WindowFrame frame
    readonly property real dpr: Screen.devicePixelRatio
    objectName: "ocrSearchWindow"
    width: 1080
    height: 720
    minimumWidth: 820
    minimumHeight: 540
    color: Theme.page
    flags: Qt.Window | Qt.FramelessWindowHint
    title: qsTr("图片文字搜索")

    TitleBar {
        id: titleBar
        width: parent.width
        frame: window.frame
        title: window.title + " - Win顺"
    }
    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: footer.top
        spacing: 0
        OcrLibraryPane {
            editor: window.editor
            frame: window.frame
            Layout.preferredWidth: 270
            Layout.fillHeight: true
        }
        Hook { // the style's line between the sidebar and the results (Theme.separator)
            id: sideSeam
            Layout.fillHeight: true
            Layout.preferredWidth: sideSeam.out("thickness", Theme.edge(window.dpr))
            part: Theme.separator
            orientation: Qt.Vertical
            dpr: window.dpr
        }
        OcrSearchPane { editor: window.editor; Layout.fillWidth: true; Layout.fillHeight: true }
    }
    Rectangle {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 38
        color: Theme.background
        Hook {
            id: footerSeam
            width: parent.width
            height: footerSeam.out("thickness", Theme.edge(window.dpr))
            part: Theme.separator
            dpr: window.dpr
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            Text {
                id: statusLabel
                Layout.fillWidth: true
                text: window.editor.statusText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
                HoverHandler { id: statusHover }
                HoverTip { target: statusHover.hovered ? statusLabel : null; text: window.editor.statusText }
            }
            Text { text: qsTr("本机识别 · 原文件只读"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
        }
    }
    WindowEdge { id: windowEdge; anchors.fill: parent; fill: Theme.page }
}
