pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Win顺's own preview of a result (FilePreview, Alt+P on a row): its name and
// 打开 over what PreviewBody shows (a picture, text, a document's handler, or
// a thumbnail and what is known about the file). Beside the launcher, never
// taking the focus from it: the keys stay there, Alt+P or the × closes this,
// ↑ ↓ show the next row's file here.
Window {
    id: window

    required property FilePreview preview
    required property WindowFrame frame

    readonly property int padding: 16
    readonly property real dpr: Screen.devicePixelRatio

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    // See-through to Mica, as the launcher; drawn as active though it never is (WindowFrame::setNoActivate).
    color: Theme.backdrop && SystemTheme.materials ? "transparent" : Theme.background
    title: qsTr("预览") // Qt adds " - Win顺"

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    // ---- the header: the file, what it is, 打开 and close ----------------------------

    Item {
        id: header
        width: parent.width
        height: Theme.headerHeight
        Component.onCompleted: window.frame.addDragArea(header)

        Image {
            id: icon
            x: window.padding
            anchors.verticalCenter: parent.verticalCenter
            width: 24
            height: 24
            sourceSize: Qt.size(24 * window.dpr, 24 * window.dpr) // image://: logical × the scale
            source: window.preview.iconUrl
            asynchronous: true
            smooth: false
        }
        Column {
            anchors.left: icon.right
            anchors.leftMargin: 10
            anchors.right: buttons.left
            anchors.rightMargin: 12
            y: window.onPixel((header.height - height) / 2)
            spacing: 1

            Text {
                width: parent.width
                text: window.preview.name
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: Theme.uiTitle
                font.weight: Theme.fontStrong
            }
            Text {
                width: parent.width
                visible: text.length > 0
                text: window.preview.summary
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.subtext
                font.pixelSize: Theme.uiCaption
            }
        }
        Row {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            FlatButton {
                dpr: window.dpr
                id: openButton
                visible: window.preview.path.length > 0
                flat: true
                height: 32
                glyph: "" // OpenInNewWindow
                text: qsTr("打开")
                onClicked: window.preview.openFile()
                Component.onCompleted: window.frame.addControl(openButton)
            }
            FlatButton {
                dpr: window.dpr
                id: closeButton
                flat: true
                width: 32
                height: 32
                glyph: "" // ChromeClose
                onClicked: window.preview.closeByUser()
                Component.onCompleted: window.frame.addControl(closeButton)
            }
        }
        Hook { // the style's seam
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.edge(window.dpr)
            part: Theme.separator
            dpr: window.dpr
        }
    }

    // ---- the content -----------------------------------------------------------------

    PreviewBody {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        preview: window.preview
        dpr: window.dpr
        padding: window.padding
    }

    WindowEdge {
        id: windowEdge
        fill: Theme.background
    }
}
