pragma ComponentBehavior: Bound
import QtQuick
import WinShun

SettingsSection {
    id: section
    required property SettingsEditor editor
    property string problem
    readonly property int nameWidth: width < 560 ? 80 : 100
    readonly property int keywordWidth: width < 560 ? 96 : 115
    readonly property int actionsWidth: 146 // four icon buttons of 32 and the gaps between them
    readonly property int gripWidth: 24 // the handle a row is dragged by
    // What is left; narrow, the extensions box gives way first (its text scrolls).
    readonly property real extensionsWidth: Math.max(64, width - 36 - gripWidth - 6 - nameWidth - keywordWidth - actionsWidth - 18)
    // The strip's chips after 不限, in its order: the kinds with 文件夹 where it
    // stands (editor.folderChipAt); `type` -1 for 文件夹, a kind's index otherwise.
    readonly property var chips: {
        const list = editor.fileTypes.map((t, i) => ({ type: i, name: t.name, extensions: t.extensions, keyword: t.keyword, folder: t.folder }))
        list.splice(Math.min(editor.folderChipAt, list.length), 0,
                    { type: -1, name: qsTr("文件夹"), extensions: [], keyword: editor.folderKeyword, folder: "" })
        return list
    }
    // A row dragged by its handle: its place, and the place it takes when let go.
    property int dragFrom: -1
    property int dragTo: -1
    // The 文件类型 row, gone to from the launcher strip's + (SettingsWindow.showFileTypesPage).
    readonly property SettingRow typesRow: fileTypesRow

    // The settings window hidden (SettingsWindow.resetForReopen): nothing half typed kept.
    function resetForReopen() {
        problem = ""
        newName.clear()
        newExtensions.clear()
    }

    SettingRow {
        title: qsTr("类型标签显示数量")
        keywords: qsTr("数量;个数;计数;统计;标签;角标;count;badge;number").split(";")
        description: qsTr("在“文件”里输入后，每个类型的名字右边显示它有多少个匹配；选了某个类型，其他类型的数量照样显示")

        ToggleSwitch {
            dpr: section.dpr
            objectName: "typeCountsToggle"
            checked: section.editor.typeCounts
            Accessible.name: qsTr("类型标签显示数量")
            onToggled: (on) => section.editor.typeCounts = on
        }
    }

    SettingRow {
        id: fileTypesRow
        title: qsTr("文件类型")
        keywords: qsTr("类型;扩展名;后缀;筛选;前缀;快捷键;搜索范围;目录;顺序;排序;拖动;folder;filter;type;extension;order").split(";")
        values: section.editor.fileTypes.map(t => t.name + " :" + t.keyword + " " + t.folder)
        description: qsTr("前缀不区分大小写，留空停用。点文件夹图标可绑定一个文件夹（含子文件夹）；例如 cw + csv，输入“cw:”直接列出该范围的 CSV。拖动左边的把手调整顺序（文件夹也能移动），搜索框下的类型条上按住类型左右拖动也可以。Ctrl+Tab 按这个顺序切换。")

        FlatButton {
            dpr: section.dpr
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
                Item { width: section.gripWidth; height: 1 }
                Text { width: section.nameWidth; text: qsTr("名称"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
                Text { width: section.extensionsWidth; text: qsTr("扩展名"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
                Text { width: section.keywordWidth; text: qsTr("搜索前缀（:）"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
            },
            Column {
                id: chipList
                width: parent.width
                spacing: 6
                Repeater {
                    id: chipRows
                    model: section.chips
                    delegate: Item {
                        id: typeRow
                        objectName: "fileTypeRow_" + index
                        required property int index
                        required property var modelData
                        readonly property bool folder: modelData.type < 0 // 文件夹: its prefix only
                        readonly property int type: modelData.type
                        property bool editingRange: false
                        readonly property bool dragged: section.dragFrom === index
                        // Making way for the row dragged: its height and the gap, up or down.
                        readonly property real shift: {
                            const from = section.dragFrom
                            const to = section.dragTo
                            const moving = chipRows.itemAt(from)
                            if (from < 0 || dragged || !moving)
                                return 0
                            if (from < to && index > from && index <= to)
                                return -(moving.height + chipList.spacing)
                            if (from > to && index >= to && index < from)
                                return moving.height + chipList.spacing
                            return 0
                        }
                        property real offset: shift
                        Behavior on offset {
                            enabled: !typeRow.dragged && SystemTheme.animations
                            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                        // The row dragged follows the pointer, inside the list.
                        readonly property real dragY: Math.max(-y, Math.min(chipList.height - y - height, dragger.activeTranslation.y))

                        width: chipList.width
                        height: lines.implicitHeight
                        z: dragged ? 2 : 0
                        transform: Translate { y: typeRow.dragged ? typeRow.dragY : typeRow.offset }

                        Rectangle { // lifted while dragged
                            visible: typeRow.dragged
                            anchors.fill: parent
                            anchors.margins: -3
                            radius: Theme.smallControlRadius
                            color: Theme.card
                            border.width: 1
                            border.color: Theme.cardBorder
                        }

                        Column {
                            id: lines
                            width: parent.width
                            spacing: 4
                            Row {
                                spacing: 6
                                Item { // the handle: dragged into another place
                                    width: section.gripWidth
                                    height: [32, 36, 40][Theme.step]
                                    Glyph {
                                        anchors.centerIn: parent
                                        glyph: "ph:dots-six-vertical"
                                        size: 16
                                        color: gripHover.hovered || dragger.active ? Theme.text : Theme.faint
                                    }
                                    HoverHandler {
                                        id: gripHover
                                        cursorShape: dragger.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                    }
                                    DragHandler {
                                        id: dragger
                                        objectName: "fileTypeGrip_" + typeRow.index
                                        target: null
                                        xAxis.enabled: false
                                        enabled: section.chips.length > 1
                                        onActiveChanged: {
                                            if (active) {
                                                section.dragTo = typeRow.index
                                                section.dragFrom = typeRow.index
                                                return
                                            }
                                            const from = section.dragFrom
                                            const to = section.dragTo
                                            section.dragFrom = -1
                                            section.dragTo = -1
                                            if (from >= 0 && to >= 0 && to !== from)
                                                section.editor.moveStripChip(from, to)
                                        }
                                        // The place of the last row whose middle its leading edge has
                                        // passed, either way (rows with a range are taller).
                                        onActiveTranslationChanged: {
                                            if (!active)
                                                return
                                            const top = typeRow.y + typeRow.dragY
                                            const bottom = top + typeRow.height
                                            let to = typeRow.index
                                            for (let i = typeRow.index + 1; i < chipRows.count; ++i) {
                                                const other = chipRows.itemAt(i)
                                                if (!other || bottom <= other.y + other.height / 2)
                                                    break
                                                to = i
                                            }
                                            for (let i = typeRow.index - 1; i >= 0; --i) {
                                                const other = chipRows.itemAt(i)
                                                if (!other || top >= other.y + other.height / 2)
                                                    break
                                                to = i
                                            }
                                            section.dragTo = to
                                        }
                                    }
                                }
                                Text { // 文件夹: named as the strip names it
                                    visible: typeRow.folder
                                    width: section.nameWidth
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: typeRow.modelData.name
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                }
                                Text {
                                    visible: typeRow.folder
                                    width: section.extensionsWidth
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("只搜索文件夹")
                                    color: Theme.subtext
                                    font.pixelSize: Theme.fontCaption
                                }
                                InputBox {
                                    visible: !typeRow.folder
                                    width: section.nameWidth
                                    text: typeRow.modelData.name
                                    Accessible.name: qsTr("类型名称")
                                    onEditingFinished: {
                                        section.problem = section.editor.renameFileType(typeRow.type, text)
                                        if (section.problem.length > 0)
                                            text = typeRow.modelData.name
                                    }
                                }
                                InputBox {
                                    visible: !typeRow.folder
                                    width: section.extensionsWidth
                                    text: typeRow.modelData.extensions.join(" ")
                                    Accessible.name: qsTr("%1的扩展名").arg(typeRow.modelData.name)
                                    onEditingFinished: {
                                        section.problem = section.editor.setFileTypeExtensions(typeRow.type, text)
                                        if (section.problem.length > 0)
                                            text = typeRow.modelData.extensions.join(" ")
                                    }
                                }
                                InputBox {
                                    width: section.keywordWidth
                                    text: typeRow.modelData.keyword
                                    placeholder: qsTr("未设置")
                                    Accessible.name: typeRow.folder ? qsTr("文件夹搜索前缀") : qsTr("%1的搜索前缀").arg(typeRow.modelData.name)
                                    onEditingFinished: {
                                        section.problem = section.editor.setFileTypeKeyword(typeRow.type, text)
                                        if (section.problem.length > 0)
                                            text = typeRow.modelData.keyword
                                    }
                                }
                                FlatButton {
                                    dpr: section.dpr
                                    flat: true
                                    visible: !typeRow.folder
                                    objectName: "fileTypeScope_" + typeRow.type
                                    // An icon like its neighbours (Phosphor's folder), lit while a folder is bound.
                                    glyph: "ph:folder"
                                    highlighted: typeRow.modelData.folder.length > 0
                                    Accessible.name: qsTr("%1的搜索范围").arg(typeRow.modelData.name)
                                    // With no folder bound it opens the row below and, clicked again, puts
                                    // it away; a bound folder's row stays, and the click goes to its box.
                                    onClicked: {
                                        typeRow.editingRange = typeRow.modelData.folder.length > 0 || !typeRow.editingRange
                                        if (typeRow.editingRange)
                                            rangeInput.takeFocus()
                                    }
                                }
                                Item { // 文件夹 has no range of its own
                                    visible: typeRow.folder
                                    width: 32
                                    height: 1
                                }
                                FlatButton {
                                    dpr: section.dpr
                                    flat: true
                                    glyph: "ph:caret-up"
                                    enabled: typeRow.index > 0
                                    Accessible.name: qsTr("上移")
                                    onClicked: section.editor.moveStripChip(typeRow.index, typeRow.index - 1)
                                }
                                FlatButton {
                                    dpr: section.dpr
                                    flat: true
                                    glyph: "ph:caret-down"
                                    enabled: typeRow.index < section.chips.length - 1
                                    Accessible.name: qsTr("下移")
                                    onClicked: section.editor.moveStripChip(typeRow.index, typeRow.index + 1)
                                }
                                FlatButton {
                                    dpr: section.dpr
                                    flat: true
                                    visible: !typeRow.folder // always there, as 不限
                                    glyph: "ph:trash"
                                    Accessible.name: qsTr("删除")
                                    onClicked: section.editor.removeFileType(typeRow.type)
                                }
                            }
                            Row {
                                spacing: 6
                                visible: !typeRow.folder && (typeRow.editingRange || typeRow.modelData.folder.length > 0)
                                Item { width: section.gripWidth; height: 1 }
                                Text {
                                    width: section.nameWidth
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("搜索范围")
                                    color: Theme.subtext
                                    font.pixelSize: Theme.fontCaption
                                }
                                InputBox {
                                    id: rangeInput
                                    objectName: "fileTypeFolder_" + typeRow.type
                                    width: section.extensionsWidth + section.keywordWidth + 6
                                    text: typeRow.modelData.folder
                                    placeholder: qsTr("不限范围；可填写完整文件夹路径")
                                    Accessible.name: qsTr("%1的范围路径（含子文件夹）").arg(typeRow.modelData.name)
                                    onEditingFinished: {
                                        section.problem = section.editor.setFileTypeFolder(typeRow.type, text)
                                        if (section.problem.length > 0)
                                            text = typeRow.modelData.folder
                                    }
                                }
                                FlatButton {
                                    dpr: section.dpr
                                    width: section.actionsWidth
                                    text: qsTr("选择文件夹…")
                                    onClicked: section.problem = section.editor.chooseFileTypeFolder(typeRow.type)
                                }
                            }
                        }
                    }
                }
            },
            Row {
                spacing: 6
                Item { width: section.gripWidth; height: 1 }
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
                    dpr: section.dpr
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
