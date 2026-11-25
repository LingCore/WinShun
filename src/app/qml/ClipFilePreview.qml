pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// One local file or folder copied, in the clipboard's preview beside the
// list: what the search's preview shows of it (GitHub #324), a picture, the
// start of a text file, a document through its handler or what is known of
// it, under its name. The Clipboard shows it (showFile) only while this is
// on the screen; a document's handler is a window of its own over it.
Item {
    id: view

    required property var host // Clipboard
    required property UiScale metrics
    required property var file // Clipboard.preview's files[0]: name, folder, icon
    required property string path // empty: none on this row
    required property bool isDir
    property int padding: 16
    // Where it is in the window (its host's place): moved, the handler goes along.
    property real offsetX
    property real offsetY
    readonly property FilePreview filePreview: host.filePreview
    readonly property var hostWindow: Window.window
    // A picture: the badge in its corner opens it large (ClipPictureViewer,
    // GitHub #414); the wheel and a double click zoom it here as ever.
    property bool pictureViewed: false
    signal pictureClicked()

    function place() { host.showFile(view, path, isDir, Theme.dark) }
    function placeSoon() { Qt.callLater(place) }
    // Ctrl+C: a text file's lines picked with the mouse, copied as anyone's
    // copy; false: none are.
    function copySelection(): bool {
        const picked = visible ? filePreview.selectedText() : ""
        if (picked.length > 0)
            host.copyText(picked, true)
        return picked.length > 0
    }

    objectName: "clipboardPreviewFile"
    Component.onCompleted: placeSoon()
    Component.onDestruction: host.showFile(null, "", false, Theme.dark)
    onPathChanged: placeSoon()
    onVisibleChanged: placeSoon()
    onWidthChanged: placeSoon()
    onHeightChanged: placeSoon()
    onOffsetXChanged: placeSoon()
    onOffsetYChanged: placeSoon()
    Connections {
        target: view.hostWindow
        function onXChanged() { view.place() }
        function onYChanged() { view.place() }
        function onVisibleChanged() { view.placeSoon() }
        function onScreenChanged() { view.placeSoon() }
    }
    Connections { target: Theme; function onDarkChanged() { view.placeSoon() } }

    Item { // the file, as in the list of files
        id: header
        x: view.padding
        y: 12
        width: parent.width - 2 * view.padding
        height: view.metrics.fileRowHeight

        Image {
            id: icon
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            source: view.file.icon ?? ""
            sourceSize.width: 20
            sourceSize.height: 20
            asynchronous: true
        }
        Column {
            anchors.left: icon.right
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter

            Text {
                width: parent.width
                text: view.file.name ?? ""
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: view.metrics.uiBody
            }
            Text {
                width: parent.width
                text: view.file.folder ?? ""
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.faint
                font.pixelSize: view.metrics.uiCaption
            }
        }
    }
    Text { // the type, the size, when written; a text's encoding and lines, a picture's size
        id: summary
        x: view.padding
        anchors.top: header.bottom
        anchors.topMargin: 2
        width: parent.width - 2 * view.padding
        visible: text.length > 0
        text: view.filePreview.summary
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: Theme.subtext
        font.pixelSize: view.metrics.uiCaption
    }
    HoverHandler { id: pointer }
    PreviewBody {
        id: body
        objectName: "clipboardPreviewFileBody"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: summary.visible ? summary.bottom : header.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: 4
        preview: view.filePreview
        dpr: Screen.devicePixelRatio
        padding: view.padding
    }
    Rectangle { // a picture's: shows it large
        objectName: "clipboardFilePictureZoom"
        visible: view.filePreview.kind === FilePreview.Picture && (pointer.hovered || view.pictureViewed)
        anchors.right: body.right
        anchors.top: body.top
        anchors.margins: view.padding
        width: 28
        height: 28
        radius: Theme.controlRadius
        color: zoomArea.containsMouse ? Theme.background : Qt.alpha(Theme.background, 0.9)
        border.width: 1
        border.color: Theme.divider
        Accessible.role: Accessible.Button
        Accessible.name: view.pictureViewed ? qsTr("关闭大图") : qsTr("看大图")
        Accessible.onPressAction: view.pictureClicked()
        Glyph {
            anchors.centerIn: parent
            iconName: view.pictureViewed ? "magnifying-glass-minus" : "magnifying-glass-plus"
            size: 18
            color: Theme.text
        }
        MouseArea {
            id: zoomArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: view.pictureClicked()
        }
        HoverTip {
            dpr: Screen.devicePixelRatio
            kind: "action"
            target: zoomArea.containsMouse ? zoomArea : null
            text: view.pictureViewed ? qsTr("关闭大图") : qsTr("看大图")
        }
    }
}
