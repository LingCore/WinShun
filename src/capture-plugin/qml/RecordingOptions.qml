pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

// What a recording is made with, under the panel's bar: two columns of
// choices over the panel's whole width (the left one from its left edge, the
// right one's widest row ending at its right edge), then where it is saved.
Item {
    id: options
    required property Recorder recorder
    readonly property bool gif: recorder.format === Recorder.Gif
    // The words before the controls, all as wide as the longest: the controls
    // line up in two columns, and in one.
    readonly property real labelWidth: Math.ceil(Math.max(qualityLabel.implicitWidth, pointerLabel.implicitWidth, folderLabel.implicitWidth,
                                                          rateLabel.implicitWidth, keysLabel.implicitWidth))
    // One column where the panel is too narrow for two.
    readonly property bool wide: Math.max(qualityRow.implicitWidth, pointerRow.implicitWidth) + grid.columnSpacing
                                 + Math.max(rateRow.implicitWidth, keysRow.implicitWidth) <= width
    implicitHeight: content.implicitHeight

    ColumnLayout {
        id: content
        width: options.width
        spacing: 8

        GridLayout {
            id: grid
            Layout.fillWidth: true
            columns: options.wide ? 2 : 1
            columnSpacing: 16
            rowSpacing: 8

            RowLayout {
                id: qualityRow
                Layout.fillWidth: true
                spacing: 8
                Caption { id: qualityLabel; text: qsTr("画质"); Layout.preferredWidth: options.labelWidth }
                ChoiceGroup {
                    spacing: 2
                    ChoiceButton {
                        objectName: "recordingSmallFile"
                        text: qsTr("小体积")
                        checked: options.recorder.quality === Recorder.SmallFile
                        tip: options.gif ? qsTr("10 帧/秒，最长边 960 像素，适合简单操作。")
                                         : qsTr("文件最小，细节略有损失，适合简单操作。")
                        onClicked: options.recorder.quality = Recorder.SmallFile
                    }
                    ChoiceButton {
                        objectName: "recordingStandard"
                        text: qsTr("标准")
                        checked: options.recorder.quality === Recorder.Standard
                        tip: options.gif ? qsTr("15 帧/秒，最长边 1280 像素，兼顾清晰度和体积。")
                                         : qsTr("兼顾清晰度和文件大小。")
                        onClicked: options.recorder.quality = Recorder.Standard
                    }
                    ChoiceButton {
                        objectName: "recordingHighQuality"
                        text: qsTr("高清")
                        checked: options.recorder.quality === Recorder.HighQuality
                        tip: options.gif ? qsTr("20 帧/秒，最长边 1920 像素，文件可能较大。")
                                         : qsTr("细节更锐利，文件更大。")
                        onClicked: options.recorder.quality = Recorder.HighQuality
                    }
                    ChoiceButton {
                        objectName: "recordingUltraQuality"
                        text: qsTr("超清")
                        checked: options.recorder.quality === Recorder.UltraQuality
                        tip: options.gif ? qsTr("20 帧/秒，保持原始尺寸，文件最大。")
                                         : qsTr("最清晰，文件也最大。选区不超过 1920 × 1080 时按 2 倍尺寸保存，彩色小字和屏幕上一样清楚。")
                        onClicked: options.recorder.quality = Recorder.UltraQuality
                    }
                }
                Item { Layout.fillWidth: true }
            }
            RowLayout {
                id: rateRow
                spacing: 8
                Caption { id: rateLabel; text: qsTr("帧率"); Layout.preferredWidth: options.labelWidth }
                ChoiceGroup {
                    visible: !options.gif
                    spacing: 2
                    ChoiceButton {
                        objectName: "recordingFps15"
                        text: qsTr("15 帧")
                        checked: options.recorder.frameRate === 15
                        tip: qsTr("每秒 15 帧，文件最小，适合文字和简单操作。")
                        onClicked: options.recorder.frameRate = 15
                    }
                    ChoiceButton {
                        objectName: "recordingFps30"
                        text: qsTr("30 帧")
                        checked: options.recorder.frameRate === 30
                        tip: qsTr("每秒 30 帧，流畅，适合大多数情况。")
                        onClicked: options.recorder.frameRate = 30
                    }
                    ChoiceButton {
                        objectName: "recordingFps60"
                        text: qsTr("60 帧")
                        checked: options.recorder.frameRate === 60
                        tip: qsTr("每秒 60 帧，最顺滑，适合游戏、动画和滚动；文件更大，也更吃电脑性能。")
                        onClicked: options.recorder.frameRate = 60
                    }
                }
                Text { // a GIF's rate goes with its quality
                    objectName: "recordingGifRate"
                    visible: options.gif
                    Layout.preferredHeight: 32
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("%1 帧/秒，随画质").arg(options.recorder.savedFrameRate)
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
            }
            RowLayout {
                id: pointerRow
                Layout.fillWidth: true
                spacing: 8
                Caption { id: pointerLabel; text: qsTr("鼠标"); Layout.preferredWidth: options.labelWidth }
                Row {
                    spacing: 4
                    OptionToggle {
                        objectName: "recordingCursor"
                        glyph: "\uE7C9"
                        Accessible.name: qsTr("鼠标指针")
                        checked: options.recorder.cursor
                        onClicked: options.recorder.cursor = !options.recorder.cursor
                    }
                    OptionToggle {
                        objectName: "recordingClicks"
                        glyph: "\uE8B0"
                        Accessible.name: qsTr("点击高亮")
                        checked: options.recorder.clickHighlights
                        onClicked: options.recorder.clickHighlights = !options.recorder.clickHighlights
                    }
                    OptionToggle {
                        objectName: "recordingTrail"
                        iconName: "path"
                        Accessible.name: qsTr("鼠标拖尾")
                        // Only with the pointer: off while it is, and turning it on records the pointer too.
                        checked: options.recorder.cursor && options.recorder.cursorTrail
                        onClicked: {
                            const on = !(options.recorder.cursor && options.recorder.cursorTrail)
                            if (on)
                                options.recorder.cursor = true
                            options.recorder.cursorTrail = on
                        }
                    }
                    OptionToggle {
                        objectName: "recordingMouseButtons"
                        glyph: "\uE962"
                        Accessible.name: qsTr("鼠标按键")
                        checked: options.recorder.mouseButtons
                        onClicked: options.recorder.mouseButtons = !options.recorder.mouseButtons
                    }
                }
                Hook { implicitWidth: Theme.edge(Screen.devicePixelRatio); implicitHeight: 18; part: Theme.separator; orientation: Qt.Vertical; dpr: Screen.devicePixelRatio }
                OptionToggle {
                    objectName: "recordingCountdown"
                    glyph: "\uE916"
                    Accessible.name: qsTr("3 秒倒计时")
                    checked: options.recorder.countdown
                    onClicked: options.recorder.countdown = !options.recorder.countdown
                }
                Item { Layout.fillWidth: true }
            }
            RowLayout {
                id: keysRow
                spacing: 8
                Caption { id: keysLabel; text: qsTr("按键"); Layout.preferredWidth: options.labelWidth; Accessible.name: qsTr("显示按键") }
                ChoiceGroup {
                    spacing: 2
                    ChoiceButton {
                        objectName: "recordingKeysOff"
                        text: qsTr("不显示")
                        checked: options.recorder.keys === Recorder.KeysOff
                        onClicked: options.recorder.keys = Recorder.KeysOff
                    }
                    ChoiceButton {
                        objectName: "recordingKeysShortcuts"
                        text: qsTr("仅快捷键")
                        checked: options.recorder.keys === Recorder.KeysShortcuts
                        tip: qsTr("“仅快捷键”显示组合键和功能键，不显示输入的文字。")
                        onClicked: options.recorder.keys = Recorder.KeysShortcuts
                    }
                    ChoiceButton {
                        objectName: "recordingKeysAll"
                        text: qsTr("全部按键")
                        checked: options.recorder.keys === Recorder.KeysAll
                        tip: qsTr("全部按键会录下输入的文字和密码，输入密码前请暂停。")
                        onClicked: options.recorder.keys = Recorder.KeysAll
                    }
                }
            }
        }
        Text {
            visible: options.recorder.keys === Recorder.KeysAll
            Layout.fillWidth: true
            text: qsTr("全部按键会录下输入的文字和密码，输入密码前请暂停。")
            color: Theme.caution
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Caption { id: folderLabel; text: qsTr("保存到"); Layout.preferredWidth: options.labelWidth }
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: options.recorder.folder
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideMiddle
                Accessible.name: text
            }
            RecordingButton {
                objectName: "recordingChooseFolder"
                implicitHeight: 32
                glyph: "\uE70F"
                Accessible.name: qsTr("更改保存文件夹")
                Accessible.description: options.recorder.folder
                tip: qsTr("更改保存文件夹") + "\n" + options.recorder.folder
                tipsEnabled: true
                onClicked: options.recorder.chooseFolder()
            }
        }
    }
    // The words before a row's controls.
    component Caption: Text {
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
    }
    // A few choices side by side, one of them on (radio buttons): the style's
    // segmented part is drawn behind them, the buttons (RecordingButton, checkable,
    // autoExclusive) only draw their words. As in RecordingPanel.
    component ChoiceGroup: Item {
        id: choices

        default property alias content: strip.data
        property alias spacing: strip.spacing
        readonly property bool isChoiceGroup: true
        readonly property Item hook: track
        // The buttons that take room now, in order.
        readonly property var items: {
            const list = []
            const kids = strip.children
            for (let i = 0; i < kids.length; ++i) {
                if (kids[i].visible && kids[i].width > 0)
                    list.push(kids[i])
            }
            return list
        }

        function indexOf(item: Item): int { return items.indexOf(item) }

        implicitWidth: strip.implicitWidth
        implicitHeight: strip.implicitHeight

        Hook {
            id: track
            anchors.fill: parent
            part: Theme.segmented
            count: choices.items.length
            currentIndex: choices.items.findIndex(item => item.checked)
            hoveredIndex: choices.items.findIndex(item => item.hovered)
            pressedIndex: choices.items.findIndex(item => item.down)
            segments: choices.items.map(item => ({ x: item.x, width: item.width }))
            role: "switch"
            dpr: Screen.devicePixelRatio
        }
        Row {
            id: strip
            spacing: 2
        }
    }
    component ChoiceButton: RecordingButton {
        implicitHeight: 32
        checkable: true
        Accessible.role: Accessible.RadioButton
        Accessible.name: text
        Accessible.description: tip
        autoExclusive: true
        tipsEnabled: true
    }
    component OptionToggle: RecordingButton {
        id: toggle
        implicitHeight: 32
        checkable: true
        Accessible.role: Accessible.CheckBox
        tip: qsTr("%1：%2").arg(Accessible.name).arg(checked ? qsTr("开") : qsTr("关"))
        tipsEnabled: true
        Glyph {
            visible: toggle.checked
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 2
            glyph: "\uE73E"
            size: 10
            color: toggle.foreground
            Accessible.ignored: true
        }
    }
}
