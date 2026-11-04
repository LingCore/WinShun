pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// On the left, how the list is chosen (Ctrl+S goes to the next): with nothing
// typed, what was opened last or the files written last; else best match
// first or by when last written, newest or oldest first (Ctrl+S goes round
// these), or from the 更多 menu by name, folder, type or size (#44). Beside it a word
// when there is something to say ("已复制路径"); keyboard hints on the
// right (the three used most; the info button after them lists the rest),
// then a button that clears the recent items (while there are some to clear),
// and in the panel at the very right its 列表 / 大图 switch. The divider above
// shows work going on. Dragging it moves the window.
Item {
    id: footer

    required property Launcher launcher
    required property WindowFrame frame

    signal galleryRequested(bool gallery) // the panel's view switch

    // The window's, read once by the host (GitHub #111).
    property real dpr: Screen.devicePixelRatio
    // The window's corners as drawn (WindowEdge.cornerRadius): the footer's
    // own bottom ones by them follow (an own frame rounds the window).
    property real cornerRadius: 0

    readonly property real hintsRight: info.visible ? info.x - 8
        : clearHistory.visible ? clearHistory.x - 8 : viewSwitch.visible ? viewSwitch.x - 10 : width - 18

    implicitHeight: Theme.footerHeight // 界面大小

    // The chrome's layer (as the search row's), on the window's base.
    Rectangle {
        anchors.fill: parent
        color: Theme.layer
        bottomLeftRadius: footer.cornerRadius
        bottomRightRadius: footer.cornerRadius
    }

    // The divider shows work only once it has lasted a moment, so brief
    // work does not make it blink.
    property bool showBusy: false
    readonly property bool busy: launcher.busy
    onBusyChanged: {
        if (busy) {
            busyDelay.start()
        } else {
            busyDelay.stop()
            showBusy = false
        }
    }
    Component.onCompleted: {
        showBusy = busy
        frame.addDragArea(footer)
    }
    Timer {
        id: busyDelay
        interval: 400
        onTriggered: footer.showBusy = footer.busy
    }

    // The divider, and while work goes on (indexing, reading files for their
    // contents) the bar along it: filled as far as a content scan has come,
    // or a light running along it while how far is not known. Done, it fills
    // up and fades.
    Item {
        id: progressLine

        readonly property bool running: footer.showBusy
        readonly property real fraction: footer.launcher.progress
        property bool determinate: false
        property real shown: 0 // of the width, filled

        function update() {
            if (running && footer.launcher.busy) {
                determinate = fraction >= 0
                if (determinate)
                    shown = fraction
            } else if (determinate) {
                shown = 1
            }
        }
        onRunningChanged: update()
        onFractionChanged: update()

        width: parent.width
        height: Theme.edge(footer.dpr) // the divider line and the progress on it, whole device pixels
        clip: true

        Rectangle { // where the chrome's layer meets the rows
            anchors.fill: parent
            color: Theme.layerEdge
        }

        Item {
            id: bar
            anchors.fill: parent
            opacity: progressLine.running ? 1 : 0
            Behavior on opacity {
                NumberAnimation { duration: progressLine.running ? 150 : 500; easing.type: Easing.OutCubic }
            }

            Rectangle {
                visible: progressLine.determinate
                width: Math.round(parent.width * progressLine.shown)
                height: parent.height
                color: Theme.accent
                Behavior on width {
                    enabled: bar.opacity > 0.99 // appearing: at once, not back from the last scan's end
                    NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
                }
            }

            Rectangle {
                id: sweep
                visible: !progressLine.determinate
                width: Math.round(parent.width * 0.3)
                height: parent.height
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0) }
                    GradientStop { position: 0.5; color: Theme.accent }
                    GradientStop { position: 1; color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0) }
                }

                NumberAnimation on x {
                    from: -sweep.width
                    to: progressLine.width
                    duration: 1400
                    loops: Animation.Infinite
                    running: sweep.visible && bar.opacity > 0 && footer.Window.active
                }
            }
        }
    }

    Text {
        id: status
        anchors.left: order.visible ? order.right : parent.left
        anchors.leftMargin: order.visible ? 14 : 18
        anchors.right: hints.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: footer.launcher.statusText
        textFormat: Text.PlainText // may name a file
        color: Theme.subtext
        font.pixelSize: Theme.uiBody
        elide: Text.ElideRight
    }

    // A few choices side by side, on the style's segmented part (role "sort").
    component Choices: Row {
        id: choices

        property alias labels: options.model
        property int current: 0
        property string glyph
        property string tip
        readonly property bool tipWanted: hover.hovered
        property int hoveredIndex: -1
        property int pressedIndex: -1
        // Each choice's place in the part, [{x, width}]: laid out here.
        property var segments: []
        function placeSegments() {
            const list = []
            for (let i = 0; i < options.count; ++i) {
                const chip = options.itemAt(i)
                if (chip)
                    list.push({ x: chips.x + chip.x, width: chip.width })
            }
            segments = list
        }

        signal activated(int index)

        spacing: 6

        function onPixel(v) { return Math.round(v * footer.dpr) / footer.dpr }

        HoverHandler { id: hover }

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            glyph: choices.glyph
            size: 16
            color: Theme.faint
        }

        Item {
            y: choices.onPixel((choices.height - height) / 2) // whole device pixels: centred, at 150 % it sat on half ones
            width: chips.implicitWidth + 4
            height: Theme.chipHeight

            Hook {
                id: track
                anchors.fill: parent
                part: Theme.segmented
                count: options.count
                currentIndex: choices.current
                hoveredIndex: choices.hoveredIndex
                pressedIndex: choices.pressedIndex
                segments: choices.segments
                role: "sort"
                dpr: footer.dpr
            }

            Row {
                id: chips
                x: 2
                y: 2
                spacing: 2
                onPositioningComplete: choices.placeSegments()

                Repeater {
                    id: options

                    delegate: Item {
                        id: chip

                        required property int index
                        required property string modelData
                        readonly property bool active: index === choices.current

                        width: Math.ceil(label.implicitWidth) + 16 // whole pixels: the labels after it stay sharp
                        height: Theme.chipHeight - 4
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData
                        Accessible.checked: active

                        Text {
                            id: label
                            x: choices.onPixel((chip.width - width) / 2)
                            y: choices.onPixel((chip.height - height) / 2 + track.call("labelOffsetY", [chip.index], 0))
                            text: chip.modelData
                            color: track.call("labelColor", [chip.index], chip.active || area.containsMouse ? Theme.text : Theme.subtext)
                            font.pixelSize: Theme.uiCaption
                            font.weight: track.call("labelWeight", [chip.index], chip.active ? Theme.fontStrong : Font.Normal)
                        }

                        MouseArea {
                            id: area
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onContainsMouseChanged: {
                                if (containsMouse)
                                    choices.hoveredIndex = chip.index
                                else if (choices.hoveredIndex === chip.index)
                                    choices.hoveredIndex = -1
                            }
                            onPressedChanged: {
                                if (pressed)
                                    choices.pressedIndex = chip.index
                                else if (choices.pressedIndex === chip.index)
                                    choices.pressedIndex = -1
                            }
                            onClicked: choices.activated(chip.index)
                        }
                    }
                }
            }
        }
    }

    // Nothing typed: which start list. Else the order; not for a plugin
    // command's list, which is in the plugin's.
    readonly property bool atStart: launcher.query.trim().length === 0 && launcher.scope !== Launcher.Content

    Choices {
        id: order
        // Not at the start with no start list (set so in the settings, GitHub #24).
        visible: footer.launcher.pluginKeyword.length === 0 && !footer.launcher.panel // the panel's columns order it
                 && !(footer.atStart && footer.launcher.startList === Launcher.NoStartList)
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        glyph: footer.atStart ? "\uE81C" : "\uE8CB" // History, Sort
        // Last chip: "更多", or the name of the sort chosen from its menu (#44).
        labels: footer.atStart ? [qsTr("最近打开"), qsTr("最近修改")]
                                 : [qsTr("最匹配"), qsTr("最新"), qsTr("最早"),
                                    footer.moreSorts ? footer.sortChipLabels[footer.launcher.sort - 3] : qsTr("更多")]
        current: footer.atStart ? footer.launcher.startList : footer.moreSorts ? 3 : footer.launcher.sort
        tip: footer.atStart ? qsTr("不输入时列出最近打开的，或最近修改的文件")
                              : qsTr("最匹配的在前，或按修改时间：最新的在前、最早的在前；“更多”里还能按名称、所在文件夹、类型、大小排")
        onActivated: (index) => {
            if (footer.atStart)
                footer.launcher.startList = index
            else if (index === 3)
                sortMenu.open = !sortMenu.open // opened by it, closed by it again
            else
                footer.launcher.sort = index
        }
        Component.onCompleted: footer.frame.addControl(order) // buttons, not part of the drag area
    }

    // The sorts the chips have no room for (Launcher.Sort from ByName on).
    readonly property var sortLabels: [qsTr("名称"), qsTr("所在文件夹"), qsTr("类型"), qsTr("大的在前"), qsTr("小的在前")]
    // On the chip the short word, so a long name does not push the key hints
    // out of the bar (#44); the menu has the full ones.
    readonly property var sortChipLabels: [qsTr("名称"), qsTr("文件夹"), qsTr("类型"), qsTr("大在前"), qsTr("小在前")]
    readonly property bool moreSorts: launcher.sort >= Launcher.ByName

    DropMenu {
        id: sortMenu
        anchor: order
        menuWidth: 150
        entries: footer.sortLabels.map((label, i) => ({
            text: label,
            glyph: footer.launcher.sort === i + Launcher.ByName ? "" : "" // CheckMark: the one in force
        }))
        onActivated: (index) => footer.launcher.sort = Launcher.ByName + index
    }

    Connections {
        target: footer.launcher
        function onSortChanged() { sortMenu.open = false } // Ctrl+S while it is open
    }

    Row {
        id: hints
        objectName: "footerKeyHints"
        visible: footer.launcher.footerHints
        width: visible ? implicitWidth : 0
        anchors.right: parent.right
        anchors.rightMargin: footer.width - footer.hintsRight
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14

        // What the keys do on the row the keyboard is on (Launcher.keyHints:
        // a plugin's row names its own actions, a row with no location has
        // no Ctrl+Enter, Tab completes the row that offers it). Plain words,
        // not "↵": a symbol missing from the UI font triggers system-wide
        // font fallback, which costs tens of MB.
        readonly property var keyHints: footer.launcher.keyHints

        // Hints give way to the status from the left, Enter's first (everyone
        // knows it), then Ctrl+Enter's; Tab's stays. Mostly there is none,
        // and all show. Worked out from the full widths, not the laid-out
        // ones, which depend on it.
        readonly property real statusNeeds: Math.ceil(statusMetrics.advanceWidth)
        readonly property real statusRoom: footer.hintsRight - (order.visible ? 16 + order.width + 14 : 18) - 12
        function roomWith(shown) { // the status's, with these hints shown
            const widths = shown.map(h => h.fullWidth).filter(w => w > 0)
            return statusRoom - widths.reduce((a, b) => a + b, 0) - hints.spacing * Math.max(0, widths.length - 1)
        }
        readonly property int given: statusNeeds <= roomWith([enterHint, secondHint, tabHint]) ? 0
                                     : statusNeeds <= roomWith([secondHint, tabHint]) ? 1 : 2

        TextMetrics {
            id: statusMetrics
            font.pixelSize: Theme.uiBody
            text: footer.launcher.statusText
        }

        KeyHint { id: enterHint; visible: hints.given < 1 && label.length > 0; keys: "Enter"; label: hints.keyHints.enter ?? "" }
        KeyHint { id: secondHint; visible: hints.given < 2 && label.length > 0; keys: hints.keyHints.secondKeys ?? ""; label: hints.keyHints.second ?? "" }
        KeyHint { id: tabHint; objectName: "footerTabHint"; visible: label.length > 0; keys: hints.keyHints.tabKeys || "Tab"; label: hints.keyHints.tab ?? "" }
    }

    // The launcher's other shortcuts, when it is clicked.
    ShortcutsButton {
        id: info
        objectName: "shortcutsButton"
        visible: footer.launcher.shortcutHelp

        readonly property string modifier: footer.launcher.numberKeys === "alt" ? "Alt" : "Ctrl"
        where: "launcher"
        context: ({
            previewKey: footer.launcher.previewHotkey,
            numberModifier: footer.launcher.numberKeys === "off" ? "" : modifier,
            // typed in a folder's window (GitHub #110): where switchTypingFolder takes it
            folderTyping: footer.launcher.typingFolder.length > 0
                          && footer.launcher.scope !== Launcher.Content && !footer.launcher.panel
        })

        host: footer.launcher
        frame: footer.frame
        anchors.right: clearHistory.visible ? clearHistory.left : viewSwitch.visible ? viewSwitch.left : parent.right
        anchors.rightMargin: clearHistory.visible ? 2 : viewSwitch.visible ? 10 : 6
        anchors.verticalCenter: parent.verticalCenter
    }

    // Two clicks, as a row's delete button: the first arms it.
    RowAction {
        id: clearHistory

        property bool armed: false

        visible: footer.launcher.canClearHistory
        role: "footer"
        anchors.right: viewSwitch.visible ? viewSwitch.left : parent.right
        anchors.rightMargin: viewSwitch.visible ? 8 : 6
        anchors.verticalCenter: parent.verticalCenter
        glyph: "\uE74D" // Delete
        tip: armed ? "" : qsTr("清除最近使用记录")
        text: armed ? qsTr("确认清除") : ""
        danger: armed
        onClicked: {
            if (armed) {
                armed = false
                footer.launcher.clearHistory()
            } else {
                armed = true
                disarm.restart()
            }
        }
        onVisibleChanged: armed = false
        Component.onCompleted: footer.frame.addControl(clearHistory) // a button, not part of the drag area

        Timer {
            id: disarm
            interval: 3000
            onTriggered: clearHistory.armed = false
        }
    }

    FileViewSwitch {
        id: viewSwitch
        visible: footer.launcher.panel
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        launcher: footer.launcher
        dpr: footer.dpr
        onModeRequested: gallery => footer.galleryRequested(gallery)
        Component.onCompleted: footer.frame.addControl(viewSwitch) // buttons, not part of the drag area
    }

    HoverTip {
        // Not while the sort menu is open: the tip would cover its last row (#44).
        target: clearHistory.tipWanted ? clearHistory : order.tipWanted && order.visible && !sortMenu.open ? order : null
        text: clearHistory.tipWanted ? clearHistory.tip : order.tip
        shortcut: clearHistory.tipWanted ? "" : "Ctrl+S"
    }
}
