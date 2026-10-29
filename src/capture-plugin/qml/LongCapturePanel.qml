pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Keep the original selection's controls visible while the target receives
// wheel input. Preview is a separate window so it cannot displace the actions.
Window {
    id: panel
    required property LongCapture capture
    readonly property color edgeColor: Theme.accent
    readonly property int padding: 12
    readonly property real dpr: Screen.devicePixelRatio
    readonly property var statusWords: [qsTr("等待滚动"), qsTr("拼接中"), qsTr("已截取的内容"), qsTr("需要接回"), qsTr("到上限")]
    readonly property real statusWidth: statusWords.reduce((width, word) => Math.max(width, captionMetrics.advanceWidth(word)), 0)
    property bool previewVisible: true

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.background
    title: qsTr("长截图")
    width: Math.max(actions.implicitWidth + 2 * padding, heading.implicitWidth + automatic.implicitWidth + 3 * padding)
    height: column.implicitHeight + 2 * padding
    onClosing: (event) => { event.accepted = false; capture.cancel() }

    FontMetrics { id: captionMetrics; font.pixelSize: Theme.fontCaption }
    Column {
        id: column
        x: panel.padding; y: panel.padding
        width: panel.width - 2 * panel.padding
        spacing: 8
        Item {
            width: parent.width
            height: Math.max(heading.implicitHeight, automatic.implicitHeight)
            DragHandler {
                target: null
                onActiveChanged: if (active) panel.startSystemMove()
            }
            Row {
                id: heading
                spacing: 10
                Text { text: qsTr("长截图"); color: Theme.text; font.pixelSize: Theme.uiTitle; font.weight: Font.DemiBold }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.statusWidth
                    text: panel.statusWords[panel.capture.status] ?? ""
                    color: panel.capture.status === LongCapture.Lost ? Theme.danger : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: captionMetrics.advanceWidth(qsTr("高 %1 像素").arg(30000))
                    text: qsTr("高 %1 像素").arg(panel.capture.pictureHeight)
                    color: Theme.subtext; font.pixelSize: Theme.fontCaption
                }
            }
            Row {
                id: automatic
                anchors.right: parent.right
                spacing: 8
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("自动滚动"); color: Theme.subtext; font.pixelSize: Theme.fontCaption
                }
                ToggleSwitch {
                    objectName: "longCaptureAutoScroll"
                    word: false
                    enabled: panel.capture.canAutoScroll
                    checked: panel.capture.autoScroll && panel.capture.canAutoScroll
                    onToggled: (on) => panel.capture.autoScroll = on
                }
            }
        }
        Text {
            width: parent.width
            height: 2 * Math.ceil(captionMetrics.height)
            text: panel.capture.partial && panel.capture.status !== LongCapture.Lost
                  ? qsTr("有一段是推测着接上的，请检查预览") : panel.capture.hint
            color: panel.capture.status === LongCapture.Lost ? Theme.danger : Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight
        }
        Row {
            id: actions
            spacing: 6
            FlatButton {
                objectName: "longCapturePreviewToggle"
                text: qsTr("预览"); glyph: String.fromCharCode(0xE890)
                flat: !panel.previewVisible; keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.previewVisible = !panel.previewVisible
            }
            FlatButton {
                text: qsTr("诊断"); flat: true; keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: qsTr("复制诊断信息")
                onClicked: panel.capture.copyDiagnostics()
            }
            FlatButton {
                objectName: "longCapturePin"
                text: qsTr("贴图"); glyph: String.fromCharCode(0xE718); keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.capture.exportTo(LongCapture.Pin)
            }
            FlatButton {
                objectName: "longCaptureSave"
                text: qsTr("保存"); glyph: String.fromCharCode(0xE74E); keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.capture.exportTo(LongCapture.Save)
            }
            FlatButton {
                objectName: "longCaptureDone"
                text: qsTr("完成"); keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: qsTr("完成并编辑")
                onClicked: panel.capture.finish()
            }
            FlatButton {
                objectName: "longCaptureCancel"
                text: qsTr("取消"); keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.capture.cancel()
            }
            FlatButton {
                objectName: "longCaptureCopy"
                text: qsTr("复制"); glyph: String.fromCharCode(0xE73E); highlighted: true; keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.capture.exportTo(LongCapture.Copy)
            }
        }
    }
    WindowEdge {}

    Window {
        id: previewWindow
        objectName: "longCapturePreview"
        flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
        visible: panel.visible && panel.previewVisible
        color: Theme.background
        title: qsTr("长图预览")
        width: panel.capture.previewBox.width + 2 * panel.padding
        height: previewHeading.implicitHeight + panel.capture.previewBox.height + 3 * panel.padding
        onClosing: (event) => { event.accepted = false; panel.previewVisible = false }
        Text {
            id: previewHeading
            x: panel.padding; y: panel.padding
            text: qsTr("长图预览"); color: Theme.text; font.pixelSize: Theme.fontBody
        }
        Item {
            x: panel.padding; y: previewHeading.y + previewHeading.implicitHeight + panel.padding
            width: panel.capture.previewBox.width; height: panel.capture.previewBox.height
            CaptureImage {
                id: shot
                x: Math.round((parent.width - width) / 2 * panel.dpr) / panel.dpr
                width: panel.capture.previewSize.width / panel.dpr
                height: panel.capture.previewSize.height / panel.dpr
                image: panel.capture.preview
            }
            Rectangle {
                visible: panel.capture.hasPreview
                x: shot.x - 1; y: -1; width: shot.width + 2; height: shot.height + 2
                color: "transparent"; border.width: 1; border.color: Theme.windowEdge
            }
            Text {
                anchors.centerIn: parent
                visible: !panel.capture.hasPreview
                text: qsTr("等第一帧…"); color: Theme.subtext; font.pixelSize: Theme.fontCaption
            }
        }
        WindowEdge {}
    }
}
