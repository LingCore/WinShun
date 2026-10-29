pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Under the search box in 文件: the kinds of file it narrows to, as chips
// (不限, 文件夹, then 文档, 图片… as the settings have them), and + for the
// settings. Ctrl+Tab goes along them (Launcher.cycleFileType), a chip's tooltip
// lists its extensions. In 内容 the folders it looks in instead
// (Launcher.folderChips: 不限 and the favorite folders, or those a folder
// named among the words stands for), + for the favorites, and after them a
// few words on naming one (Launcher.folderHint); at the right end the
// content index's switch (内容索引). It opens and closes by its height (`shown`), the
// chips coming down from under the search box; the rows, footer and window
// edge follow its current height (see Main.qml). Too
// many for the width, they scroll, the current one kept in view and + after them. The rest
// of it drags the window.
Item {
    id: strip

    required property Launcher launcher
    required property WindowFrame frame
    property bool shown: false

    readonly property int fullHeight: Theme.stripHeight // 界面大小
    readonly property int targetHeight: shown ? fullHeight : 0
    // Keep the departing chips while returning to 全部; its folderChips
    // are already empty when scopeChanged arrives.
    property bool folders: false
    // The chip that is current, as Launcher.chooseFileType (or chooseFolderChip) takes it.
    property var current: -1
    property var chips: []
    property string folderHint
    // A dot for each, by place (any number of kinds of the user's own).
    readonly property var dotColors: ["#4F9DFF", "#3FBF7F", "#A57CFF", "#FF8A5C", "#C9953A", "#7B8CFF", "#2FBFB4", "#E07BD0"]
    readonly property var fileChips: {
        const list = [{ type: -1, name: qsTr("不限"), tip: qsTr("所有文件和文件夹"), dot: "" },
                      { type: -2, name: qsTr("文件夹"), tip: qsTr("只找文件夹") + (launcher.folderKeyword.length > 0 ? " · :" + launcher.folderKeyword : ""), dot: "#E8B84A" }]
        const types = launcher.fileTypes
        for (let i = 0; i < types.length; ++i) {
            const exts = types[i].extensions.split(" ")
            const listed = exts.slice(0, 10).map(e => "." + e).join(" ")
            list.push({ type: i, name: types[i].name, tip: (exts.length > 10 ? listed + " …" : listed)
                        + (types[i].keyword.length > 0 ? " · :" + types[i].keyword : ""),
                        dot: dotColors[i % dotColors.length] })
        }
        return list
    }
    function updateChips() {
        if (!launcher || launcher.scope === Launcher.All)
            return
        folders = launcher.scope === Launcher.Content
        current = folders ? launcher.folderChip : launcher.scope === Launcher.Folders ? -2 : launcher.fileType
        chips = folders ? launcher.folderChips : fileChips
        folderHint = launcher.folderHint
    }
    onFileChipsChanged: updateChips()
    Connections {
        target: strip.launcher
        function onScopeChanged() { strip.updateChips() }
        function onFolderChipsChanged() { strip.updateChips() }
        function onFileTypeChanged() { strip.updateChips() }
    }

    // The chip the mouse rests on, for its tooltip.
    property Item hoveredChip: null
    property string hoveredTip

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    // Whole logical pixels, also while it moves: the native window's height
    // and its contents use the same steps, without fractional layout slack.
    // Both directions use the current height, including a reversal halfway
    // through. Enable the Behavior only after the initial height is set.
    property real opening: 0
    property bool animated: false
    onTargetHeightChanged: {
        opening = targetHeight
        if (targetHeight > 0)
            scheduleReveal()
        else
            cancelReveal()
    }
    Behavior on opening {
        enabled: strip.animated && SystemTheme.animations
        NumberAnimation { duration: 200; easing.type: Easing.OutCubic } // as fast as Windows' own
    }
    height: Math.round(opening)
    clip: true
    enabled: shown

    Component.onCompleted: {
        opening = targetHeight
        animated = true
        updateChips()
        frame.addDragArea(strip)
        frame.addControl(view)
        frame.addControl(edit)
        frame.addControl(indexSwitch) // in 内容 only: WindowFrame skips hidden items
    }

    // Keeps the current chip in view (Tab past the right edge).
    property bool revealRequested: false
    property bool writingScroll: false
    property real slidePosition: 0
    function scheduleReveal() {
        revealRequested = true
        Qt.callLater(runReveal)
    }
    function runReveal() {
        if (revealRequested && shown)
            reveal()
    }
    function cancelReveal() {
        revealRequested = false
        slide.stop()
    }
    function reveal() {
        cancelReveal()
        const chip = chipRepeater.itemAt(chips.findIndex(c => c.type === current))
        if (!chip || view.contentWidth <= view.width)
            return
        const left = chip.x - 8
        const right = chip.x + chip.width + 8
        let to = view.contentX
        if (left < view.contentX)
            to = Math.max(0, left)
        else if (right > view.contentX + view.width)
            to = Math.min(view.contentWidth - view.width, right - view.width)
        if (to !== view.contentX) {
            view.cancelFlick()
            if (!SystemTheme.animations) {
                view.contentX = to
                return
            }
            slide.from = view.contentX
            slide.to = to
            slide.restart()
        }
    }
    onSlidePositionChanged: {
        if (slide.running) {
            writingScroll = true
            view.contentX = slidePosition
            writingScroll = false
        }
    }
    NumberAnimation { id: slide; target: strip; property: "slidePosition"; duration: 150; easing.type: Easing.OutCubic }
    onCurrentChanged: scheduleReveal()
    onChipsChanged: scheduleReveal()

    Flickable {
        id: view
        x: 12
        // Comes down from under the search box as the strip opens.
        y: strip.height - strip.fullHeight
        width: Math.min(contentWidth, strip.width - 24 - 4 - edit.width - hint.room - indexSwitch.room) // + stays in view
        height: strip.fullHeight - 6
        contentWidth: row.width
        contentHeight: height
        interactive: contentWidth > width
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        onContentXChanged: if (!strip.writingScroll) strip.cancelReveal()
        onDraggingChanged: if (dragging) strip.cancelReveal()
        onMovementStarted: strip.cancelReveal()
        opacity: strip.shown ? 1 : 0
        Behavior on opacity {
            enabled: strip.animated && SystemTheme.animations
            NumberAnimation { duration: 150 }
        }
        WheelHandler {
            target: null
            orientation: Qt.Horizontal
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            blocking: false // observe, then let Flickable handle the event
            onWheel: strip.cancelReveal()
        }

        Row {
            id: row
            y: strip.onPixel((view.height - height) / 2)
            spacing: 4

            Repeater {
                id: chipRepeater
                model: strip.chips

                delegate: Rectangle {
                    id: chip

                    required property var modelData
                    readonly property bool active: !strip.folders && modelData.type >= 0
                        ? strip.launcher.scope !== Launcher.Folders && strip.launcher.selectedFileTypes.includes(modelData.type)
                        : modelData.type === strip.current
                    readonly property bool tipWanted: area.containsMouse
                    onTipWantedChanged: {
                        if (tipWanted) {
                            strip.hoveredChip = chip
                            strip.hoveredTip = modelData.tip
                        } else if (strip.hoveredChip === chip) {
                            strip.hoveredChip = null
                        }
                    }

                    // Whole pixels: a half one would leave the labels after it blurred.
                    width: (modelData.dot.length > 0 ? 10 + 8 + 6 : 12) + Math.ceil(label.implicitWidth) + 12
                    height: Theme.chipHeight
                    radius: Theme.pillRadius(height)
                    color: active ? Theme.chipActive : area.containsMouse ? Theme.chipHover : "transparent"
                    border.width: active ? 1 : 0
                    border.color: Theme.chipBorder
                    Accessible.role: !strip.folders && modelData.type >= 0 ? Accessible.CheckBox : Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.checked: active

                    Rectangle {
                        visible: chip.modelData.dot.length > 0
                        x: 10
                        y: strip.onPixel((chip.height - height) / 2)
                        width: 8
                        height: 8
                        radius: Theme.progressRadius
                        color: chip.modelData.dot
                    }

                    Text {
                        id: label
                        x: chip.modelData.dot.length > 0 ? 24 : 12
                        y: strip.onPixel((chip.height - height) / 2)
                        text: chip.modelData.name
                        color: chip.active || area.containsMouse ? Theme.text : Theme.subtext
                        font.pixelSize: Theme.uiCaption
                        font.weight: chip.active ? Font.DemiBold : Font.Normal
                    }

                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: strip.folders ? strip.launcher.chooseFolderChip(chip.modelData.type)
                                                 : strip.launcher.toggleFileType(chip.modelData.type)
                    }
                }
            }
        }
    }

    Rectangle { // the kinds and their extensions, in the settings
        id: edit

        readonly property string tip: strip.folders ? qsTr("收藏常搜的文件夹，它们会出现在这里") : qsTr("添加或修改类型")
        readonly property bool tipWanted: editArea.containsMouse

        x: view.x + view.width + 4
        y: view.y + row.y
        width: Theme.chipHeight
        height: Theme.chipHeight
        radius: Theme.pillRadius(height)
        opacity: view.opacity
        color: editArea.pressed ? Theme.controlPressed : editArea.containsMouse ? Theme.chipHover : "transparent"
        Accessible.role: Accessible.Button
        Accessible.name: tip

        Glyph {
            anchors.centerIn: parent
            glyph: "\uE710" // Add
            size: 12
            color: editArea.containsMouse ? Theme.text : Theme.subtext
        }

        MouseArea {
            id: editArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: strip.folders ? strip.launcher.editFolderChips() : strip.launcher.editFileTypes()
        }
    }

    Text { // 内容: how to name a folder, where there is room for it after +
        id: hint
        // Its room, whole pixels; none when it would push the chips into scrolling.
        readonly property int room: shown ? Math.ceil(implicitWidth) + 8 : 0
        readonly property bool shown: strip.folders && text.length > 0
                                      && view.contentWidth + Math.ceil(implicitWidth) + 8
                                             <= strip.width - 24 - 4 - edit.width - indexSwitch.room
        x: edit.x + edit.width + 8
        y: strip.onPixel(edit.y + (edit.height - height) / 2)
        visible: shown
        opacity: view.opacity
        text: strip.folderHint
        color: Theme.subtext
        font.pixelSize: Theme.uiCaption
    }

    Item { // 内容: the content index on or off (Settings::contentIndex), at the right end; label and track one target
        id: indexSwitch

        readonly property bool indexOn: strip.launcher.contentIndex
        readonly property string tip: indexOn ? qsTr("在后台记下文件里的文字，内容搜索快得多。关掉会清空本机文件的内容索引")
                                         : qsTr("打开后在后台记下文件里的文字，内容搜索快得多。只用于 NTFS 磁盘，约占一两百 MB。网络位置在设置里另有开关")
        readonly property bool tipWanted: indexHover.hovered || indexToggle.hovered
        // Its room with the gap before it, whole pixels: the chips scroll and
        // the hint goes before they reach it.
        readonly property int room: visible ? width + 12 : 0
        function toggle() { strip.launcher.requestContentIndex(!indexOn) }

        x: strip.width - 12 - width
        y: edit.y
        width: indexToggle.x + indexToggle.width
        height: Theme.chipHeight
        visible: strip.folders
        opacity: view.opacity
        Accessible.role: Accessible.CheckBox
        Accessible.name: qsTr("内容索引")
        Accessible.description: tip
        Accessible.checkable: true
        Accessible.checked: indexOn
        Accessible.onPressAction: toggle()
        Accessible.onToggleAction: toggle()

        // A handler: it sees the pointer over the track too, whose own area takes it.
        HoverHandler { id: indexHover }

        MouseArea { // the label and the gap; the track has its own
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: indexSwitch.toggle()
        }

        Text {
            id: indexLabel
            y: strip.onPixel((indexSwitch.height - height) / 2)
            text: qsTr("内容索引")
            color: indexSwitch.tipWanted ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiCaption
        }

        ToggleSwitch {
            id: indexToggle
            x: Math.ceil(indexLabel.implicitWidth) + 8
            height: indexSwitch.height // the track (20) centred on the chips' row
            word: false // the label says what it is
            activeFocusOnTab: false // Tab in the search box changes the scope
            checked: indexSwitch.indexOn
            onToggled: (on) => strip.launcher.requestContentIndex(on)
        }
    }

    HoverTip {
        target: !strip.shown ? null : indexSwitch.tipWanted ? indexSwitch : edit.tipWanted ? edit : strip.hoveredChip
        text: indexSwitch.tipWanted ? indexSwitch.tip : edit.tipWanted ? edit.tip : strip.hoveredTip
    }
}
