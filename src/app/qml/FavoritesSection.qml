pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → 收藏夹: the favorites (FavoritesSettings), in the order the
// launcher lists them with nothing typed: each with its icon, its keyword as
// keys to press, its name and where it is; dragged by the grip into another
// place, changed in its row (FavoriteForm). Added with the buttons, by
// dropping files or folders on the page (SettingsWindow's FileDropTarget),
// or from the launcher's rows.
SettingsSection {
    id: section
    objectName: "favoritesSection"

    required property FavoritesSettings favorites
    property int editing: -1 // the row whose form is open; -1: none
    // What was typed in the open form and not saved, {path, name, keyword}:
    // kept while the form is made again (moved, the list redone).
    property var draft: null
    property bool addingCommand: false
    // While a row is dragged by its grip: from where, to where it would go.
    property int dragFrom: -1
    property int dragTo: -1
    readonly property int rowHeight: 58

    // The page opened for a favorite (a row's 编辑收藏 in the launcher): its form open.
    function edit(path) {
        draft = null
        editing = favorites.indexOf(path)
    }

    note: qsTr("什么都不输入时，收藏排在最前；只输入关键词再按 Enter 就打开，收藏的文件夹按 Tab 可以进去接着搜。在搜索结果上右键可以加入收藏，也可以把文件或文件夹拖到这里。")

    onVisibleChanged: if (!visible) { editing = -1; addingCommand = false; draft = null; favorites.clearMessage() }

    SettingRow {
        id: listRow

        title: qsTr("收藏")
        keywords: qsTr("收藏;常用;书签;固定;关键词;自定义命令;命令;参数;command;favorites;bookmark;pin").split(";")
        values: {
            const words = []
            for (const f of section.favorites.items)
                words.push(f.name, f.keyword, f.shownPath)
            return words
        }
        description: section.favorites.items.length > 0
                     ? qsTr("%n 项，拖动把手排序", "", section.favorites.items.length)
                     : qsTr("还没有收藏")

        FlatButton {
            flat: true
            text: qsTr("导入")
            glyph: "\uE8B5" // Import
            onClicked: section.favorites.importFile()
        }
        FlatButton {
            flat: true
            text: qsTr("导出")
            glyph: "\uEDE1" // Export
            onClicked: section.favorites.exportFile()
        }
        body: [Flow {
            width: parent.width
            spacing: 8
            bottomPadding: 8
            FlatButton {
                text: qsTr("添加文件夹")
                glyph: "\uE8B7"
                onClicked: section.favorites.addFolder()
            }
            FlatButton {
                text: qsTr("添加文件")
                glyph: "\uE8A5"
                onClicked: section.favorites.addFile()
            }
            FlatButton {
                text: qsTr("添加命令")
                glyph: "\uE756"
                onClicked: {
                    section.editing = -1
                    section.draft = null
                    section.addingCommand = !section.addingCommand
                }
            }
        }, Loader {
            active: section.addingCommand
            visible: active
            width: parent.width
            sourceComponent: FavoriteForm {
                favorites: section.favorites
                index: -1
                count: section.favorites.items.length
                favorite: ({kind: "command", name: "", keyword: "", ownName: "", shownPath: ""})
                onFinished: section.addingCommand = false
            }
        }, Text {
            visible: section.favorites.message.length > 0
            width: parent.width
            bottomPadding: 8
            text: section.favorites.message
            textFormat: Text.PlainText
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }, Rectangle {
            id: listBox

            width: parent.width
            height: Math.max(favoriteList.implicitHeight, 48)
            radius: Theme.controlRadius
            color: "transparent"
            border.width: 1
            border.color: Theme.cardBorder

            Text {
                visible: section.favorites.items.length === 0
                anchors.centerIn: parent
                width: parent.width - 32
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("在搜索结果上右键“加入收藏夹”，或点“添加文件夹”，或把文件、文件夹拖到这里")
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Column {
                id: favoriteList
                width: parent.width

                Repeater {
                    model: section.favorites.items

                    delegate: Item {
                        id: favRow

                        required property int index
                        required property var modelData
                        readonly property bool editing: section.editing === index
                        readonly property bool dragged: section.dragFrom === index
                        // Making way for the row dragged: a row's height up or down.
                        readonly property real shift: {
                            const from = section.dragFrom
                            const to = section.dragTo
                            if (from < 0 || dragged)
                                return 0
                            if (from < to && index > from && index <= to)
                                return -section.rowHeight
                            if (from > to && index >= to && index < from)
                                return section.rowHeight
                            return 0
                        }
                        property real offset: shift

                        Behavior on offset {
                            enabled: !favRow.dragged
                            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }

                        width: favoriteList.width
                        height: editing ? favForm.implicitHeight : section.rowHeight
                        z: dragged ? 2 : 0
                        transform: Translate { y: favRow.dragged ? grip.drag.activeTranslation.y : favRow.offset }

                        Rectangle {
                            visible: !favRow.editing
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: Theme.smallControlRadius
                            color: favRow.dragged ? Theme.card : favArea.containsMouse ? Theme.navHover : "transparent"
                            border.width: favRow.dragged ? 1 : 0
                            border.color: Theme.cardBorder
                        }
                        Rectangle {
                            visible: favRow.index > 0 && !favRow.dragged
                            x: 12
                            width: parent.width - 24
                            height: Theme.edge(Screen.devicePixelRatio)
                            color: Theme.divider
                        }
                        MouseArea {
                            id: favArea
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }

                        Item { // the grip: dragged into another place
                            id: grip

                            readonly property alias drag: dragger

                            visible: !favRow.editing
                            x: 4
                            width: 24
                            height: parent.height

                            Glyph {
                                anchors.centerIn: parent
                                glyph: "\uE76F" // GripperBarHorizontal
                                size: 14
                                color: gripHover.hovered || dragger.active ? Theme.text : Theme.faint
                            }
                            HoverHandler {
                                id: gripHover
                                cursorShape: dragger.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                            }
                            DragHandler {
                                id: dragger
                                target: null
                                xAxis.enabled: false
                                enabled: section.editing < 0 && section.favorites.items.length > 1
                                onActiveChanged: {
                                    if (active) {
                                        section.dragFrom = favRow.index
                                        section.dragTo = favRow.index
                                        return
                                    }
                                    const from = section.dragFrom
                                    const to = section.dragTo
                                    section.dragFrom = -1
                                    section.dragTo = -1
                                    if (from >= 0 && to >= 0 && to !== from)
                                        section.favorites.move(from, to)
                                }
                                onActiveTranslationChanged: {
                                    if (!active)
                                        return
                                    const steps = Math.round(activeTranslation.y / section.rowHeight)
                                    section.dragTo = Math.max(0, Math.min(section.favorites.items.length - 1, favRow.index + steps))
                                }
                            }
                        }

                        Item { // as its row shows it in the launcher
                            id: favIcon
                            visible: !favRow.editing
                            opacity: favRow.modelData.missing ? 0.6 : 1
                            anchors.left: grip.right
                            anchors.leftMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            width: 24
                            height: 24

                            Image {
                                readonly property real dpr: Screen.devicePixelRatio
                                readonly property int pixels: Math.round(favIcon.width * dpr)

                                // Exactly its device pixels, unsmoothed (as in ResultRow).
                                x: 1 / 64
                                y: 1 / 64
                                width: pixels / dpr
                                height: pixels / dpr
                                sourceSize.width: pixels / dpr
                                sourceSize.height: pixels / dpr
                                smooth: false
                                asynchronous: true
                                source: favRow.modelData.icon
                            }
                        }
                        Rectangle { // the keyword, as keys to press; on the right, so the names line up
                            id: keycap
                            visible: !favRow.editing && favRow.modelData.keyword.length > 0
                            anchors.right: favButtons.left
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: visible ? Math.max(36, keywordText.implicitWidth + 16) : 0
                            height: 26
                            radius: Theme.controlRadius
                            color: Theme.keycap

                            Text {
                                id: keywordText
                                anchors.centerIn: parent
                                text: favRow.modelData.keyword
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                        Column {
                            visible: !favRow.editing
                            opacity: favRow.modelData.missing ? 0.6 : 1
                            anchors.left: favIcon.right
                            anchors.leftMargin: 10
                            anchors.right: keycap.visible ? keycap.left : favButtons.left
                            anchors.rightMargin: keycap.visible ? 12 : 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                width: parent.width
                                text: favRow.modelData.name
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: favRow.modelData.missing ? qsTr("已不存在 · %1").arg(favRow.modelData.shownPath)
                                                               : favRow.modelData.shownPath
                                textFormat: Text.PlainText
                                color: favRow.modelData.missing ? Theme.danger : Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideMiddle
                            }
                        }
                        Row {
                            id: favButtons
                            visible: !favRow.editing
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            FlatButton {
                                visible: favRow.modelData.missing && (favRow.modelData.kind === "folder" || favRow.modelData.kind === "file")
                                implicitHeight: 32
                                flat: true
                                text: qsTr("重新选择")
                                onClicked: section.favorites.repick(favRow.index)
                            }
                            FlatButton {
                                visible: !favRow.modelData.missing
                                implicitHeight: 32
                                flat: true
                                text: qsTr("打开")
                                onClicked: section.favorites.open(favRow.index)
                            }
                            FlatButton {
                                implicitHeight: 32
                                flat: true
                                text: qsTr("编辑")
                                onClicked: {
                                    section.addingCommand = false
                                    section.draft = null
                                    section.editing = favRow.index
                                }
                            }
                            FlatButton {
                                implicitHeight: 32
                                flat: true
                                text: qsTr("移除")
                                onClicked: {
                                    const removed = favRow.index
                                    if (section.editing === removed)
                                        section.editing = -1
                                    else if (section.editing > removed)
                                        section.editing -= 1 // the one being changed moves up
                                    section.favorites.remove(removed)
                                }
                            }
                        }

                        Loader {
                            id: favForm
                            active: favRow.editing
                            width: parent.width
                            sourceComponent: FavoriteForm {
                                favorites: section.favorites
                                index: favRow.index
                                favorite: favRow.modelData
                                count: section.favorites.items.length
                                draft: section.draft && section.draft.path === favRow.modelData.path ? section.draft : null
                                onFinished: {
                                    section.draft = null
                                    section.editing = -1
                                }
                                onEdited: (name, keyword, commandLine, directory) => section.draft = {
                                    path: favRow.modelData.path, name: name, keyword: keyword,
                                    commandLine: commandLine, directory: directory
                                }
                                onMoveRequested: (to) => {
                                    const from = favRow.index
                                    if (section.favorites.move(from, to))
                                        section.editing = to // the draft goes with it: it is by path
                                }
                            }
                        }
                    }
                }
            }
        }]
    }
}
