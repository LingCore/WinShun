pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → AI 助手 › 隐私与记录: the folders AI clients never see, and the
// log of their calls. Its rows show while AI may connect (the switch is on
// the 权限 tab, AiAccessSection, which `lockedRows` are given to).
SettingsSection {
    id: privacySection

    required property SettingsEditor editor

    // Shown only while AI may connect: for the switch's search hint.
    readonly property var lockedRows: [excludedRow, logRow]

    SettingRow {
        id: excludedRow

        title: qsTr("不让 AI 看到的文件夹")
        keywords: qsTr("排除;隐私;隐藏;黑名单;exclude;AI").split(";")
        values: privacySection.editor.mcpExcludedPaths
        description: qsTr("这些文件夹里的东西不出现在 AI 的搜索结果里，也不能打开；“搜索范围”里不搜索的文件夹 AI 本来就看不到")
        shown: privacySection.editor.mcp

        FlatButton {
            dpr: privacySection.dpr
            text: qsTr("添加文件夹")
            glyph: "" // Add
            onClicked: privacySection.editor.addMcpExcludedFolder()
        }

        body: Rectangle {
            width: parent.width
            height: Math.max(pathList.implicitHeight, 40)
            radius: Theme.controlRadius
            color: "transparent"
            border.width: 1
            border.color: Theme.cardBorder

            Text {
                visible: privacySection.editor.mcpExcludedPaths.length === 0
                anchors.centerIn: parent
                text: qsTr("AI 能看到索引里的所有文件夹")
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
            }

            Column {
                id: pathList
                width: parent.width

                Repeater {
                    model: privacySection.editor.mcpExcludedPaths

                    delegate: Item {
                        id: pathRow

                        required property int index
                        required property string modelData

                        width: pathList.width
                        height: 42

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: Theme.smallControlRadius
                            color: rowArea.containsMouse ? Theme.navHover : "transparent"
                        }
                        Rectangle {
                            visible: pathRow.index > 0
                            x: 12
                            width: parent.width - 24
                            height: Theme.edge(Screen.devicePixelRatio)
                            color: Theme.divider
                        }
                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }
                        Glyph {
                            id: folderIcon
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "" // Folder
                            size: 16
                        }
                        Text {
                            anchors.left: folderIcon.right
                            anchors.leftMargin: 10
                            anchors.right: removePath.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: pathRow.modelData
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideMiddle
                        }
                        FlatButton {
                            dpr: privacySection.dpr
                            id: removePath
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            implicitHeight: 32
                            flat: true
                            text: qsTr("移除")
                            onClicked: privacySection.editor.removeMcpExcludedPath(pathRow.index)
                        }
                    }
                }
            }
        }
    }

    SettingRow {
        id: logRow

        title: qsTr("调用记录")
        keywords: qsTr("日志;记录;log;AI").split(";")
        description: qsTr("AI 每次调用都记在 mcp.log 里：时间、客户端、工具和参数、结果数量；不记录剪贴板里的文字")
        shown: privacySection.editor.mcp

        FlatButton {
            dpr: privacySection.dpr
            text: qsTr("打开")
            glyph: "" // Document
            onClicked: privacySection.editor.openMcpLog()
        }
    }
}
