pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The search panel's quick filters: a query saved under a name, a chip
// each in the panel's side bar (Settings' [QuickFilters]).
SettingsSection {
    id: section
    required property SettingsEditor editor
    property string problem
    readonly property int nameWidth: width < 560 ? 110 : 140
    readonly property int actionsWidth: 108 // three icon buttons of 32 and the gaps between them
    readonly property real queryWidth: Math.max(120, width - 36 - nameWidth - actionsWidth - 12)
    // The row, gone to from the + of the panel's chips (SettingsWindow.showQuickFiltersPage).
    readonly property SettingRow filtersRow: quickFiltersRow

    // The settings window hidden (SettingsWindow.resetForReopen): nothing half typed kept.
    function resetForReopen() {
        problem = ""
        newName.clear()
        newQuery.clear()
    }

    // What was typed in the panel as a new one's query: only its name is left to give.
    function draft(query) {
        problem = ""
        newQuery.text = query
        Qt.callLater(newName.takeFocus)
    }

    SettingRow {
        id: quickFiltersRow
        objectName: "quickFiltersRow"
        title: qsTr("快捷筛选")
        keywords: qsTr("筛选器;过滤;排除;条件;保存;常用;回收站;面板;侧栏;filter;exclude;saved search").split(";")
        values: section.editor.quickFilters.map(f => f.name + " " + f.query)
        description: qsTr("把常用的搜索条件存起来，在搜索面板的侧栏点一下就套用，再点一下取消；同时开几个就要全部满足。条件和搜索框里的写法一样：%1 排除回收站里的东西，%2 排除这类文件夹，%3 只看大 PDF。").arg("!$RECYCLE.BIN\\").arg("!node_modules\\").arg("ext:pdf size:>10mb")

        body: [
            Row {
                spacing: 6
                visible: section.editor.quickFilters.length > 0
                Text { width: section.nameWidth; text: qsTr("名称"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
                Text { width: section.queryWidth; text: qsTr("条件"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
            },
            Column {
                width: parent.width
                spacing: 6
                visible: section.editor.quickFilters.length > 0
                Repeater {
                    model: section.editor.quickFilters
                    delegate: Row {
                        id: filterRow
                        objectName: "quickFilterRow_" + index
                        required property int index
                        required property var modelData
                        spacing: 6
                        InputBox {
                            width: section.nameWidth
                            text: filterRow.modelData.name
                            Accessible.name: qsTr("筛选名称")
                            onEditingFinished: {
                                section.problem = section.editor.renameQuickFilter(filterRow.index, text)
                                if (section.problem.length > 0)
                                    text = filterRow.modelData.name
                            }
                        }
                        InputBox {
                            objectName: "quickFilterQuery_" + filterRow.index
                            width: section.queryWidth
                            text: filterRow.modelData.query
                            // Written into the file by hand and unreadable: marked until put right.
                            error: filterRow.modelData.problem.length > 0
                            Accessible.name: qsTr("%1的条件").arg(filterRow.modelData.name)
                            onEditingFinished: {
                                section.problem = section.editor.setQuickFilterQuery(filterRow.index, text)
                                if (section.problem.length > 0)
                                    text = filterRow.modelData.query
                            }
                        }
                        FlatButton {
                            dpr: section.dpr
                            flat: true
                            glyph: ""
                            enabled: filterRow.index > 0
                            Accessible.name: qsTr("上移")
                            onClicked: section.editor.moveQuickFilter(filterRow.index, -1)
                        }
                        FlatButton {
                            dpr: section.dpr
                            flat: true
                            glyph: ""
                            enabled: filterRow.index < section.editor.quickFilters.length - 1
                            Accessible.name: qsTr("下移")
                            onClicked: section.editor.moveQuickFilter(filterRow.index, 1)
                        }
                        FlatButton {
                            dpr: section.dpr
                            flat: true
                            glyph: ""
                            Accessible.name: qsTr("删除")
                            onClicked: {
                                section.problem = ""
                                section.editor.removeQuickFilter(filterRow.index)
                            }
                        }
                    }
                }
            },
            Row {
                spacing: 6
                InputBox {
                    id: newName
                    objectName: "quickFilterNewName"
                    width: section.nameWidth
                    placeholder: qsTr("名称，例如 排除回收站")
                    Accessible.name: qsTr("新筛选的名称")
                    onAccepted: newQuery.text.trim().length > 0 ? addFilter.clicked() : newQuery.takeFocus()
                }
                InputBox {
                    id: newQuery
                    objectName: "quickFilterNewQuery"
                    width: section.queryWidth
                    placeholder: qsTr("条件，例如 %1").arg("!$RECYCLE.BIN\\")
                    Accessible.name: qsTr("新筛选的条件")
                    onAccepted: newName.text.trim().length > 0 ? addFilter.clicked() : newName.takeFocus()
                }
                FlatButton {
                    dpr: section.dpr
                    id: addFilter
                    objectName: "quickFilterAdd"
                    width: section.actionsWidth
                    text: qsTr("添加")
                    enabled: newName.text.trim().length > 0 && newQuery.text.trim().length > 0
                    onClicked: {
                        section.problem = section.editor.addQuickFilter(newName.text, newQuery.text)
                        if (section.problem.length === 0) {
                            newName.clear()
                            newQuery.clear()
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
