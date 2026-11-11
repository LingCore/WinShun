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
    // What the box is for: a group's name, or an entry's note (GitHub #267).
    property string namePlaceholder: qsTr("分组名称")
    property int nameLength: 40
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

    property UiScale metrics: Theme.metrics
    readonly property real dpr: Screen.devicePixelRatio

    implicitHeight: tabs.metrics.tabsHeight

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14 + orderChip.width + 6
        contentWidth: row.implicitWidth
        // While a name is typed the box at the end stays in sight: the row is laid out
        // a moment after the box grows, so this follows the width it comes to.
        onContentWidthChanged: if (tabs.naming) contentX = Math.max(0, contentWidth - width)
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

                // A chip on the style's chipSurface (role "group"): 全部, the
                // resting choice, is the neutral one (isAll).
                delegate: Item {
                    id: chip

                    required property int index
                    required property var modelData
                    readonly property bool active: index === tabs.clipboard.category
                    // Where the words start: after the lamp's room where the part has one (tactile's 全部), else 12.
                    readonly property real lead: surface.out("leadingInset", 0) > 0 ? surface.out("leadingInset", 0) : 12

                    width: Math.ceil(lead + content.implicitWidth) + 12
                    height: tabs.metrics.chipHeight + 2
                    onActiveChanged: if (active) Qt.callLater(() => tabs.ensureVisible(chip))

                    Hook {
                        id: surface
                        anchors.fill: parent
                        part: Theme.chipSurface
                        checked: chip.active
                        hovered: area.containsMouse
                        pressed: area.pressed
                        isAll: chip.index === 0
                        role: "group"
                        dpr: tabs.dpr
                    }

                    Row {
                        id: content
                        x: chip.lead
                        y: Theme.snap((chip.height - height) / 2 + surface.out("contentOffsetY", 0), tabs.dpr)
                        spacing: 5

                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: chip.modelData.pinned
                            glyph: "" // Pin
                            size: 12
                            color: surface.out("iconColor", chip.active && chip.index !== 0 ? Theme.accentText : Theme.subtext)
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: chip.modelData.title
                            textFormat: Text.PlainText
                            color: surface.out("foreground", chip.active && chip.index !== 0 ? Theme.accentText
                                                            : chip.active || area.containsMouse ? Theme.text : Theme.subtext)
                            font.pixelSize: tabs.metrics.uiTab
                            font.weight: surface.out("fontWeight", chip.active ? Theme.fontStrong : Font.Normal)
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

            Item { // naming a group
                visible: tabs.naming
                // As wide as what is typed, or as the words that say what to type.
                width: Math.max(120, Math.max(field.contentWidth, field.text.length === 0 ? hintMetrics.advanceWidth : 0) + 28)

                TextMetrics { // measured apart from the items laid out in the box
                    id: hintMetrics
                    font: field.font
                    text: tabs.namePlaceholder
                }
                height: tabs.metrics.chipHeight + 2

                Hook { // the style's field, always focused here
                    id: namePart
                    anchors.fill: parent
                    part: Theme.inputField
                    focused: true
                    dpr: tabs.dpr
                }

                TextInput {
                    id: field
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: namePart.out("textColor", Theme.text)
                    font.pixelSize: tabs.metrics.uiTab
                    selectByMouse: true
                    selectionColor: Theme.textSelection
                    selectedTextColor: Theme.text
                    maximumLength: tabs.nameLength
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
                        text: tabs.namePlaceholder
                        color: namePart.out("placeholderColor", Theme.faint)
                        font: field.font
                    }
                }
            }

            Item { // + : a new group
                id: addChip
                visible: !tabs.naming
                width: height
                height: tabs.metrics.chipHeight + 2

                Hook { // the style's key, flat until hovered, round as the chips
                    id: addKey
                    anchors.fill: parent
                    part: Theme.keySurface
                    hovered: addArea.containsMouse
                    pressed: addArea.pressed
                    flat: true
                    corner: Theme.pillRadius(addChip.height)
                    role: "toolbar"
                    dpr: tabs.dpr
                }

                Glyph {
                    x: Theme.snap((parent.width - width) / 2, Screen.devicePixelRatio) // on whole device pixels
                    y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio)
                    glyph: "" // Add
                    size: tabs.metrics.dense ? 16 : 64 / 3 // as the header's pin button (RowAction): 32 device pixels at 150%
                    color: addKey.out("foreground", addArea.containsMouse ? Theme.text : Theme.subtext)
                    inkScale: addKey.out("contentScale", 1)
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
    Item {
        id: orderChip
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        width: height
        height: tabs.metrics.chipHeight + 2

        Hook { // the style's key: "固定的排在最前" on is its checked state
            id: orderKey
            anchors.fill: parent
            part: Theme.keySurface
            hovered: orderArea.containsMouse
            pressed: orderArea.pressed
            checked: tabs.clipboard.pinnedFirst
            flat: true
            corner: Theme.pillRadius(orderChip.height)
            role: "toolbar"
            dpr: tabs.dpr
        }

        Glyph {
            x: Theme.snap((parent.width - width) / 2, Screen.devicePixelRatio) // on whole device pixels
            y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio)
            glyph: "" // Sort
            size: tabs.metrics.dense ? 16 : 64 / 3 // as the header's pin button (RowAction): 32 device pixels at 150%
            color: orderKey.out("foreground", tabs.clipboard.pinnedFirst ? Theme.accentText : orderArea.containsMouse ? Theme.text : Theme.subtext)
            inkScale: orderKey.out("contentScale", 1)
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
