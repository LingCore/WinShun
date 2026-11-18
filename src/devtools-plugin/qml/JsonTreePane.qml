pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

// The JSON as a tree: arrows walk it (← → close and open), Enter or a
// double click opens a row, Ctrl+C copies the value, Ctrl+Shift+C the
// path; right click for the rest. With nothing to show, what to do; with
// an error, where it is.
FocusScope {
    id: pane
    Keys.onPressed: (event) => pane.walk(event) // the list hidden (nothing, an error): Esc, Ctrl+V

    required property JsonTool tool
    property real dpr: 1
    readonly property JsonTree tree: tool.tree
    readonly property int codeSize: Theme.uiBody
    readonly property int rowHeight: codeSize + 10

    signal locateError()
    signal closeRequested()

    function focusList() { list.forceActiveFocus() }
    function select(row) {
        if (row < 0 || row >= list.count)
            return
        list.currentIndex = row
        list.forceLayout() // rows just opened (a search's match) laid out first
        list.positionViewAtIndex(row, ListView.Contain)
    }
    function copyValue(row) { if (row >= 0) pane.tool.copyText(pane.tree.value(row), "value") }
    function copyPath(row) { if (row >= 0) pane.tool.copyText(pane.tree.path(row), "path") }
    function walk(event) {
        event.accepted = true
        const row = list.currentIndex
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        if (ctrl && event.key === Qt.Key_V) {
            pane.tool.paste()
        } else if (ctrl && event.key === Qt.Key_C) {
            if (shift)
                copyPath(row)
            else
                copyValue(row)
        } else if (event.key === Qt.Key_Escape) {
            pane.closeRequested()
        } else if (row < 0 && list.count > 0 && [Qt.Key_Down, Qt.Key_Up, Qt.Key_Home, Qt.Key_End].includes(event.key)) {
            select(event.key === Qt.Key_End || event.key === Qt.Key_Up ? list.count - 1 : 0)
        } else if (row < 0) {
            event.accepted = false
        } else if (event.key === Qt.Key_Left) {
            if (pane.tree.data(pane.tree.index(row, 0), JsonTree.ExpandedRole))
                pane.tree.setExpanded(row, false)
            else
                select(pane.tree.parentRow(row))
        } else if (event.key === Qt.Key_Right) {
            if (!pane.tree.data(pane.tree.index(row, 0), JsonTree.ExpandableRole))
                return
            if (!pane.tree.data(pane.tree.index(row, 0), JsonTree.ExpandedRole))
                pane.tree.setExpanded(row, true)
            else
                select(pane.tree.firstChildRow(row))
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            pane.tree.toggle(row)
        } else if (event.key === Qt.Key_Home) {
            select(0)
        } else if (event.key === Qt.Key_End) {
            select(list.count - 1)
        } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
            const page = Math.max(1, Math.floor(list.height / pane.rowHeight) - 1)
            select(Math.max(0, Math.min(list.count - 1, row + (event.key === Qt.Key_PageDown ? page : -page))))
        } else {
            event.accepted = false
        }
    }
    function openMenu(row, item, x, y) {
        list.currentIndex = row
        const p = item.mapToItem(pane, x, y)
        menuAnchor.x = p.x + menu.menuWidth
        menuAnchor.y = p.y - 4
        const container = pane.tree.data(pane.tree.index(row, 0), JsonTree.ExpandableRole)
        const named = pane.tree.data(pane.tree.index(row, 0), JsonTree.HasKeyRole)
                      && !pane.tree.data(pane.tree.index(row, 0), JsonTree.KeyIsIndexRole)
        const entries = [{ text: container ? qsTr("复制值（格式化的 JSON）") : qsTr("复制值"), glyph: "ph:copy", act: "value" }]
        if (named)
            entries.push({ text: qsTr("复制键名"), glyph: "ph:key", act: "key" })
        entries.push({ text: qsTr("复制路径"), glyph: "ph:path", act: "path" })
        if (container) {
            entries.push({ text: qsTr("复制压缩的 JSON"), glyph: "ph:arrows-in-line-horizontal", act: "compact" })
            entries.push({ text: qsTr("展开所有子项"), glyph: "ph:arrows-out-line-vertical", act: "open" })
            entries.push({ text: qsTr("折叠所有子项"), glyph: "ph:arrows-in-line-vertical", act: "close" })
        }
        menu.row = row
        menu.entries = entries
        menu.open = false
        menu.open = true
    }

    ListView {
        id: list
        objectName: "jsonTreeList"
        anchors.fill: parent
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        model: pane.tree
        visible: pane.tool.valid
        currentIndex: -1
        clip: true
        reuseItems: true
        focus: true
        activeFocusOnTab: true
        keyNavigationEnabled: true
        highlightFollowsCurrentItem: false
        boundsBehavior: Flickable.StopAtBounds
        Accessible.role: Accessible.Tree
        Accessible.name: qsTr("JSON 树")
        Keys.onPressed: (event) => pane.walk(event)
        delegate: JsonRow {
            id: rowItem
            width: ListView.view.width
            height: pane.rowHeight
            dpr: pane.dpr
            codeSize: pane.codeSize
            selected: ListView.isCurrentItem
            onClicked: { list.currentIndex = index; list.forceActiveFocus() }
            onToggled: pane.tree.toggle(index)
            onMenuRequested: (x, y) => pane.openMenu(index, rowItem, x, y)
            onHoveredChanged: {
                if (hovered)
                    tip.row = rowItem
                else if (tip.row === rowItem)
                    tip.row = null
            }
        }
        ListWheelScroll { view: list; animate: SystemTheme.animations }
    }
    ListScrollBar {
        dpr: pane.dpr
        flickable: list
        anchors.top: list.top
        anchors.bottom: list.bottom
        anchors.right: list.right
        visible: list.visible
        z: 2
    }
    HoverTip { // a value cut short: the whole of it (up to a few lines)
        id: tip
        property JsonRow row: null
        dpr: pane.dpr
        maxWidth: 560
        readonly property bool wanted: row !== null && row.cut && !row.expandable
        target: wanted ? row : null
        text: wanted ? pane.tree.value(row.index).slice(0, 600) : ""
    }

    Item { id: menuAnchor; width: 0; height: 0 }
    DropMenu {
        id: menu
        property int row: -1
        anchor: menuAnchor
        menuWidth: 220
        onActivated: (i) => {
            const act = menu.entries[i].act
            if (act === "value")
                pane.copyValue(menu.row)
            else if (act === "key")
                pane.tool.copyText(pane.tree.key(menu.row), "key")
            else if (act === "path")
                pane.copyPath(menu.row)
            else if (act === "compact")
                pane.tool.copyText(pane.tree.json(menu.row, false), "compact")
            else
                pane.tree.setSubtreeExpanded(menu.row, act === "open")
        }
    }

    // Nothing yet: how to give it some.
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 420)
        visible: pane.tool.empty
        spacing: 10
        Glyph {
            Layout.alignment: Qt.AlignHCenter
            glyph: "ph:brackets-curly"
            size: 40
            color: Theme.faint
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("粘贴 JSON 就能看")
            color: Theme.text
            font.pixelSize: Theme.uiTitle
            font.weight: Theme.fontStrong
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            lineHeight: 1.4
            text: qsTr("日志里夹着的、转义过的、一行一段的 JSON 都能认出来。\n也可以把 .json 文件拖到窗口里。")
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
        FlatButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            dpr: pane.dpr
            highlighted: true
            glyph: "ph:clipboard-text"
            text: qsTr("粘贴 (Ctrl+V)")
            onClicked: pane.tool.paste()
        }
    }

    // Not JSON: where it went wrong.
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 460)
        visible: !pane.tool.empty && !pane.tool.valid
        spacing: 10
        Glyph {
            Layout.alignment: Qt.AlignHCenter
            glyph: "ph:warning-circle"
            size: 40
            color: Theme.danger
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("不是有效的 JSON")
            color: Theme.text
            font.pixelSize: Theme.uiTitle
            font.weight: Theme.fontStrong
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            lineHeight: 1.4
            text: pane.tool.errorLine > 0 ? qsTr("第 %1 行第 %2 列：%3").arg(pane.tool.errorLine).arg(pane.tool.errorColumn).arg(pane.tool.error)
                                          : pane.tool.error
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
        FlatButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            dpr: pane.dpr
            visible: pane.tool.editable && pane.tool.errorOffset >= 0
            glyph: "ph:crosshair-simple"
            text: qsTr("在原文中定位")
            onClicked: pane.locateError()
        }
    }
}
