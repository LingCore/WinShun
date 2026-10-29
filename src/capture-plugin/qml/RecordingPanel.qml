pragma ComponentBehavior: Bound
import QtQuick
import WinShun

Window {
    id: panel
    required property Recorder recorder
    readonly property bool ready: recorder.stage === Recorder.Ready
    readonly property bool completed: recorder.stage === Recorder.Done || recorder.stage === Recorder.Failed
    readonly property bool recording: recorder.stage === Recorder.Recording || recorder.stage === Recorder.Paused
    readonly property bool compact: !ready && !completed
    readonly property bool gif: recorder.format === Recorder.Gif
    readonly property int padding: compact ? 12 : 16

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: Theme.background
    title: qsTr("区域录屏")
    width: 360
    height: content.implicitHeight + 2 * padding
    onClosing: (event) => { event.accepted = false; recorder.close() }

    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: panel.visible && panel.recorder.stage !== Recorder.Saving
        autoRepeat: false
        onActivated: panel.recorder.close()
    }
    Column {
        id: content
        x: panel.padding
        y: panel.padding
        width: panel.width - 2 * panel.padding
        spacing: panel.compact ? 8 : 12

        Item {
            width: parent.width
            height: Math.max(statusText.implicitHeight, timerText.implicitHeight)
            Rectangle {
                id: indicator
                visible: panel.compact
                width: 6
                height: 6
                anchors.verticalCenter: parent.verticalCenter
                radius: 3
                color: panel.recorder.stage === Recorder.Recording ? Theme.danger : Theme.subtext
                Accessible.ignored: true
            }
            Text {
                id: statusText
                anchors.left: panel.compact ? indicator.right : parent.left
                anchors.leftMargin: panel.compact ? 8 : 0
                anchors.right: timerText.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: panel.recorder.status
                color: panel.recorder.stage === Recorder.Failed ? Theme.danger : Theme.text
                font.pixelSize: panel.compact ? Theme.fontBody : Theme.uiTitle
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                id: timerText
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: panel.compact ? qsTr("%1 · %2").arg(panel.recorder.timeText).arg(panel.recorder.sizeText) : ""
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                onActiveChanged: if (active) panel.startSystemMove()
            }
        }
        Text {
            visible: !panel.compact
            width: parent.width
            text: panel.recorder.dimensions
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }

        Column {
            width: parent.width
            spacing: 12
            visible: panel.ready

            Row {
                spacing: 8
                RecordingButton {
                    objectName: "recordingMp4"
                    option: true
                    exclusive: true
                    text: qsTr("MP4 视频")
                    highlighted: !panel.gif
                    onClicked: panel.recorder.format = Recorder.Mp4
                }
                RecordingButton {
                    objectName: "recordingGif"
                    option: true
                    exclusive: true
                    text: qsTr("GIF 动图")
                    highlighted: panel.gif
                    onClicked: panel.recorder.format = Recorder.Gif
                }
            }
            Flow {
                width: parent.width
                spacing: 8
                visible: !panel.gif
                RecordingButton {
                    objectName: "recordingSystemAudio"
                    option: true
                    text: qsTr("系统声音：%1").arg(panel.recorder.systemAudio ? qsTr("开") : qsTr("关"))
                    highlighted: panel.recorder.systemAudio
                    onClicked: panel.recorder.systemAudio = !panel.recorder.systemAudio
                }
                RecordingButton {
                    objectName: "recordingMicrophone"
                    option: true
                    text: qsTr("麦克风：%1").arg(panel.recorder.microphone ? qsTr("开") : qsTr("关"))
                    highlighted: panel.recorder.microphone
                    onClicked: panel.recorder.microphone = !panel.recorder.microphone
                }
            }
            Flow {
                width: parent.width
                spacing: 8
                RecordingButton {
                    objectName: "recordingSmallFile"
                    option: true
                    exclusive: true
                    text: qsTr("小体积")
                    highlighted: panel.recorder.quality === Recorder.SmallFile
                    onClicked: panel.recorder.quality = Recorder.SmallFile
                }
                RecordingButton {
                    objectName: "recordingStandard"
                    option: true
                    exclusive: true
                    text: qsTr("标准画质")
                    highlighted: panel.recorder.quality === Recorder.Standard
                    onClicked: panel.recorder.quality = Recorder.Standard
                }
                RecordingButton {
                    objectName: "recordingHighQuality"
                    option: true
                    exclusive: true
                    text: qsTr("更清晰")
                    highlighted: panel.recorder.quality === Recorder.HighQuality
                    onClicked: panel.recorder.quality = Recorder.HighQuality
                }
            }
            Text {
                width: parent.width
                text: panel.gif ? (panel.recorder.quality === Recorder.SmallFile ? qsTr("10 帧/秒，最长边 960 像素，适合简单操作。")
                    : panel.recorder.quality === Recorder.HighQuality ? qsTr("20 帧/秒，最长边 1920 像素，文件可能较大。")
                    : qsTr("15 帧/秒，最长边 1280 像素，兼顾清晰度和体积。"))
                    : panel.recorder.quality === Recorder.SmallFile ? qsTr("15 帧/秒，保留原始尺寸，适合文字和简单操作。")
                    : panel.recorder.quality === Recorder.HighQuality ? qsTr("30 帧/秒，优先保留细节，文件可能更大。")
                    : qsTr("30 帧/秒，兼顾清晰度和文件大小。")
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Flow {
                width: parent.width
                spacing: 8
                RecordingButton {
                    objectName: "recordingCursor"
                    option: true
                    text: qsTr("鼠标指针：%1").arg(panel.recorder.cursor ? qsTr("开") : qsTr("关"))
                    highlighted: panel.recorder.cursor
                    onClicked: panel.recorder.cursor = !panel.recorder.cursor
                }
                RecordingButton {
                    objectName: "recordingCountdown"
                    option: true
                    text: qsTr("3 秒倒计时：%1").arg(panel.recorder.countdown ? qsTr("开") : qsTr("关"))
                    highlighted: panel.recorder.countdown
                    onClicked: panel.recorder.countdown = !panel.recorder.countdown
                }
                RecordingButton {
                    objectName: "recordingClicks"
                    option: true
                    text: qsTr("点击高亮：%1").arg(panel.recorder.clickHighlights ? qsTr("开") : qsTr("关"))
                    highlighted: panel.recorder.clickHighlights
                    Accessible.description: qsTr("在视频中显示鼠标点击圈，暂停期间和控制条上的点击不显示。")
                    onClicked: panel.recorder.clickHighlights = !panel.recorder.clickHighlights
                }
                RecordingButton {
                    objectName: "recordingMouseButtons"
                    option: true
                    text: qsTr("鼠标按键：%1").arg(panel.recorder.mouseButtons ? qsTr("开") : qsTr("关"))
                    highlighted: panel.recorder.mouseButtons
                    Accessible.description: qsTr("在视频角落显示按下的是左键、右键、中键还是侧键。")
                    onClicked: panel.recorder.mouseButtons = !panel.recorder.mouseButtons
                }
            }
            Flow {
                width: parent.width
                spacing: 8
                RecordingButton {
                    objectName: "recordingKeysOff"
                    option: true
                    exclusive: true
                    text: qsTr("不显示按键")
                    highlighted: panel.recorder.keys === Recorder.KeysOff
                    onClicked: panel.recorder.keys = Recorder.KeysOff
                }
                RecordingButton {
                    objectName: "recordingKeysShortcuts"
                    option: true
                    exclusive: true
                    text: qsTr("仅快捷键")
                    highlighted: panel.recorder.keys === Recorder.KeysShortcuts
                    onClicked: panel.recorder.keys = Recorder.KeysShortcuts
                }
                RecordingButton {
                    objectName: "recordingKeysAll"
                    option: true
                    exclusive: true
                    text: qsTr("全部按键")
                    highlighted: panel.recorder.keys === Recorder.KeysAll
                    onClicked: panel.recorder.keys = Recorder.KeysAll
                }
            }
            Text {
                width: parent.width
                text: panel.recorder.keys === Recorder.KeysAll ? qsTr("打的字也会录进视频，包括密码；输入密码前请先暂停。")
                    : panel.recorder.keys === Recorder.KeysShortcuts ? qsTr("在视频角落显示 Ctrl、Alt、Win 组合键和 Esc、Enter、F1 等功能键，打的字不显示。")
                    : qsTr("可在视频角落显示按下的键；推荐“仅快捷键”，不会录下打的字。")
                color: panel.recorder.keys === Recorder.KeysAll ? Theme.caution : Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: panel.recorder.folder
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 3
                elide: Text.ElideMiddle
            }
            RecordingButton {
                objectName: "recordingChooseFolder"
                flat: true
                text: qsTr("更改保存文件夹…")
                onClicked: panel.recorder.chooseFolder()
            }
        }

        Text {
            visible: panel.completed
            width: parent.width
            text: qsTr("%1 · %2").arg(panel.recorder.timeText).arg(panel.recorder.sizeText)
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        Text {
            visible: panel.compact
            width: parent.width
            text: panel.gif ? qsTr("GIF 动图 · 无音频") : qsTr("系统声音：%1 · 麦克风：%2").arg(panel.recorder.systemAudio ? qsTr("开") : qsTr("关"))
                .arg(panel.recorder.microphone ? qsTr("开") : qsTr("关"))
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Text {
            visible: panel.recording && panel.recorder.keys === Recorder.KeysAll
            width: parent.width
            text: qsTr("正在显示全部按键：打的字会录进视频，输入密码前请先暂停。")
            color: Theme.caution
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Text {
            visible: panel.recorder.error.length > 0
            width: parent.width
            text: panel.recorder.error
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WrapAnywhere
        }
        Text {
            visible: panel.completed && panel.recorder.fileName.length > 0
            width: parent.width
            text: panel.recorder.fileName
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WrapAnywhere
        }

        Flow {
            width: parent.width
            spacing: 8
            visible: panel.completed
            RecordingButton { text: qsTr("播放"); enabled: panel.recorder.fileName.length > 0; onClicked: panel.recorder.play() }
            RecordingButton { text: qsTr("复制文件"); enabled: panel.recorder.fileName.length > 0; onClicked: panel.recorder.copy() }
            RecordingButton { text: qsTr("打开文件夹"); enabled: panel.recorder.fileName.length > 0; onClicked: panel.recorder.reveal() }
        }

        Row {
            spacing: 8
            RecordingButton {
                objectName: "recordingClose"
                visible: panel.ready || panel.completed || panel.recorder.stage === Recorder.Counting
                text: panel.completed ? qsTr("关闭") : qsTr("取消")
                onClicked: panel.recorder.close()
            }
            RecordingButton {
                objectName: "recordingPause"
                visible: panel.recording
                text: panel.recorder.stage === Recorder.Paused ? qsTr("继续") : qsTr("暂停")
                onClicked: panel.recorder.pause()
            }
            RecordingButton {
                objectName: "recordingAgain"
                visible: panel.completed
                highlighted: true
                text: panel.recorder.stage === Recorder.Done ? qsTr("再录一段") : qsTr("重新设置")
                onClicked: panel.recorder.again()
            }
            RecordingButton {
                objectName: "recordingStart"
                visible: !panel.completed
                enabled: panel.recorder.stage !== Recorder.Saving
                highlighted: true
                text: panel.ready ? qsTr("开始录制") : panel.recorder.stage === Recorder.Counting ? qsTr("立即开始")
                    : panel.recorder.stage === Recorder.Saving ? qsTr("正在保存…") : qsTr("停止并保存")
                onClicked: {
                    if (panel.ready) panel.recorder.start()
                    else if (panel.recorder.stage === Recorder.Counting) panel.recorder.skipCountdown()
                    else panel.recorder.finish()
                }
            }
        }
        Text {
            visible: !panel.compact
            width: parent.width
            text: panel.ready ? (panel.gif ? qsTr("GIF 自动循环播放，不含音频；停止后会显示生成进度。") : qsTr("再次按截图快捷键也可停止；控制条不会录入视频。"))
                             : panel.recorder.stage === Recorder.Done ? qsTr("文件已保留在本机。")
                             : panel.recorder.stage === Recorder.Failed ? qsTr("可重新设置声音和保存位置后再录。")
                             : ""
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }
    component RecordingButton: FlatButton {
        id: button
        property bool option: false
        property bool exclusive: false
        Accessible.role: exclusive ? Accessible.RadioButton : option ? Accessible.CheckBox : Accessible.Button
        Accessible.name: button.text
        Accessible.checkable: option
        Accessible.checked: option && button.highlighted
        Accessible.onPressAction: if (button.enabled) button.clicked()
    }
    WindowEdge {}
}
