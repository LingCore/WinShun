pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The clipboard page's categories: 全部 / 文本 / 链接 / 图片 / 文件, then the
// groups (固定 first) and a + that makes a new one. Tab and Shift+Tab switch
// too. Naming a group (new or renamed) happens in a field at the end of the
// row; Enter keeps the name, Esc drops it.
Item {
    id: tabs

    required property var clipboard
    property bool naming: false
    property string nameText // what the field starts with
    property real parentLeft // where the parent's left is in the window: to draw on whole device pixels

    signal nameCommitted(string name)
    signal nameCancelled()
    signal menuRequested(int category, point globalPos)

    function startNaming(text) {
        nameText = text
        naming = true
        field.text = text
        field.selectAll()
        field.forceActiveFocus()
        Qt.callLater(() => flick.contentX = Math.max(0, flick.contentWidth - flick.width))
    }
    function stopNaming() { naming = false }

    implicitHeight: Theme.tabsHeight

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14 + orderChip.width + 6
        contentWidth: row.implicitWidth
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Row {
            id: row
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            // On whole device pixels wherever it has scrolled to, a fiftieth
            // of one to the right (see SettingsWindow): the tabs' names are
            // centred, often on half pixels.
            transform: Translate {
                x: {
                    const dpr = Screen.devicePixelRatio
                    const deviceX = (tabs.parentLeft + tabs.x + flick.x + flick.contentItem.x + row.x) * dpr
                    return (Math.round(deviceX) - deviceX + 0.02) / dpr
                }
            }

            Repeater {
                model: tabs.clipboard.categories

                delegate: Rectangle {
                    id: chip

                    required property int index
                    required property var modelData
                    readonly property bool active: index === tabs.clipboard.category

                    width: content.implicitWidth + 24
                    height: Theme.chipHeight + 2
                    radius: Theme.pillRadius(height)
                    color: active ? Theme.chipActive : area.containsMouse ? Theme.chipHover : Theme.track
                    border.width: active ? 1 : 0
                    border.color: Theme.chipBorder
                    onActiveChanged: if (active) Qt.callLater(() => tabs.ensureVisible(chip))

                    Row {
                        id: content
                        anchors.centerIn: parent
                        spacing: 5

                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: chip.modelData.pinned
                            glyph: "" // Pin
                            size: 12
                            color: chip.active ? Theme.accent : Theme.subtext
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: chip.modelData.title
                            textFormat: Text.PlainText
                            color: chip.active ? Theme.text : Theme.subtext
                            font.pixelSize: Theme.uiTab
                            font.weight: chip.active ? Theme.fontStrong : Font.Normal
                        }
                    }

                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        cursorShape: Qt.PointingHandCursor
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton)
                                tabs.menuRequested(chip.index, area.mapToGlobal(mouse.x, mouse.y))
                            else
                                tabs.clipboard.category = chip.index
                        }
                    }
                }
            }

            Rectangle { // naming a group
                visible: tabs.naming
                width: Math.max(120, field.contentWidth + 28)
                height: Theme.chipHeight + 2
                radius: Theme.pillRadius(height)
                color: Theme.inputFocused
                border.width: 1
                border.color: Theme.accent

                TextInput {
                    id: field
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.text
                    font.pixelSize: Theme.uiTab
                    selectByMouse: true
                    selectionColor: Theme.textSelection
                    selectedTextColor: Theme.text
                    maximumLength: 40
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            tabs.nameCommitted(field.text.trim())
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape) {
                            if (field.preeditText.length === 0) { // Esc while composing belongs to the input method
                                tabs.nameCancelled()
                                event.accepted = true
                            }
                        } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                            event.accepted = true // stays in the field
                        }
                    }
                    onActiveFocusChanged: if (!activeFocus && tabs.naming) tabs.nameCancelled()

                    Text {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: field.text.length === 0 && field.preeditText.length === 0
                        text: qsTr("分组名称")
                        color: Theme.faint
                        font: field.font
                    }
                }
            }

            Rectangle { // + : a new group
                id: addChip
                visible: !tabs.naming
                width: height
                height: Theme.chipHeight + 2
                radius: Theme.pillRadius(height)
                color: addArea.containsMouse ? Theme.chipHover : "transparent"

                Glyph {
                    x: Theme.snap((parent.width - width) / 2, Screen.devicePixelRatio) // on whole device pixels
                    y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio)
                    glyph: "" // Add
                    size: 64 / 3 // as the header's pin button (RowAction): 32 device pixels at 150%
                    color: addArea.containsMouse ? Theme.text : Theme.subtext
                }

                MouseArea {
                    id: addArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tabs.clipboard.groupNameRequested(-2, 0) // a new, empty group
                }
            }
        }
    }

    // How the list is ordered: "固定" first (in its own order), or all newest
    // first (Clipboard.pinnedFirst). A group's tab has its own order either way.
    Rectangle {
        id: orderChip
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        width: height
        height: Theme.chipHeight + 2
        radius: Theme.pillRadius(height)
        color: tabs.clipboard.pinnedFirst ? Theme.chipActive : orderArea.containsMouse ? Theme.chipHover : "transparent"
        border.width: tabs.clipboard.pinnedFirst ? 1 : 0
        border.color: Theme.chipBorder

        Glyph {
            x: Theme.snap((parent.width - width) / 2, Screen.devicePixelRatio) // on whole device pixels
            y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio)
            glyph: "" // Sort
            size: 64 / 3 // as the header's pin button (RowAction): 32 device pixels at 150%
            color: tabs.clipboard.pinnedFirst ? Theme.accent : orderArea.containsMouse ? Theme.text : Theme.subtext
        }

        MouseArea {
            id: orderArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: tabs.clipboard.pinnedFirst = !tabs.clipboard.pinnedFirst
        }
    }

    HoverTip {
        target: orderArea.containsMouse ? orderChip : null
        text: tabs.clipboard.pinnedFirst ? qsTr("固定的排在最前（可拖动调整顺序）。点一下改为全部按时间")
                                         : qsTr("全部按时间排列。点一下让固定的排在最前")
    }

    function ensureVisible(item) {
        const x = item.mapToItem(row, 0, 0).x
        if (x < flick.contentX)
            flick.contentX = x
        else if (x + item.width > flick.contentX + flick.width)
            flick.contentX = x + item.width - flick.width
    }

    HoverTip {
        target: addArea.containsMouse ? addChip : null
        text: qsTr("新建分组：放进分组的内容不会过期")
    }
}
