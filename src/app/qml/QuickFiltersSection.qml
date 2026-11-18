pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The search panel's quick filters: a query saved under a name, a chip
// each in the panel's side bar (Settings' [QuickFilters]).
SettingsSection {
    id: section
    required property SettingsEditor editor
    property string problem
    readonly property bool narrow: width < 640
    readonly property int nameWidth: narrow ? 100 : 130
    readonly property int actionsWidth: 108 // three icon buttons of 32 and the gaps between them
    readonly property int keywordWidth: narrow ? 88 : 104
    readonly property int sortWidth: narrow ? 108 : 120
    // Too narrow for all of a filter on one line (the window at its
    // narrowest): its prefix and sort go on a line of their own, labelled.
    readonly property bool stacked: width - 36 - nameWidth - keywordWidth - sortWidth - actionsWidth - 24 < 120
    readonly property real queryWidth: stacked ? width - 36 - nameWidth - actionsWidth - 12
                                               : width - 36 - nameWidth - keywordWidth - sortWidth - actionsWidth - 24
    // A filter's own order (GitHub #392): the settings' names of the orders, none first.
    readonly property var sorts: ["", "match", "newest", "oldest", "name", "folder", "type", "largest", "smallest", "opened"]
    readonly property var sortLabels: [qsTr("不指定"), qsTr("最匹配"), qsTr("最新在前"), qsTr("最早在前"), qsTr("名称"),
        qsTr("所在文件夹"), qsTr("类型"), qsTr("大的在前"), qsTr("小的在前"), qsTr("打开最多")]
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
        values: section.editor.quickFilters.map(f => f.name + " " + f.query + " :" + f.keyword)
        description: qsTr("把常用的搜索条件存起来，在搜索面板的侧栏点一下就套用，再点一下取消；同时开几个就要全部满足。条件和搜索框里的写法一样：%1 排除回收站里的东西，%2 排除这类文件夹，%3 只看大 PDF。填了搜索前缀（例如 nobin），在搜索框里输入“:nobin 报告”也能套用，不用展开面板。还能给它定个排序，比如“大文件”按大的在前，开着它时列表就这样排。").arg("!$RECYCLE.BIN\\").arg("!node_modules\\").arg("ext:pdf size:>10mb")

        body: [
            Row {
                spacing: 6
                visible: section.editor.quickFilters.length > 0
                Text { width: section.nameWidth; text: qsTr("名称"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
                Text { width: section.queryWidth; text: qsTr("条件"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
                Text { visible: !section.stacked; width: section.keywordWidth; text: qsTr("搜索前缀（:）"); color: Theme.subtext; font.pixelSize: Theme.fontCaption; elide: Text.ElideRight }
                Text { visible: !section.stacked; width: section.sortWidth; text: qsTr("排序"); color: Theme.subtext; font.pixelSize: Theme.fontCaption }
            },
            Column {
                width: parent.width
                spacing: section.stacked ? 14 : 6 // two lines each: kept apart
                visible: section.editor.quickFilters.length > 0
                Repeater {
                    model: section.editor.quickFilters
                    delegate: Column {
                        id: filterRow
                        objectName: "quickFilterRow_" + index
                        required property int index
                        required property var modelData
                        spacing: 6
                        // Its prefix and sort: beside the condition, or under it (section.stacked).
                        Component {
                            id: extras
                            Row {
                                spacing: 6
                                Text {
                                    visible: section.stacked
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("搜索前缀（:）")
                                    color: Theme.subtext
                                    font.pixelSize: Theme.fontCaption
                                }
                                InputBox {
                                    objectName: "quickFilterKeyword_" + filterRow.index
                                    width: section.keywordWidth
                                    text: filterRow.modelData.keyword
                                    placeholder: qsTr("未设置")
                                    Accessible.name: qsTr("%1的搜索前缀").arg(filterRow.modelData.name)
                                    onEditingFinished: {
                                        section.problem = section.editor.setQuickFilterKeyword(filterRow.index, text)
                                        if (section.problem.length > 0)
                                            text = filterRow.modelData.keyword
                                    }
                                }
                                Text {
                                    visible: section.stacked
                                    anchors.verticalCenter: parent.verticalCenter
                                    leftPadding: 6
                                    text: qsTr("排序")
                                    color: Theme.subtext
                                    font.pixelSize: Theme.fontCaption
                                }
                                ChoiceBox {
                                    objectName: "quickFilterSort_" + filterRow.index
                                    width: section.sortWidth
                                    dpr: section.dpr
                                    model: section.sortLabels
                                    currentIndex: Math.max(0, section.sorts.indexOf(filterRow.modelData.sort))
                                    Accessible.name: qsTr("%1的排序").arg(filterRow.modelData.name)
                                    onActivated: (index) => section.editor.setQuickFilterSort(filterRow.index, section.sorts[index])
                                }
                            }
                        }
                        Row {
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
                            Loader {
                                active: !section.stacked
                                visible: active
                                sourceComponent: extras
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
                        Loader {
                            active: section.stacked
                            visible: active
                            sourceComponent: extras
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
                    placeholder: section.narrow ? qsTr("名称") : qsTr("名称，例如 排除回收站")
                    Accessible.name: qsTr("新筛选的名称")
                    onAccepted: newQuery.text.trim().length > 0 ? addFilter.clicked() : newQuery.takeFocus()
                }
                InputBox {
                    id: newQuery
                    objectName: "quickFilterNewQuery"
                    width: section.stacked ? section.queryWidth : section.queryWidth + section.keywordWidth + section.sortWidth + 12
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
