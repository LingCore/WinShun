pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → 插件 › 开发者, in developer mode (the plugin design, §5.5; its
// switch is on the 已安装 tab, PluginsSection, which `lockedRows` are given
// to): a new plugin from a template, and the log of what goes to and from
// the plugins (the last 500 lines, by plugin, each unfolding to the whole
// message). Scripts are reloaded as they change.
SettingsSection {
    id: devSection

    required property PluginSettings plugins

    readonly property var templateKinds: ["powershell", "python", "python-resident", "cpp", "zero"]

    // Shown only in developer mode: for the switch's search hint.
    readonly property var lockedRows: [newRow, logRow]

    // The settings window hidden (SettingsWindow.resetForReopen): as a window made anew.
    function resetForReopen() {
        newRow.problem = ""
        nameBox.clear()
        kindTabs.current = 0
    }

    note: qsTr("每一条发给插件和插件回来的消息都记在这里；改了脚本，常驻插件会自动重新加载。")

    SettingRow {
        id: newRow

        property string problem

        title: qsTr("新建插件")
        FlatButton {
            text: qsTr("复制 AI 开发提示")
            glyph: "\uE8C8"
            onClicked: devSection.plugins.copyDevelopmentPrompt()
        }
        keywords: qsTr("模板;新建;创建;PowerShell;Python;零代码;template;new").split(";")
        description: qsTr("按模板在插件文件夹里建一个，SDK 文件一起放进去：PowerShell、Python、C++ 的常驻插件，Python 的一次性插件，或只有 plugin.json 的零代码")
        shown: devSection.plugins.developerMode

        body: [
            ScopeTabs { // the language and kind, on a line of its own: five of them do not fit beside the name
                id: kindTabs
                labels: [qsTr("PowerShell 常驻"), qsTr("Python 一次性"), qsTr("Python 常驻"), qsTr("C++ 常驻"), qsTr("零代码")]
                current: 0
                onActivated: (index) => current = index
            },
            Row {
                spacing: 8

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: devSection.plugins.templateAuthor + "."
                    color: Theme.subtext
                    font.pixelSize: Theme.fontBody
                }
                InputBox {
                    id: nameBox
                    width: 180
                    implicitHeight: 32
                    placeholder: qsTr("名字，如 hello")
                    onAccepted: if (createButton.enabled) createButton.clicked()
                }
                FlatButton {
                    id: createButton
                    implicitHeight: 32
                    text: qsTr("创建")
                    highlighted: true
                    enabled: nameBox.text.trim().length > 0
                    onClicked: {
                        newRow.problem = devSection.plugins.createFromTemplate(devSection.templateKinds[kindTabs.current], nameBox.text)
                        if (newRow.problem.length === 0)
                            nameBox.clear()
                    }
                }
            },
            Text {
                visible: newRow.problem.length > 0
                width: parent.width
                text: newRow.problem
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        ]
    }

    SettingRow {
        id: logRow

        readonly property var filterChoices: [{ id: "", name: qsTr("全部") }].concat(devSection.plugins.logPlugins)

        title: qsTr("消息记录")
        keywords: qsTr("日志;记录;协议;JSON;log;trace").split(";")
        description: logList.count > 0
                     ? qsTr("最近 %n 条；→ 发给插件，← 插件回的，· 事件；点一条看整个消息", "", logList.count)
                     : qsTr("还没有消息：开着开发者模式，在搜索框里用一下插件")
        shown: devSection.plugins.developerMode

        FlatButton {
            text: qsTr("清空")
            flat: true
            enabled: logList.count > 0
            onClicked: devSection.plugins.clearLog()
        }
        FlatButton {
            text: qsTr("打开 plugins.log")
            glyph: "" // Document
            onClicked: devSection.plugins.openLog()
        }

        body: [
            ScopeTabs { // which plugin's lines
                visible: logRow.filterChoices.length > 2
                labels: logRow.filterChoices.map(c => c.name)
                current: Math.max(0, logRow.filterChoices.findIndex(c => c.id === devSection.plugins.logFilter))
                onActivated: (index) => devSection.plugins.logFilter = logRow.filterChoices[index].id
            },
            Rectangle {
                width: parent.width
                height: Math.max(40, Math.min(logList.contentHeight + 2 * Theme.edge(Screen.devicePixelRatio), 360))
                radius: Theme.controlRadius
                color: "transparent"
                border.width: 1
                border.color: Theme.cardBorder
                clip: true

                Text {
                    visible: logList.count === 0
                    anchors.centerIn: parent
                    text: qsTr("没有消息")
                    color: Theme.faint
                    font.pixelSize: Theme.fontCaption
                }

                ListView {
                    id: logList
                    anchors.fill: parent
                    anchors.margins: Theme.edge(Screen.devicePixelRatio) // inside the border
                    model: devSection.plugins.log
                    boundsBehavior: Flickable.StopAtBounds
                    onCountChanged: positionViewAtEnd()

                    delegate: Item {
                        id: logLine

                        required property int index
                        required property string time
                        required property string plugin
                        required property string direction
                        required property string summary
                        required property string detail
                        required property int elapsed
                        property bool open: false

                        width: logList.width
                        height: 28 + (open ? detailText.implicitHeight + 12 : 0)

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.smallControlRadius
                            color: lineArea.containsMouse ? Theme.navHover : "transparent"
                        }
                        Rectangle {
                            visible: logLine.index > 0
                            x: 8
                            width: parent.width - 16
                            height: Theme.edge(Screen.devicePixelRatio)
                            color: Theme.divider
                        }
                        Row {
                            x: 10
                            height: 28
                            spacing: 10

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 86
                                text: logLine.time
                                color: Theme.faint
                                font.pixelSize: 12
                                font.family: "Consolas"
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 130
                                text: logLine.plugin
                                textFormat: Text.PlainText
                                color: Theme.subtext
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 44
                                text: logLine.direction
                                color: logLine.direction === "stderr" ? Theme.caution : Theme.subtext
                                font.pixelSize: 12
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: logLine.width - 10 - 86 - 130 - 44 - 60 - 4 * 10
                                text: logLine.summary
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 60
                                text: logLine.elapsed >= 0 ? qsTr("%1 ms").arg(logLine.elapsed) : ""
                                color: Theme.faint
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                        Text { // the whole message
                            id: detailText
                            visible: logLine.open
                            x: 10
                            y: 30
                            width: parent.width - 20
                            text: logLine.detail
                            textFormat: Text.PlainText
                            color: Theme.subtext
                            font.pixelSize: 12
                            font.family: "Consolas"
                            wrapMode: Text.WrapAnywhere
                        }
                        MouseArea {
                            id: lineArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: logLine.open = !logLine.open
                        }
                    }
                }

                ListScrollBar {
                    flickable: logList
                    anchors.top: logList.top
                    anchors.bottom: logList.bottom
                    anchors.right: parent.right
                    anchors.rightMargin: 2
                }
            }
        ]
    }
}
