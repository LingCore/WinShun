pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The search panel's 预览 tab: what the launcher's own preview shows (a
// picture, text, a document through its handler, or what is known about
// the file), here in the page (GitHub #224), with what it is above it.
Item {
    id: root
    required property ContentPane pane
    property real dpr: 0 // the window's, passed by the host
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio // Screen only without one
    readonly property FilePreview preview: pane.filePreview
    readonly property var hostWindow: Window.window
    objectName: "contentPreviewView"

    function positionPreview() { pane.setPreviewArea(root, Theme.dark); pane.movePreview() }
    Component.onCompleted: Qt.callLater(positionPreview)
    Component.onDestruction: pane.setPreviewArea(null, Theme.dark)
    onWidthChanged: Qt.callLater(positionPreview)
    onHeightChanged: Qt.callLater(positionPreview)
    onVisibleChanged: Qt.callLater(positionPreview)
    onXChanged: Qt.callLater(positionPreview)
    onYChanged: Qt.callLater(positionPreview)
    Connections {
        target: root.hostWindow
        function onXChanged() { root.positionPreview() }
        function onYChanged() { root.positionPreview() }
        function onVisibleChanged() { root.positionPreview() }
        function onScreenChanged() { root.positionPreview() }
    }
    Connections { target: Theme; function onDarkChanged() { root.positionPreview() } }

    Text { // the type, the size, when written; a text's encoding and lines, a picture's size
        id: summary
        x: 12
        y: 8
        width: parent.width - 24
        visible: text.length > 0
        text: root.preview.summary
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: Theme.subtext
        font.pixelSize: Theme.uiCaption
    }
    PreviewBody {
        objectName: "contentPreviewBody"
        anchors.fill: parent
        anchors.topMargin: summary.visible ? summary.y + summary.height + 2 : 0
        preview: root.preview
        dpr: root.pixelRatio
        padding: 12
    }
    Text { // nothing chosen yet
        anchors.centerIn: parent
        width: parent.width - 32
        visible: root.preview.path.length === 0 && root.preview.kind === FilePreview.Loading
        text: qsTr("选择文件查看原始外观")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: Theme.subtext
        font.pixelSize: Theme.uiBody
    }
}
