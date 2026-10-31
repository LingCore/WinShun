pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import WinShun

Window {
    id: panel
    required property Recorder recorder
    readonly property bool ready: recorder.stage === Recorder.Ready
    readonly property bool completed: recorder.stage === Recorder.Done || recorder.stage === Recorder.Failed
    readonly property bool recording: recorder.stage === Recorder.Recording || recorder.stage === Recorder.Paused
    readonly property bool gif: recorder.format === Recorder.Gif
    property bool optionsShown: false
    readonly property int padding: 10
    property int maximumOptionsHeight: Math.max(80, Screen.desktopAvailableHeight - 100 - (errorViewport.visible ? errorViewport.height : 0))
    FontMetrics { id: captionMetrics; font.pixelSize: Theme.fontCaption }

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: Theme.background
    title: qsTr("区域录屏")
    width: Math.min(toolbar.implicitWidth + 2 * padding, Screen.desktopAvailableWidth - 16)
    height: body.implicitHeight + 2 * padding
    onVisibleChanged: if (!visible) optionsShown = false
    onClosing: (event) => { event.accepted = false; recorder.close() }

    Connections {
        target: panel.recorder
        function onChanged() {
            if (!panel.ready) panel.optionsShown = false
            else if (panel.visible && panel.recorder.error.length > 0) panel.optionsShown = true
        }
    }
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: panel.visible && panel.recorder.stage !== Recorder.Saving
        autoRepeat: false
        onActivated: {
            if (panel.optionsShown) {
                panel.optionsShown = false
                optionsButton.forceActiveFocus(Qt.ShortcutFocusReason)
            } else panel.recorder.close()
        }
    }
    Column {
        id: body
        x: panel.padding
        y: panel.padding
        width: panel.width - 2 * panel.padding
        spacing: 0

        RowLayout {
            id: toolbar
            width: parent.width
            height: 36
            spacing: 6

            Item {
                objectName: "recordingDragHandle"
                Layout.preferredWidth: 14
                Layout.fillHeight: true
                Glyph { anchors.centerIn: parent; glyph: "\uE76F"; size: 12; color: Theme.faint }
                DragHandler {
                    target: null
                    acceptedButtons: Qt.LeftButton
                    onActiveChanged: if (active) panel.startSystemMove()
                }
                HoverHandler { cursorShape: Qt.SizeAllCursor }
                Accessible.ignored: true
            }
            Row {
                visible: panel.ready
                spacing: 2
                RecordingButton {
                    objectName: "recordingMp4"
                    text: "MP4"
                    checkable: true
                    checked: !panel.gif
                    Accessible.role: Accessible.RadioButton
                    autoExclusive: true
                    Accessible.name: qsTr("MP4 视频")
                    onClicked: panel.recorder.format = Recorder.Mp4
                }
                RecordingButton {
                    objectName: "recordingGif"
                    text: "GIF"
                    checkable: true
                    checked: panel.gif
                    Accessible.role: Accessible.RadioButton
                    autoExclusive: true
                    Accessible.name: qsTr("GIF 动图")
                    onClicked: panel.recorder.format = Recorder.Gif
                }
            }
            Item {
                Layout.preferredWidth: panel.ready ? 168 : panel.recording || panel.completed ? 152 : 250
                Layout.minimumWidth: 60
                Layout.fillWidth: true
                Layout.fillHeight: true
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    spacing: 8
                    Rectangle {
                        visible: !panel.ready
                        anchors.verticalCenter: parent.verticalCenter
                        width: 6; height: 6; radius: 3
                        color: panel.recorder.stage === Recorder.Failed ? Theme.danger : panel.recorder.stage === Recorder.Done ? Theme.success
                             : panel.recorder.stage === Recorder.Recording ? Theme.danger : Theme.subtext
                        Accessible.ignored: true
                    }
                    Column {
                        width: parent.width - (panel.ready ? 0 : 14)
                        spacing: 2
                        Text {
                            width: parent.width
                            text: panel.ready ? panel.recorder.dimensions : panel.recording ? panel.recorder.timeText : panel.recorder.status
                            color: panel.recorder.stage === Recorder.Failed ? Theme.danger : Theme.text
                            font.pixelSize: panel.recording ? Theme.fontBody : Theme.fontCaption
                            font.features: { "tnum": 1 }
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: panel.recording || panel.completed
                            width: parent.width
                            text: panel.completed ? qsTr("%1 · %2").arg(panel.recorder.timeText).arg(panel.recorder.sizeText)
                                 : panel.recorder.stage === Recorder.Paused ? qsTr("已暂停 · %1").arg(panel.recorder.sizeText) : panel.recorder.sizeText
                            color: Theme.subtext
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                }
            }
            RecordingButton {
                objectName: "recordingSystemAudio"
                visible: panel.ready
                glyph: !panel.gif && panel.recorder.systemAudio ? "\uE767" : "\uE74F"
                enabled: !panel.gif
                checkable: true
                checked: !panel.gif && panel.recorder.systemAudio
                tip: panel.gif ? qsTr("GIF 不含音频") : qsTr("系统声音：%1").arg(panel.recorder.systemAudio ? qsTr("开") : qsTr("关"))
                tipsEnabled: panel.ready
                onClicked: panel.recorder.systemAudio = !panel.recorder.systemAudio
            }
            RecordingButton {
                objectName: "recordingMicrophone"
                visible: panel.ready
                glyph: !panel.gif && panel.recorder.microphone ? "\uE720" : "\uEC54"
                enabled: !panel.gif
                checkable: true
                checked: !panel.gif && panel.recorder.microphone
                tip: panel.gif ? qsTr("GIF 不含音频") : qsTr("麦克风：%1").arg(panel.recorder.microphone ? qsTr("开") : qsTr("关"))
                tipsEnabled: panel.ready
                onClicked: panel.recorder.microphone = !panel.recorder.microphone
            }
            RecordingButton {
                id: optionsButton
                objectName: "recordingOptions"
                visible: panel.ready
                glyph: "\uE713"
                checkable: true
                checked: panel.optionsShown
                tip: qsTr("录屏选项")
                tipsEnabled: panel.ready
                onClicked: panel.optionsShown = !panel.optionsShown
            }
            RecordingButton {
                objectName: "recordingPlay"
                visible: panel.completed && panel.recorder.fileName.length > 0
                enabled: panel.recorder.fileName.length > 0
                text: qsTr("播放")
                glyph: "\uE768"
                onClicked: panel.recorder.play()
            }
            RecordingButton {
                objectName: "recordingReveal"
                visible: panel.completed && panel.recorder.fileName.length > 0
                enabled: panel.recorder.fileName.length > 0
                glyph: "\uE838"
                tip: qsTr("打开文件夹")
                tipsEnabled: panel.completed
                onClicked: panel.recorder.reveal()
            }
            RecordingButton {
                objectName: "recordingCopy"
                visible: panel.completed && panel.recorder.fileName.length > 0
                enabled: panel.recorder.fileName.length > 0
                text: qsTr("复制文件")
                glyph: "\uE8C8"
                highlighted: true
                onClicked: panel.recorder.copy()
            }
            RecordingButton {
                objectName: "recordingPause"
                visible: panel.recording
                Layout.preferredWidth: Math.max(captionMetrics.advanceWidth(qsTr("暂停")), captionMetrics.advanceWidth(qsTr("继续"))) + 42
                text: panel.recorder.stage === Recorder.Paused ? qsTr("继续") : qsTr("暂停")
                glyph: panel.recorder.stage === Recorder.Paused ? "\uE768" : "\uE769"
                onClicked: panel.recorder.pause()
            }
            RecordingButton {
                objectName: "recordingAgain"
                visible: panel.completed
                text: panel.recorder.stage === Recorder.Done ? qsTr("再录一段") : qsTr("重新设置")
                onClicked: {
                    const failed = panel.recorder.stage === Recorder.Failed
                    panel.recorder.again()
                    if (failed) panel.optionsShown = true
                }
            }
            RecordingButton {
                objectName: "recordingStart"
                visible: !panel.completed
                enabled: panel.recorder.stage !== Recorder.Saving
                highlighted: true
                text: panel.ready ? qsTr("开始录制") : panel.recorder.stage === Recorder.Counting ? qsTr("立即开始")
                    : panel.recorder.stage === Recorder.Saving ? qsTr("保存中…") : qsTr("停止并保存")
                glyph: panel.ready || panel.recorder.stage === Recorder.Counting ? "\uEA3A" : "\uE71A"
                tip: qsTr("再次按截图快捷键也可停止；控制条不会录入视频。")
                tipsEnabled: panel.ready
                onClicked: {
                    if (panel.ready) panel.recorder.start()
                    else if (panel.recorder.stage === Recorder.Counting) panel.recorder.skipCountdown()
                    else panel.recorder.finish()
                }
            }
            RecordingButton {
                objectName: "recordingClose"
                visible: panel.ready || panel.completed || panel.recorder.stage === Recorder.Counting
                glyph: "\uE711"
                tip: panel.completed ? qsTr("关闭") : qsTr("取消")
                tipsEnabled: panel.ready || panel.completed
                onClicked: panel.recorder.close()
            }
        }
        Loader {
            id: optionsLoader
            objectName: "recordingOptionsLoader"
            active: panel.ready && panel.optionsShown
            width: parent.width
            height: active && status === Loader.Ready && item ? Math.min(item.implicitHeight, panel.maximumOptionsHeight) : 0
            sourceComponent: Flickable {
                id: optionsViewport
                objectName: "recordingOptionsViewport"
                readonly property int sectionGap: 8
                readonly property real separatorHeight: Theme.edge(Screen.devicePixelRatio)
                implicitHeight: options.y + options.implicitHeight
                contentWidth: width
                contentHeight: implicitHeight
                clip: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                Connections {
                    target: panel
                    function onActiveFocusItemChanged() {
                        const item = panel.activeFocusItem
                        if (!item) return
                        let ancestor = item.parent
                        while (ancestor && ancestor !== options) ancestor = ancestor.parent
                        if (!ancestor) return
                        const top = item.mapToItem(optionsViewport.contentItem, 0, 0).y
                        if (top < optionsViewport.contentY) optionsViewport.contentY = top
                        else if (top + item.height > optionsViewport.contentY + optionsViewport.height)
                            optionsViewport.contentY = Math.min(optionsViewport.contentHeight - optionsViewport.height,
                                                                top + item.height - optionsViewport.height)
                    }
                }
                Rectangle {
                    objectName: "recordingOptionsDivider"
                    y: optionsViewport.sectionGap
                    width: parent.width
                    height: optionsViewport.separatorHeight
                    color: Theme.divider
                }
                RecordingOptions {
                    id: options
                    objectName: "recordingOptionsContent"
                    y: 2 * optionsViewport.sectionGap + optionsViewport.separatorHeight
                    width: parent.width - (optionsViewport.clip ? 12 : 0)
                    recorder: panel.recorder
                }
            }
        }
        Text {
            visible: panel.completed && panel.recorder.fileName.length > 0
            topPadding: 10
            width: parent.width
            text: panel.recorder.fileName
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideMiddle
        }
        Text {
            visible: panel.recording && panel.recorder.keys === Recorder.KeysAll
            topPadding: 8
            width: parent.width
            text: qsTr("正在显示全部按键；输入密码前请暂停。")
            color: Theme.caution
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Flickable {
            id: errorViewport
            visible: panel.recorder.error.length > 0
            width: parent.width
            height: visible ? Math.min(errorText.implicitHeight + 12, 160, Screen.desktopAvailableHeight / 3) : 0
            contentWidth: width
            contentHeight: errorText.implicitHeight + 12
            clip: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            TextEdit {
                id: errorText
                y: 12
                width: parent.width - 12
                text: panel.recorder.error
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
            }
        }
    }
    WindowEdge {}
}
