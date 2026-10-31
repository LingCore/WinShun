pragma ComponentBehavior: Bound
import QtQuick
import WinShun

Item {
    id: root
    required property ContentPane pane
    readonly property var hostWindow: Window.window
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
    Text {
        anchors.centerIn: parent
        width: parent.width - 32
        text: root.pane.previewMessage.length > 0 ? root.pane.previewMessage : qsTr("选择文件查看原始外观")
        textFormat: Text.PlainText; wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: Theme.subtext; font.pixelSize: Theme.uiBody
    }
}
