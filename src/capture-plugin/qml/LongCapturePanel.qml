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
    readonly property var speedWords: [qsTr("慢速"), qsTr("标准"), qsTr("快速")]
    readonly property real statusWidth: Math.ceil(statusWords.reduce((width, word) => Math.max(width, captionMetrics.advanceWidth(word)), 0))
    readonly property real dimensionWidth: Math.ceil(Math.max(
        captionMetrics.advanceWidth(qsTr("高 %1 像素").arg(30000)),
        captionMetrics.advanceWidth(qsTr("宽 %1 像素").arg(30000))))
    readonly property real directionWidth: Math.ceil(Math.max(
        bodyMetrics.advanceWidth(qsTr("竖向")),
        bodyMetrics.advanceWidth(qsTr("横向")))) + 44
    readonly property real speedWidth: Math.ceil(speedWords.reduce((width, word) => Math.max(width, bodyMetrics.advanceWidth(word)), 0)) + 44
    readonly property bool liveAuto: panel.capture.autoScroll && panel.capture.canAutoScroll
        && panel.capture.status !== LongCapture.Lost && panel.capture.status !== LongCapture.Full
    property bool previewVisible: true

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.background
    title: qsTr("长截图")
    width: Math.max(actions.implicitWidth + 2 * padding, heading.implicitWidth + automatic.implicitWidth + 3 * padding)
    height: column.implicitHeight + 2 * padding
    onClosing: (event) => { event.accepted = false; capture.cancel() }

    FontMetrics { id: captionMetrics; font.pixelSize: Theme.fontCaption }
    FontMetrics { id: bodyMetrics; font.pixelSize: Theme.fontBody }

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
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Rectangle {
                    id: statusDot
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8; height: 8; radius: 4
                    color: panel.capture.status === LongCapture.Lost
                           ? Theme.danger
                           : (panel.liveAuto || panel.capture.status === LongCapture.Stitching || panel.capture.status === LongCapture.Full)
                             ? Theme.accent
                             : Theme.subtext
                    opacity: panel.liveAuto ? 1.0
                           : (panel.capture.status === LongCapture.Waiting || panel.capture.status === LongCapture.Behind) ? 0.55 : 1.0
                    SequentialAnimation on opacity {
                        running: panel.visible && panel.liveAuto
                        loops: Animation.Infinite
                        NumberAnimation { from: 1.0; to: 0.35; duration: 650; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 0.35; to: 1.0; duration: 650; easing.type: Easing.InOutSine }
                    }
                }
                Text { text: qsTr("长截图"); color: Theme.text; font.pixelSize: Theme.uiTitle; font.weight: Theme.fontStrong }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.statusWidth
                    text: panel.statusWords[panel.capture.status] ?? ""
                    color: panel.capture.status === LongCapture.Lost ? Theme.danger : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.dimensionWidth
                    text: (panel.capture.horizontal ? qsTr("宽 %1 像素") : qsTr("高 %1 像素")).arg(panel.capture.pictureHeight)
                    color: Theme.subtext; font.pixelSize: Theme.fontCaption
                }
            }
            Row {
                id: automatic
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                // Up and down, or sideways (feedback 2026-11-16). Scrolled
                // sideways by hand at the start, it turns by itself.
                FlatButton {
                    dpr: panel.dpr
                    objectName: "longCaptureDirection"
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.directionWidth
                    implicitHeight: 28
                    flat: true; keyboardActivation: false
                    text: panel.capture.horizontal ? qsTr("横向") : qsTr("竖向")
                    glyph: panel.capture.horizontal ? "ph:arrows-left-right" : "ph:arrows-down-up"
                    Accessible.role: Accessible.Button
                    Accessible.name: panel.capture.horizontal ? qsTr("横向长截图，点一下改成竖向") : qsTr("竖向长截图，点一下改成横向")
                    onClicked: panel.capture.horizontal = !panel.capture.horizontal
                }
                FlatButton {
                    dpr: panel.dpr
                    objectName: "longCaptureSpeed"
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.speedWidth
                    implicitHeight: 28
                    flat: true; keyboardActivation: false
                    enabled: panel.capture.canAutoScroll
                    opacity: !enabled ? 0.4 : (panel.capture.autoScroll ? 1.0 : 0.72)
                    text: panel.speedWords[panel.capture.autoScrollSpeed] ?? qsTr("标准")
                    glyph: "ph:gauge"
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("自动滚动速度：%1，点一下切换").arg(text)
                    onClicked: panel.capture.cycleAutoScrollSpeed()
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("自动滚动"); color: Theme.subtext; font.pixelSize: Theme.fontCaption
                }
                ToggleSwitch {
                    dpr: panel.dpr
                    objectName: "longCaptureAutoScroll"
                    anchors.verticalCenter: parent.verticalCenter
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
                dpr: panel.dpr
                objectName: "longCapturePreviewToggle"
                text: qsTr("预览"); glyph: panel.previewVisible ? "ph:eye" : "ph:eye-slash"
                flat: !panel.previewVisible; keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.previewVisible = !panel.previewVisible
            }
            FlatButton {
                dpr: panel.dpr
                text: qsTr("诊断"); flat: true; keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: qsTr("复制诊断信息")
                onClicked: panel.capture.copyDiagnostics()
            }
            FlatButton {
                dpr: panel.dpr
                objectName: "longCapturePin"
                text: qsTr("贴图"); glyph: "ph:push-pin"; keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.capture.exportTo(LongCapture.Pin)
            }
            FlatButton {
                dpr: panel.dpr
                objectName: "longCaptureSave"
                text: qsTr("保存"); glyph: "ph:floppy-disk"; keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.capture.exportTo(LongCapture.Save)
            }
            FlatButton {
                dpr: panel.dpr
                objectName: "longCaptureDone"
                text: qsTr("完成"); keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: qsTr("完成并编辑")
                onClicked: panel.capture.finish()
            }
            FlatButton {
                dpr: panel.dpr
                objectName: "longCaptureCancel"
                text: qsTr("取消"); keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.capture.cancel()
            }
            FlatButton {
                dpr: panel.dpr
                objectName: "longCaptureCopy"
                text: qsTr("复制"); glyph: "ph:check"; highlighted: true; keyboardActivation: false
                Accessible.role: Accessible.Button; Accessible.name: text
                onClicked: panel.capture.exportTo(LongCapture.Copy)
            }
        }
    }
    WindowEdge { fill: Theme.background }

    Window {
        id: previewWindow
        objectName: "longCapturePreview"
        flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
        visible: panel.visible && panel.previewVisible
        color: Theme.background
        title: qsTr("长图预览")
        width: panel.capture.previewBox.width + 2 * panel.padding
        height: previewHeader.implicitHeight + panel.capture.previewBox.height + 3 * panel.padding
        onClosing: (event) => { event.accepted = false; panel.previewVisible = false }
        // Dragged anywhere, as the panel by its heading: off the region when it covers it (GitHub #378).
        DragHandler {
            target: null
            onActiveChanged: if (active) previewWindow.startSystemMove()
        }
        Item {
            id: previewHeader
            x: panel.padding; y: panel.padding
            width: previewWindow.width - 2 * panel.padding
            implicitHeight: Math.max(previewHeading.implicitHeight, previewMeta.implicitHeight)
            height: implicitHeight
            Text {
                id: previewHeading
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: qsTr("长图预览"); color: Theme.text; font.pixelSize: Theme.fontBody
            }
            Text {
                id: previewMeta
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                text: (panel.capture.horizontal ? qsTr("宽 %1 像素") : qsTr("高 %1 像素")).arg(panel.capture.pictureHeight)
                color: Theme.subtext; font.pixelSize: Theme.fontCaption
            }
        }
        Item {
            x: panel.padding; y: previewHeader.y + previewHeader.height + panel.padding
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
                readonly property real edge: Theme.edge(panel.dpr)
                x: shot.x - edge; y: -edge; width: shot.width + 2 * edge; height: shot.height + 2 * edge
                color: "transparent"; border.width: edge; border.color: Theme.windowEdge
            }
            Text {
                anchors.centerIn: parent
                visible: !panel.capture.hasPreview
                text: qsTr("等第一帧…"); color: Theme.subtext; font.pixelSize: Theme.fontCaption
            }
        }
        WindowEdge { fill: Theme.background }
    }
}
