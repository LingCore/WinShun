pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

Item {
    id: options
    required property Recorder recorder
    readonly property bool gif: recorder.format === Recorder.Gif
    implicitHeight: content.implicitHeight

    ColumnLayout {
        id: content
        width: options.width
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text { text: qsTr("画质"); color: Theme.subtext; font.pixelSize: Theme.fontCaption; Layout.preferredWidth: 38 }
            ChoiceGroup {
                spacing: 2
                ChoiceButton {
                    objectName: "recordingSmallFile"
                    text: qsTr("小体积")
                    checked: options.recorder.quality === Recorder.SmallFile
                    tip: options.gif ? qsTr("10 帧/秒，最长边 960 像素，适合简单操作。")
                                     : qsTr("15 帧/秒，保留原始尺寸，适合文字和简单操作。")
                    onClicked: options.recorder.quality = Recorder.SmallFile
                }
                ChoiceButton {
                    objectName: "recordingStandard"
                    text: qsTr("标准画质")
                    checked: options.recorder.quality === Recorder.Standard
                    tip: options.gif ? qsTr("15 帧/秒，最长边 1280 像素，兼顾清晰度和体积。")
                                     : qsTr("30 帧/秒，兼顾清晰度和文件大小。")
                    onClicked: options.recorder.quality = Recorder.Standard
                }
                ChoiceButton {
                    objectName: "recordingHighQuality"
                    text: qsTr("更清晰")
                    checked: options.recorder.quality === Recorder.HighQuality
                    tip: options.gif ? qsTr("20 帧/秒，最长边 1920 像素，文件可能较大。")
                                     : qsTr("30 帧/秒，优先保留细节，文件可能更大。")
                    onClicked: options.recorder.quality = Recorder.HighQuality
                }
            }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
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
                    objectName: "recordingMouseButtons"
                    glyph: "\uE962"
                    Accessible.name: qsTr("鼠标按键")
                    checked: options.recorder.mouseButtons
                    onClicked: options.recorder.mouseButtons = !options.recorder.mouseButtons
                }
                OptionToggle {
                    objectName: "recordingCountdown"
                    glyph: "\uE916"
                    Accessible.name: qsTr("3 秒倒计时")
                    checked: options.recorder.countdown
                    onClicked: options.recorder.countdown = !options.recorder.countdown
                }
            }
            Hook { implicitWidth: Theme.edge(Screen.devicePixelRatio); implicitHeight: 18; part: Theme.separator; orientation: Qt.Vertical; dpr: Screen.devicePixelRatio }
            Glyph { glyph: "\uE765"; size: 16; Accessible.name: qsTr("显示按键") }
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
            Item { Layout.fillWidth: true }
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
            Glyph { glyph: "\uE8B7"; size: 16; Accessible.name: qsTr("保存到") }
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: options.recorder.folder
                color: Theme.subtext
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
            size: 8
            color: toggle.foreground
            Accessible.ignored: true
        }
    }
}
