pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The card's own options, here too: the trail, full-screen programs, pausing,
// the programs left alone; and putting the gestures back as they came.
// The column its sections read (see SettingsSection.qml).
Column {
    id: page

    required property GestureEditor editor
    required property ProgramPicker programPicker
    property real dpr: Screen.devicePixelRatio
    property bool confirming: false // 恢复默认 asked once

    readonly property int currentPage: -1
    readonly property int currentTab: 0

    width: parent ? parent.width : 0
    spacing: 20

    SettingsSection {
        width: parent.width
        title: qsTr("手势")
        glyph: "ph:sliders-horizontal"

        SettingRow {
            title: qsTr("画出轨迹和将要做的事")
            description: qsTr("画的时候显示轨迹和它要做什么；也管“已复制”这类小提示")

            ToggleSwitch {
                dpr: page.dpr
                checked: page.editor.trail
                onToggled: (on) => page.editor.trail = on
            }
        }
        SettingRow {
            title: qsTr("全屏的程序里也用手势")
            description: qsTr("关着时，游戏、放幻灯片、全屏看视频时右键和平常一样，角落和屏幕边也不触发")

            ToggleSwitch {
                dpr: page.dpr
                checked: page.editor.fullScreen
                onToggled: (on) => page.editor.fullScreen = on
            }
        }
        SettingRow {
            title: qsTr("暂停鼠标手势")
            description: qsTr("暂停后右键、角落和屏幕边都和平常一样；在 Win顺 里输入“手势”也能暂停和恢复")

            ToggleSwitch {
                dpr: page.dpr
                checked: page.editor.paused
                onToggled: (on) => page.editor.paused = on
            }
        }
    }

    SettingsSection {
        width: parent.width
        title: qsTr("不用手势的程序")
        glyph: "ph:prohibit"

        SettingRow {
            id: leftAlone

            title: qsTr("这些程序里不用手势")
            description: qsTr("这些程序里右键和平常一样，角落和屏幕边也不触发")

            FlatButton {
                id: addExcluded
                objectName: "addExcludedButton"
                dpr: page.dpr
                glyph: "ph:plus"
                text: qsTr("添加程序")
                onClicked: page.programPicker.ask(addExcluded, page.editor.excluded, (name) => page.editor.addExcluded(name))
            }

            body: Flow {
                width: parent.width
                spacing: 8

                Repeater {
                    model: page.editor.excluded

                    delegate: Chip {
                        required property string modelData

                        text: modelData
                        onRemoveClicked: page.editor.removeExcluded(modelData)
                    }
                }
                Text {
                    visible: page.editor.excluded.length === 0
                    height: 32
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("还没有")
                    color: Theme.faint
                    font.pixelSize: Theme.fontCaption
                }
            }
        }
    }

    SettingsSection {
        width: parent.width
        title: qsTr("恢复")
        glyph: "ph:arrow-counter-clockwise"

        SettingRow {
            title: qsTr("恢复默认手势")
            description: page.confirming ? qsTr("所有手势、角落、屏幕边和滚轮都会改回刚装好时的样子，你加的会被清掉。确定吗？")
                                         : qsTr("把所有手势、角落、屏幕边和滚轮改回刚装好时的样子")

            FlatButton {
                visible: !page.confirming
                dpr: page.dpr
                text: qsTr("恢复默认…")
                onClicked: page.confirming = true
            }
            FlatButton {
                visible: page.confirming
                dpr: page.dpr
                text: qsTr("取消")
                onClicked: page.confirming = false
            }
            FlatButton {
                visible: page.confirming
                dpr: page.dpr
                highlighted: true
                text: qsTr("恢复默认")
                onClicked: {
                    page.editor.restoreDefaults()
                    page.confirming = false
                }
            }
        }
    }
}
