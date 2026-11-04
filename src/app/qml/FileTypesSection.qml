pragma ComponentBehavior: Bound
import QtQuick
import WinShun

SettingsSection {
    id: section
    required property SettingsEditor editor
    property string problem
    readonly property int nameWidth: width < 560 ? 80 : 100
    readonly property int keywordWidth: 115
    readonly property int actionsWidth: 108
    readonly property real extensionsWidth: Math.max(100, width - 36 - nameWidth - keywordWidth - actionsWidth - 18)
    // The 文件类型 row, gone to from the launcher strip's + (SettingsWindow.showFileTypesPage).
    readonly property SettingRow typesRow: fileTypesRow

    // The settings window hidden (SettingsWindow.resetForReopen): nothing half typed kept.
    function resetForReopen() {
        problem = ""
        newName.clear()
        newExtensions.clear()
    }

    SettingRow {
        id: fileTypesRow
        title: qsTr("文件类型")
        keywords: qsTr("类型;扩展名;后缀;筛选;前缀;快捷键;folder;filter;type;extension").split(";")
        values: section.editor.fileTypes.map(t => t.name + " :" + t.keyword)
        description: qsTr("输入“:folder 报告”只找文件夹；类型前缀可自定义，不区分大小写。留空可停用，Ctrl+Tab 仍可切换。")

        FlatButton {
            visible: !section.editor.fileTypesDefault
            text: qsTr("恢复默认")
            onClicked: {
                section.problem = ""
                section.editor.resetFileTypes()
            }
        }

        body: [
            Row {
                spacing: 6
                Text { width: section.nameWidth; text: qsTr("名称"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
                Text { width: section.extensionsWidth; text: qsTr("扩展名"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
                Text { width: section.keywordWidth; text: qsTr("搜索前缀（:）"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
            },
            Row {
                spacing: 6
                Text {
                    width: section.nameWidth
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("文件夹")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    width: section.extensionsWidth
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("只搜索文件夹")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
                InputBox {
                    width: section.keywordWidth
                    text: section.editor.folderKeyword
                    Accessible.name: qsTr("文件夹搜索前缀")
                    onEditingFinished: {
                        section.problem = section.editor.setFileTypeKeyword(-1, text)
                        if (section.problem.length > 0)
                            text = section.editor.folderKeyword
                    }
                }
            },
            Column {
                width: parent.width
                spacing: 6
                Repeater {
                    model: section.editor.fileTypes
                    delegate: Row {
                        id: typeRow
                        required property int index
                        required property var modelData
                        spacing: 6
                        InputBox {
                            width: section.nameWidth
                            text: typeRow.modelData.name
                            Accessible.name: qsTr("类型名称")
                            onEditingFinished: {
                                section.problem = section.editor.renameFileType(typeRow.index, text)
                                if (section.problem.length > 0)
                                    text = typeRow.modelData.name
                            }
                        }
                        InputBox {
                            width: section.extensionsWidth
                            text: typeRow.modelData.extensions.join(" ")
                            Accessible.name: qsTr("%1的扩展名").arg(typeRow.modelData.name)
                            onEditingFinished: {
                                section.problem = section.editor.setFileTypeExtensions(typeRow.index, text)
                                if (section.problem.length > 0)
                                    text = typeRow.modelData.extensions.join(" ")
                            }
                        }
                        InputBox {
                            width: section.keywordWidth
                            text: typeRow.modelData.keyword
                            placeholder: qsTr("未设置")
                            Accessible.name: qsTr("%1的搜索前缀").arg(typeRow.modelData.name)
                            onEditingFinished: {
                                section.problem = section.editor.setFileTypeKeyword(typeRow.index, text)
                                if (section.problem.length > 0)
                                    text = typeRow.modelData.keyword
                            }
                        }
                        FlatButton {
                            flat: true
                            glyph: "\uE70E"
                            enabled: typeRow.index > 0
                            Accessible.name: qsTr("上移")
                            onClicked: section.editor.moveFileType(typeRow.index, -1)
                        }
                        FlatButton {
                            flat: true
                            glyph: "\uE70D"
                            enabled: typeRow.index < section.editor.fileTypes.length - 1
                            Accessible.name: qsTr("下移")
                            onClicked: section.editor.moveFileType(typeRow.index, 1)
                        }
                        FlatButton {
                            flat: true
                            glyph: "\uE74D"
                            Accessible.name: qsTr("删除")
                            onClicked: section.editor.removeFileType(typeRow.index)
                        }
                    }
                }
            },
            Row {
                spacing: 6
                InputBox {
                    id: newName
                    width: section.nameWidth
                    placeholder: qsTr("新类型")
                    Accessible.name: qsTr("新类型名称")
                    onAccepted: newExtensions.takeFocus()
                }
                InputBox {
                    id: newExtensions
                    width: section.extensionsWidth + section.keywordWidth + 6
                    placeholder: qsTr("扩展名，例如 dwg dxf")
                    Accessible.name: qsTr("新类型扩展名")
                    onAccepted: addType.clicked()
                }
                FlatButton {
                    id: addType
                    width: section.actionsWidth
                    text: qsTr("添加")
                    enabled: newName.text.trim().length > 0 && newExtensions.text.trim().length > 0
                    onClicked: {
                        section.problem = section.editor.addFileType(newName.text, newExtensions.text)
                        if (section.problem.length === 0) {
                            newName.clear()
                            newExtensions.clear()
                            newName.takeFocus()
                        }
                    }
                }
            },
            Text {
                visible: section.problem.length > 0
                width: parent.width
                text: section.problem
                wrapMode: Text.Wrap
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                Accessible.role: Accessible.AlertMessage
            }
        ]
    }
}
