pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import WinShun

// The launcher grown into the search panel (Launcher.panel; the forum's
// #209, as Listary has it): the search box on top with the button that
// collapses it again at the right, filters down the left (范围, 类型, 修改
// 时间, 大小, 位置: the engine applies them, see NameSearchOptions::filter),
// and the rows in columns on the right (名称, 路径, 大小, 修改时间), each
// header sorting the whole list. The same window, query, scope and rows as
// the box: Main.qml hands the keys and the rows' events over as it does
// for its own list (`view` here is that list while the panel shows).
Item {
    id: panel

    required property Launcher launcher
    required property WindowFrame frame
    required property Placement placement
    property NameTip nameTip: null // the window's, for a name or folder cut short (Main.qml)
    readonly property var view: launcher.gallery ? gallery : list
    readonly property alias searchBar: bar
    readonly property int rowHeight: launcher.gallery ? gallery.cellHeight : Theme.panelRowHeight
    readonly property int gridColumns: launcher.gallery ? gallery.columns : 1
    // The rows are the last query's while a slow search has found nothing yet (Main.qml's dimRows).
    property bool dimmed: false

    signal keyPressed(var event)
    signal editingRequested()
    signal rowClicked(int index, int modifiers)
    signal rowDoubleClicked(int index, int modifiers)
    signal rowMenuRequested(int index, point globalPos, int modifiers)
    signal rowDragRequested(int index)

    readonly property int sidebarWidth: 290
    readonly property var columnSpecs: launcher.panelColumns.visibleColumns
    property string resizingColumn: ""
    property real resizingWidth: 0
    readonly property var columnLayout: {
        const layout = {}
        let x = 0
        for (const column of columnSpecs) {
            const width = column.id === resizingColumn ? resizingWidth : column.width
            layout[column.id] = { x: x, width: width }
            x += width
        }
        return layout
    }
    readonly property real columnsWidth: {
        let width = 0
        for (const column of columnSpecs)
            width += columnLayout[column.id] ? columnLayout[column.id].width : column.width
        return width
    }
    function columnX(id) { return columnLayout[id] ? columnLayout[id].x : 0 }
    function columnWidth(id) { return columnLayout[id] ? columnLayout[id].width : 0 }

    // The columns menu is the launcher's own ContextMenu (a topmost window of its
    // own): Controls' Menu with Popup.Window opened behind the topmost launcher.
    // Main.qml forwards the keys to it (handleKey) and closes it with the others.
    // Untyped: this file's QtQuick.Controls.Basic import has a ContextMenu of its own, so `as ContextMenu` fails here.
    readonly property var columnMenu: columnLoader.item
    readonly property bool columnMenuOpen: columnMenu !== null && columnMenu.visible
    readonly property int resetColumnsAction: -1 // the other actions are the column's index

    // A second call while it is open closes it (the opener toggles what it opened).
    function openColumns(globalPos, fromKeyboard) {
        if (columnMenuOpen) {
            closeColumns()
            return
        }
        const columns = launcher.panelColumns.availableColumns
        const entries = columns.map((column, i) => ({
            text: column.title, checked: column.visible, enabled: column.id !== "name", action: i
        }))
        entries.push({ separator: true })
        entries.push({ text: qsTr("恢复默认列"), action: resetColumnsAction })
        columnLoader.active = true
        columnMenu.popup(-1, "columns", entries, globalPos, fromKeyboard === true)
    }
    function closeColumns() {
        if (columnMenu)
            columnMenu.dismiss()
    }

    Loader {
        id: columnLoader
        active: false
        sourceComponent: ContextMenu {
            objectName: "panelColumnsMenu"
            host: panel.launcher
            // Like Explorer's header menu, one pick closes it.
            onTriggered: (row, action) => {
                if (action === panel.resetColumnsAction)
                    panel.launcher.panelColumns.reset()
                else
                    panel.launcher.panelColumns.toggle(panel.launcher.panelColumns.availableColumns[action].id)
            }
        }
    }

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    // Fades in as the window grows into it (the window is resized once;
    // the rows then come in over it).
    onVisibleChanged: {
        if (visible) {
            fadeIn.restart()
        } else { // also free the menu window until it is needed again
            closeColumns()
            columnLoader.active = false
        }
    }
    NumberAnimation { id: fadeIn; target: panel; property: "opacity"; from: 0; to: 1; duration: 160; easing.type: Easing.OutCubic }

    SearchBar {
        id: bar
        width: parent.width
        launcher: panel.launcher
        placement: panel.placement
        frame: panel.frame
        panelMode: true
        onKeyPressed: (event) => panel.keyPressed(event)
        onEditingRequested: panel.editingRequested()
    }

    Rectangle {
        id: topDivider
        anchors.top: bar.bottom
        width: parent.width
        height: Theme.edge(Screen.devicePixelRatio)
        color: Theme.divider
    }

    // ---- the filters -----------------------------------------------------------------

    component Choice: Item {
        id: choice

        property string text
        property string tip
        property bool active: false
        readonly property bool tipWanted: choiceArea.containsMouse && tip.length > 0

        signal clicked()

        width: parent ? parent.width : 200
        height: Theme.labelRowHeight + 6
        Accessible.role: Accessible.RadioButton
        Accessible.name: text
        Accessible.checked: active

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            radius: Theme.controlRadius
            color: choice.active ? Theme.selection : choiceArea.containsMouse ? Theme.hover : "transparent"
        }
        Rectangle { // accent pill, as on the current row
            visible: choice.active
            x: 9
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: 14
            radius: Theme.smallRadius
            color: Theme.accent
        }
        Text {
            x: 20
            width: parent.width - 32
            y: panel.onPixel((choice.height - height) / 2)
            text: choice.text
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: choice.active || choiceArea.containsMouse ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiBody
            font.weight: choice.active ? Font.DemiBold : Font.Normal
        }
        MouseArea {
            id: choiceArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: choice.clicked()
        }
    }

    component GroupTitle: Text {
        x: 16
        height: Theme.labelRowHeight
        verticalAlignment: Text.AlignVCenter
        color: Theme.faint
        font.pixelSize: Theme.uiBody
    }

    // A row of chips that wraps: one choice of a filter each, as the strip
    // under the box shows the kinds of file (TypeStrip).
    component Chips: Flow {
        x: 16
        width: parent ? parent.width - 32 : 200
        spacing: 6
        bottomPadding: 6
    }
    component Chip: Rectangle {
        id: chip

        property string text
        property string tip
        property bool active: false
        property bool multi: false
        readonly property bool tipWanted: chipArea.containsMouse && tip.length > 0

        signal clicked()

        width: Math.ceil(chipLabel.implicitWidth) + 20 // whole pixels: the labels after it stay sharp
        height: Theme.chipHeight + 4
        radius: Theme.pillRadius(height)
        color: active ? Theme.chipActive : chipArea.containsMouse ? Theme.chipHover : "transparent"
        border.width: active ? 1 : 0
        border.color: Theme.chipBorder
        Accessible.role: multi ? Accessible.CheckBox : Accessible.RadioButton
        Accessible.name: text
        Accessible.checked: active
        activeFocusOnTab: true
        Keys.onSpacePressed: chip.clicked()
        Keys.onReturnPressed: chip.clicked()

        Text {
            id: chipLabel
            x: 10
            y: panel.onPixel((chip.height - height) / 2)
            text: chip.text
            color: chip.active || chipArea.containsMouse ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiBody
            font.weight: chip.active ? Font.DemiBold : Font.Normal
        }
        MouseArea {
            id: chipArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.clicked()
        }
    }

    // A small field for a bound typed by hand (a date, a size in MB). Enter
    // and Esc give the keyboard back to the search field (what it typed
    // is taken on either); Tab goes to the box beside it.
    component BoundBox: InputBox {
        id: bound
        property Item next
        implicitHeight: Theme.chipHeight + 4
        implicitWidth: 110
        onAccepted: panel.searchBar.focusAtEnd()
        Keys.onEscapePressed: panel.searchBar.focusAtEnd()
        Keys.onTabPressed: if (next) next.takeFocus(); else panel.searchBar.focusAtEnd()
    }

    Item {
        id: sidebar
        anchors.top: topDivider.bottom
        anchors.bottom: footer.top
        width: panel.sidebarWidth

        Flickable {
            id: filters
            anchors.fill: parent
            contentWidth: width
            contentHeight: filterColumn.height + 12
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick

            Column {
                id: filterColumn
                width: filters.width
                topPadding: 6

                Item { // "筛选", and the link that clears them all
                    width: parent.width
                    height: Theme.labelRowHeight

                    Text {
                        x: 20
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("筛选")
                        color: Theme.subtext
                        font.pixelSize: Theme.uiBody
                        font.weight: Font.DemiBold
                    }
                    Text {
                        id: clearAll
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        visible: panel.launcher.filtersActive
                        text: qsTr("清除全部")
                        color: Theme.accent
                        font.pixelSize: Theme.uiBody
                        font.underline: clearArea.containsMouse
                        Accessible.role: Accessible.Button
                        Accessible.name: text

                        MouseArea {
                            id: clearArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.launcher.clearFilters()
                        }
                    }
                }

                GroupTitle { text: qsTr("范围") }
                Chips {
                    Chip {
                        text: qsTr("全部")
                        active: panel.launcher.scope === Launcher.All
                        onClicked: panel.launcher.scope = Launcher.All
                    }
                    Chip {
                        text: qsTr("文件")
                        active: panel.launcher.scope === Launcher.Files
                        onClicked: panel.launcher.scope = Launcher.Files
                    }
                    Chip {
                        text: qsTr("文件夹")
                        active: panel.launcher.scope === Launcher.Folders
                        onClicked: panel.launcher.chooseFileType(-2) // Launcher.kFoldersChip
                    }
                    Chip {
                        visible: panel.launcher.contentEnabled
                        text: qsTr("内容")
                        active: panel.launcher.scope === Launcher.Content
                        onClicked: panel.launcher.scope = Launcher.Content
                    }
                }

                GroupTitle { text: qsTr("类型（可多选）") }
                Chips {
                    Chip {
                        text: qsTr("不限")
                        active: panel.launcher.scope !== Launcher.Folders && panel.launcher.fileType < 0
                        onClicked: panel.launcher.chooseFileType(-1)
                    }
                    Repeater {
                        model: panel.launcher.fileTypes

                        delegate: Chip {
                            required property int index
                            required property var modelData
                            multi: true
                            text: modelData.name
                            tip: modelData.extensions.split(" ").slice(0, 12).map(e => "." + e).join(" ")
                            active: panel.launcher.scope !== Launcher.Folders && panel.launcher.selectedFileTypes.includes(index)
                            onClicked: panel.launcher.toggleFileType(index)
                        }
                    }
                }

                GroupTitle { text: qsTr("修改时间") }
                Chips {
                    Repeater {
                        model: [qsTr("不限"), qsTr("今天"), qsTr("最近 7 天"), qsTr("最近 30 天"), qsTr("今年"), qsTr("自定义")]

                        delegate: Chip {
                            required property int index
                            required property string modelData
                            text: modelData
                            active: panel.launcher.dateChoice === index
                            onClicked: {
                                panel.launcher.setDateChoice(index)
                                if (index === 5)
                                    dateFrom.takeFocus()
                            }
                        }
                    }
                }
                Row { // 自定义: from and to, "yyyy-MM-dd"
                    x: 16
                    visible: panel.launcher.dateChoice === 5
                    spacing: 6
                    topPadding: 4
                    bottomPadding: 4

                    BoundBox {
                        id: dateFrom
                        implicitWidth: 126 // the placeholder fits; 16 + 126 + 6 + 126 leaves 16 on the right too
                        next: dateTo
                        placeholder: qsTr("起 2026-01-01")
                        text: panel.launcher.dateFrom
                        onEditingFinished: panel.launcher.setDateRange(text, dateTo.text)
                        Component.onCompleted: panel.frame.addControl(dateFrom)
                    }
                    BoundBox {
                        id: dateTo
                        implicitWidth: 126
                        placeholder: qsTr("止")
                        text: panel.launcher.dateTo
                        onEditingFinished: panel.launcher.setDateRange(dateFrom.text, text)
                        Component.onCompleted: panel.frame.addControl(dateTo)
                    }
                }

                GroupTitle { text: qsTr("大小") }
                Chips {
                    Repeater {
                        model: [qsTr("不限"), qsTr("小于 1 MB"), qsTr("1 MB – 100 MB"), qsTr("100 MB – 1 GB"), qsTr("大于 1 GB"), qsTr("自定义")]

                        delegate: Chip {
                            required property int index
                            required property string modelData
                            text: modelData
                            active: panel.launcher.sizeChoice === index
                            onClicked: {
                                panel.launcher.setSizeChoice(index)
                                if (index === 5)
                                    sizeMin.takeFocus()
                            }
                        }
                    }
                }
                Row { // 自定义: the least and the most, in MB
                    x: 16
                    visible: panel.launcher.sizeChoice === 5
                    spacing: 6
                    topPadding: 4
                    bottomPadding: 4

                    BoundBox {
                        id: sizeMin
                        next: sizeMax
                        placeholder: qsTr("最小 MB")
                        text: panel.launcher.sizeMin
                        onEditingFinished: panel.launcher.setSizeRange(text, sizeMax.text)
                        Component.onCompleted: panel.frame.addControl(sizeMin)
                    }
                    BoundBox {
                        id: sizeMax
                        placeholder: qsTr("最大 MB")
                        text: panel.launcher.sizeMax
                        onEditingFinished: panel.launcher.setSizeRange(sizeMin.text, text)
                        Component.onCompleted: panel.frame.addControl(sizeMax)
                    }
                }

                GroupTitle { text: qsTr("位置") }
                Choice {
                    text: qsTr("不限")
                    active: panel.launcher.folderFilter.length === 0
                    onClicked: panel.launcher.setFolderFilter("")
                }
                Repeater { // the favorite folders (the settings' 收藏夹)
                    model: panel.launcher.favoriteFolders

                    delegate: Choice {
                        required property var modelData
                        text: modelData.name
                        tip: modelData.path
                        active: panel.launcher.folderFilter.toLowerCase() === modelData.path.toLowerCase()
                        onClicked: panel.launcher.setFolderFilter(modelData.path)
                    }
                }
                Choice { // one picked with the folder dialog, while it is not a favorite
                    readonly property bool isFavorite: panel.launcher.favoriteFolders.some(
                        f => f.path.toLowerCase() === panel.launcher.folderFilter.toLowerCase())
                    visible: panel.launcher.folderFilter.length > 0 && !isFavorite
                    text: panel.launcher.folderFilter
                    tip: panel.launcher.folderFilter
                    active: true
                    onClicked: panel.launcher.pickFolderFilter()
                }
                Choice {
                    text: qsTr("选择文件夹…")
                    onClicked: panel.launcher.pickFolderFilter()
                }
            }
        }

        ListScrollBar {
            flickable: filters
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
        }

        HoverTip {
            readonly property Item hovered: {
                for (let i = 0; i < filterColumn.children.length; ++i) {
                    const c = filterColumn.children[i]
                    if (c.tipWanted)
                        return c
                    if (c instanceof Flow) {
                        for (let k = 0; k < c.children.length; ++k) {
                            if (c.children[k].tipWanted)
                                return c.children[k]
                        }
                    }
                }
                return null
            }
            target: hovered
            text: hovered ? hovered.tip : ""
        }
    }

    Rectangle { // between the filters and the rows
        anchors.left: sidebar.right
        anchors.top: topDivider.bottom
        anchors.bottom: footer.top
        width: Theme.edge(Screen.devicePixelRatio)
        color: Theme.divider
    }

    // ---- the rows --------------------------------------------------------------------

    // A column's header: sorts by it, the arrow on the one in charge.
    component Header: Item {
        id: header

        property string text
        property int key // ws::SortKey
        property string columnId
        property int columnIndex
        property int minimumWidth: 72
        property bool alignRight: false
        readonly property bool active: panel.launcher.panelSortKey === key

        height: parent.height
        Accessible.role: Accessible.Button
        Accessible.name: text
        activeFocusOnTab: true
        // These run before onPressed: with the menu open the key belongs to the menu.
        Keys.onSpacePressed: (event) => header.activate(event)
        Keys.onReturnPressed: (event) => header.activate(event)
        function activate(event) {
            if (panel.columnMenuOpen)
                panel.keyPressed(event)
            else
                panel.launcher.sortBy(key)
        }
        Keys.onMenuPressed: panel.openColumns(mapToGlobal(0, height), true)
        Keys.onPressed: (event) => {
            if (panel.columnMenuOpen) { // the menu never takes the focus: Main.qml hands its keys over
                panel.keyPressed(event)
                return
            }
            if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
                panel.openColumns(mapToGlobal(0, height), true)
                event.accepted = true
            }
        }

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 2
            anchors.bottomMargin: 2
            anchors.leftMargin: -4 // the text sits on the column line, as the rows' text; the hover reaches past it
            anchors.rightMargin: -4
            radius: Theme.controlRadius
            color: headerArea.dragging ? Theme.selection : headerArea.containsMouse ? Theme.hover : "transparent"
            border.width: header.activeFocus ? 1 : 0
            border.color: Theme.accent
        }
        Text {
            id: headerText
            x: header.alignRight ? Math.max(0, header.width - width - (arrow.visible ? 14 : 0)) : 0
            y: panel.onPixel((header.height - height) / 2)
            width: Math.min(implicitWidth, header.width - (arrow.visible ? 14 : 0))
            text: header.text
            elide: Text.ElideRight
            color: header.active ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiBody
            font.weight: header.active ? Font.DemiBold : Font.Normal
        }
        Glyph {
            id: arrow
            visible: header.active
            x: headerText.x + headerText.width + 4
            y: panel.onPixel((header.height - height) / 2)
            glyph: panel.launcher.panelSortDescending ? "" : "" // ChevronDown, ChevronUp
            size: 8
            color: Theme.subtext
        }
        MouseArea {
            id: headerArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property real pressedX: 0
            property bool dragging: false
            property int moveTo: -1
            onPressed: (mouse) => {
                pressedX = mapToGlobal(mouse.x, mouse.y).x
                dragging = false
                moveTo = header.columnIndex
            }
            onPositionChanged: (mouse) => {
                if (!pressed || !(mouse.buttons & Qt.LeftButton))
                    return
                if (Math.abs(mapToGlobal(mouse.x, mouse.y).x - pressedX) > Application.styleHints.startDragDistance)
                    dragging = true
                if (dragging) {
                    const at = mapToItem(columns, mouse.x, mouse.y).x + list.contentX
                    let target = 0
                    for (let i = 0; i < panel.columnSpecs.length; ++i) {
                        const column = panel.columnSpecs[i]
                        if (at >= panel.columnX(column.id) + panel.columnWidth(column.id) / 2)
                            target = i
                    }
                    moveTo = target
                }
            }
            onReleased: {
                if (dragging)
                    panel.launcher.panelColumns.move(header.columnIndex, moveTo)
            }
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton)
                    panel.openColumns(mapToGlobal(mouse.x, mouse.y))
                else if (!dragging)
                    panel.launcher.sortBy(header.key)
            }
        }
        HoverTip {
            target: headerArea.containsMouse && !headerArea.pressed ? header : null
            delay: 900
            text: qsTr("右键选择列；拖动表头调整顺序，拖动分隔线调整宽度")
        }
        MouseArea {
            objectName: "columnResize-" + header.columnId
            x: parent.width + 8
            width: 8
            height: parent.height
            cursorShape: Qt.SizeHorCursor
            property real startX: 0
            property real startWidth: 0
            onPressed: (mouse) => {
                startX = mapToGlobal(mouse.x, mouse.y).x
                startWidth = panel.columnWidth(header.columnId)
                panel.resizingWidth = startWidth
                panel.resizingColumn = header.columnId
            }
            onPositionChanged: (mouse) => {
                if (pressed)
                    panel.resizingWidth = Math.max(header.minimumWidth, Math.min(2000, startWidth + mapToGlobal(mouse.x, mouse.y).x - startX))
            }
            onReleased: {
                const width = panel.resizingWidth
                panel.resizingColumn = ""
                panel.launcher.panelColumns.resize(header.columnId, width)
            }
            onCanceled: panel.resizingColumn = ""
        }
    }

    Item {
        id: columns
        anchors.left: sidebar.right
        anchors.leftMargin: 1
        anchors.right: parent.right
        anchors.top: topDivider.bottom
        height: panel.launcher.gallery ? 0 : Theme.labelRowHeight + 8
        visible: !panel.launcher.gallery
        clip: true

        Repeater {
            model: panel.columnSpecs
            delegate: Header {
                required property var modelData
                required property int index
                objectName: "columnHeader-" + modelData.id
                x: panel.columnX(modelData.id) + 12 - list.contentX
                width: panel.columnWidth(modelData.id) - 24
                text: modelData.title
                key: modelData.sort
                columnId: modelData.id
                columnIndex: index
                minimumWidth: modelData.minimum
                alignRight: modelData.id === "size"
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            x: 8
            width: parent.width - 16
            height: Theme.edge(Screen.devicePixelRatio)
            color: Theme.divider
        }
    }

    ListView {
        id: list
        visible: !panel.launcher.gallery
        objectName: "panelResultList"
        anchors.left: sidebar.right
        anchors.leftMargin: 1
        anchors.right: parent.right
        anchors.top: columns.bottom
        anchors.bottom: footer.top
        topMargin: 4
        bottomMargin: horizontalScroll.visible ? 14 : 4
        clip: true
        opacity: panel.dimmed ? 0.45 : 1
        Behavior on opacity { NumberAnimation { duration: 150 } }
        model: visible ? panel.launcher.results : null
        boundsBehavior: Flickable.StopAtBounds
        highlightMoveDuration: 0
        reuseItems: true
        contentWidth: Math.max(width, panel.columnsWidth)
        flickableDirection: Flickable.AutoFlickIfNeeded
        ScrollBar.horizontal: ScrollBar {
            id: horizontalScroll
            policy: ScrollBar.AsNeeded
            height: 8
            contentItem: Rectangle { implicitHeight: 6; radius: Theme.pillRadius(height); color: Theme.faint }
        }
        function stopScrolling() {
            wheelScroll.cancel()
            cancelFlick()
        }
        ListWheelScroll {
            id: wheelScroll
            view: list
            animate: SystemTheme.animations
        }
        onCurrentIndexChanged: {
            positionViewAtIndex(currentIndex, ListView.Contain)
            if (currentIndex === 0)
                contentY = originY - topMargin
        }
        onContentYChanged: if (panel.nameTip) panel.nameTip.dismiss() // scrolled: another row under the pointer

        // Scrolled to the end: a page more (Launcher.loadMore), as the box's list.
        function loadMoreAtEnd() {
            if (visible && atYEnd && count > 0 && panel.launcher.moreAvailable && !panel.launcher.searching)
                panel.launcher.loadMore()
        }
        onAtYEndChanged: Qt.callLater(loadMoreAtEnd)
        Connections {
            target: panel.launcher
            function onStatusChanged() { Qt.callLater(list.loadMoreAtEnd) }
            function onPanelRowRestored(index) { if (index >= 0) list.currentIndex = index }
        }

        delegate: PanelRow {
            id: panelRow
            width: list.contentWidth
            height: Theme.panelRowHeight
            columns: panel.columnSpecs
            columnLayout: panel.columnLayout
            nameTip: panel.nameTip
            // On whole device pixels, as the box's rows (see Main.qml).
            transform: ScrollPixelAlignment {
                view: list
                viewY: list.y
                itemY: panelRow.y
            }
            onClicked: (index, modifiers) => panel.rowClicked(index, modifiers)
            onDoubleClicked: (index, modifiers) => panel.rowDoubleClicked(index, modifiers)
            onMenuRequested: (index, globalPos, modifiers) => panel.rowMenuRequested(index, globalPos, modifiers)
            onDragRequested: (index) => panel.rowDragRequested(index)
        }
    }

    ListScrollBar {
        id: listScrollBar
        flickable: panel.view
        anchors.right: parent.right
        anchors.top: list.top
        anchors.bottom: list.bottom
        // Its own at the window's right edge, not the edge that resizes the window.
        Component.onCompleted: panel.frame.addControl(listScrollBar)
    }

    Gallery {
        id: gallery
        objectName: "panelGallery"
        visible: panel.launcher.gallery
        anchors.left: sidebar.right
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.top: columns.bottom
        anchors.topMargin: 8
        anchors.bottom: footer.top
        launcher: panel.launcher
        nameTip: panel.nameTip
        opacity: panel.dimmed ? 0.45 : 1
        onRowClicked: (index, modifiers) => panel.rowClicked(index, modifiers)
        onRowDoubleClicked: (index, modifiers) => panel.rowDoubleClicked(index, modifiers)
        onRowMenuRequested: (index, globalPos, modifiers) => panel.rowMenuRequested(index, globalPos, modifiers)
        onRowDragRequested: index => panel.rowDragRequested(index)
    }

    Text { // nothing to list, or not yet
        readonly property string query: panel.launcher.query.trim().replace(/\s+/g, " ")
        anchors.centerIn: panel.view
        width: panel.view.width - 48
        horizontalAlignment: Text.AlignHCenter
        visible: panel.view.count === 0
        text: panel.launcher.queryError.length > 0 ? panel.launcher.queryError
            : panel.launcher.searching ? (panel.launcher.scope === Launcher.Content ? qsTr("正在搜索文件内容…") : qsTr("正在搜索…"))
            : query.length > 0 ? qsTr("没有找到“%1”").arg(query)
            : panel.launcher.filtersActive ? qsTr("没有符合筛选条件的文件")
            : qsTr("输入名称或拼音，或在左侧加筛选条件")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.faint
        font.pixelSize: Theme.uiBody
    }

    Footer {
        id: footer
        anchors.bottom: parent.bottom
        width: parent.width
        launcher: panel.launcher
        frame: panel.frame
        // The other view, on the same file where there is one.
        onGalleryRequested: gallery => {
            const path = panel.launcher.results.pathAt(panel.view.currentIndex)
            panel.launcher.gallery = gallery
            Qt.callLater(() => {
                const index = panel.launcher.results.indexOf(path)
                panel.view.currentIndex = index >= 0 ? index : panel.launcher.results.defaultRow()
                panel.searchBar.focusAtEnd()
            })
        }
    }
}
