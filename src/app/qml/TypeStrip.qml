pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Under the search box in 文件: the kinds of file it narrows to, as chips
// (不限, 文件夹, then 文档, 图片… as the settings have them), and + for the
// settings. Ctrl+Tab goes along them (Launcher.cycleFileType), a chip's tooltip
// lists its extensions; held and dragged, a chip after 不限 goes to another
// place (Launcher.moveFileChip, the settings' order). In 内容 the folders it looks in instead
// (Launcher.folderChips: 不限 and the favorite folders, or those a folder
// named among the words stands for), + for the favorites, and after them a
// few words on naming one (Launcher.folderHint); at the right end the
// content index's switch (内容索引). It opens and closes by its height (`shown`), the
// chips coming down from under the search box; the rows, footer and window
// edge follow its current height (see Main.qml). Too
// many for the width, they scroll, the current one kept in view and + after them. The rest
// of it remains client area, so a miss cannot move the window.
Item {
    id: strip

    required property Launcher launcher
    required property WindowFrame frame
    property bool shown: false
    property bool showKeywords: false

    readonly property int fullHeight: Theme.stripHeight // 界面大小
    readonly property int targetHeight: shown ? fullHeight : 0
    // Keep the departing chips while returning to 全部; its folderChips
    // are already empty when scopeChanged arrives.
    property bool folders: false
    // The chip that is current, as Launcher.chooseFileType (or chooseFolderChip) takes it.
    property var current: -1
    property var chips: []
    property string folderHint
    // The window's, read once by the host (GitHub #111).
    property real dpr: Screen.devicePixelRatio

    // Each kind's line icon (a Segoe code point Glyph draws with Phosphor), by
    // what its extensions mostly are: the kinds are the user's own, renamed,
    // reordered or new, so not by name or place.
    readonly property string folderGlyph: "" // Folder
    function typeGlyph(extensions) {
        const tally = {}
        let best = null
        for (const extension of extensions) {
            const kind = SearchKeys.fileKind("x." + extension)
            tally[kind] = (tally[kind] || 0) + 1
            if (best === null || tally[kind] > tally[best])
                best = kind
        }
        switch (best) {
        case "image": return "" // Picture
        case "video": return "" // Video
        case "audio": return "" // Volume
        case "zip": return "" // ZipFolder
        case "": return "" // ViewAll: programs
        case "ebook": return "" // ReadingMode
        case "code": return "" // Code
        default: return "" // Document
        }
    }
    // 不限, then the kinds with 文件夹 where the settings put it (Launcher.folderChipAt).
    // hueIndex (Hook): -1 不限, -2 文件夹 (and a folder chip in 内容), 0.. the kinds in this order.
    readonly property var fileChips: {
        const list = [{ type: -1, name: qsTr("不限"), tip: launcher.mergeFileScope ? qsTr("所有应用、文件和文件夹") : qsTr("所有文件和文件夹"), glyph: "", hue: -1 }]
        const types = launcher.fileTypes
        for (let i = 0; i < types.length; ++i) {
            const exts = types[i].extensions.split(" ")
            const listed = exts.slice(0, 10).map(e => "." + e).join(" ")
            list.push({ type: i, name: types[i].name, tip: (exts.length > 10 ? listed + " …" : listed)
                        + (types[i].keyword.length > 0 ? " · :" + types[i].keyword : ""),
                        glyph: typeGlyph(exts), hue: i, keyword: types[i].keyword })
        }
        list.splice(1 + Math.min(launcher.folderChipAt, types.length), 0,
                    { type: -2, name: qsTr("文件夹"), tip: qsTr("只找文件夹") + (launcher.folderKeyword.length > 0 ? " · :" + launcher.folderKeyword : ""),
                      glyph: folderGlyph, hue: -2, keyword: launcher.folderKeyword })
        return list
    }
    // 文件: how many of what is typed a chip would list (Launcher.fileTypeCounts:
    // all, folders, then the kinds), -1 for no number.
    function countOf(type): int {
        const counts = launcher.fileTypeCounts
        if (folders || apps || counts.length === 0 || typeof type !== "number")
            return -1
        const at = type === -1 ? 0 : type === -2 ? 1 : type + 2
        return at < counts.length ? counts[at] : -1
    }
    // 应用 (GitHub #296): which apps its grid shows, each with how many in its tooltip.
    property bool apps: false
    readonly property var appChips: {
        const counts = launcher.appCounts
        const count = (view) => " · " + qsTr("%1 个").arg(counts[view] ?? 0)
        return [{ type: Launcher.RunningApps, name: qsTr("运行"), tip: qsTr("正在运行的程序，Enter 切换过去") + count(Launcher.RunningApps), glyph: "ph:play-circle" },
                { type: Launcher.FrequentApps, name: qsTr("常用"), tip: qsTr("打开最多的应用") + count(Launcher.FrequentApps), glyph: "ph:clock-counter-clockwise" },
                { type: Launcher.FavoriteApps, name: qsTr("收藏"), tip: qsTr("收藏夹里的应用") + count(Launcher.FavoriteApps), glyph: "ph:star" },
                { type: Launcher.AllApps, name: qsTr("全部"), tip: qsTr("开始菜单里的所有应用") + count(Launcher.AllApps), glyph: "ph:squares-four" }]
    }
    onAppChipsChanged: updateChips()
    function updateChips() {
        if (!launcher || (launcher.scope === Launcher.All && !launcher.mergeFileScope))
            return
        apps = launcher.scope === Launcher.Apps
        if (apps) {
            folders = false
            current = launcher.appsView
            chips = appChips
            folderHint = ""
            return
        }
        folders = launcher.scope === Launcher.Content
        current = folders ? launcher.folderChip : launcher.scope === Launcher.All ? -1 : launcher.scope === Launcher.Folders ? -2 : launcher.fileType
        chips = folders ? launcher.folderChips : fileChips
        folderHint = launcher.folderHint
    }
    onFileChipsChanged: updateChips()
    Connections {
        target: strip.launcher
        function onScopeChanged() { strip.updateChips() }
        function onFolderChipsChanged() { strip.updateChips() }
        function onFileTypeChanged() { strip.updateChips() }
        function onAppsViewChanged() { strip.updateChips() }
    }

    // The chip the mouse rests on, for its tooltip.
    property Item hoveredChip: null
    property string hoveredTip

    function onPixel(v) { return Math.round(v * dpr) / dpr }

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
        enabled: strip.animated && SystemTheme.animations && strip.Window.window !== null && strip.Window.window.visible
        NumberAnimation { duration: 200; easing.type: Easing.OutCubic } // as fast as Windows' own
    }
    height: Math.round(opening)
    clip: true
    enabled: shown

    Component.onCompleted: {
        opening = targetHeight
        animated = true
        updateChips()
        frame.addControl(view)
        frame.addControl(edit)
        frame.addControl(indexSwitch) // in 内容 only: WindowFrame skips hidden items
    }

    // Too many for the width, the chips scroll by whole chips: the first one
    // shown starts at the left edge and only whole chips show, none cut off
    // (but while one is dragged). Kept so that the current chip is in view
    // (Ctrl+Tab past the end).
    property int firstChip: 0
    // The chips by place. Repeater.itemAt is no binding's dependency, so this
    // reads chipsMade too (counted up as the Repeater makes or drops a chip):
    // what reads the chips this way follows them when they are made anew (a
    // reorder, the settings) rather than keeping the old ones' places.
    property int chipsMade: 0
    function chipAt(index: int): Item {
        return chipsMade > 0 ? chipRepeater.itemAt(index) : null
    }
    property bool revealRequested: false
    property real slidePosition: 0
    readonly property bool scrolls: view.contentWidth > view.width
    // Whole logical pixels that are whole device pixels too (150%: even
    // ones): the words, icons and keys stay sharp and together as they move.
    function stepUp(v: real): real { return Math.ceil(v / scrollStep - 0.001) * scrollStep }
    readonly property int scrollStep: { let n = 1; while (n < 4 && Math.abs(n * dpr - Math.round(n * dpr)) > 0.001) ++n; return n }
    // Where the chips rest with chip `index` first.
    function stopAt(index: int): real {
        const chip = chipAt(index)
        return chip ? Math.floor(chip.x / scrollStep + 0.001) * scrollStep : 0
    }
    readonly property real restX: scrolls ? stopAt(firstChip) : 0
    // The room left after the last whole chip, shared out between the chips
    // shown: they then fill the width from edge to edge, + right after.
    readonly property real spreadGap: {
        if (!scrolls)
            return 0
        let last = null
        let count = 0
        for (let i = firstChip; i < chipRepeater.count; ++i) {
            const chip = chipAt(i)
            if (!chip || chip.x + chip.keyWidth > restX + view.width + 0.5)
                break
            last = chip
            ++count
        }
        // Whole device pixels each, so the gaps come out equal (the rest, under a pixel each, stays at the end).
        return last && count > 1 ? Math.floor(Math.max(0, restX + view.width - last.x - last.keyWidth) / (count - 1) * dpr) / dpr : 0
    }
    // The first chip shown with the last one in full at the end.
    function lastFirst(): int {
        for (let i = 0; i < chipRepeater.count; ++i)
            if (view.contentWidth - stopAt(i) <= view.width + 0.5)
                return i
        return Math.max(0, chipRepeater.count - 1)
    }
    // While the chips scroll their gaps go along with them, step for step
    // (slideProgress; each chip from where it was, see the delegate's shift),
    // so a chip's key, icon and words stay on the same whole pixels; chips
    // made anew (文件 ↔ 内容) take theirs at once.
    property bool moving: false
    signal scrollStarting()
    readonly property real slideProgress: slide.running && slide.to !== slide.from
        ? (slidePosition - slide.from) / (slide.to - slide.from) : 1
    function scrollTo(first: int, animated: bool) {
        // Already there or on the way: a wheel turned on and on past the
        // end must not start the slide over (it crept on, eased again and
        // again, a pixel back at each start).
        const target = scrolls ? Math.max(0, Math.min(lastFirst(), first)) : 0
        if (target === firstChip && (slide.running || view.contentX === restX))
            return
        scrollStarting() // the chips keep where they are shown now
        slide.stop()
        moving = animated && SystemTheme.animations
        firstChip = scrolls ? Math.max(0, Math.min(lastFirst(), first)) : 0
        const to = restX
        if (to === view.contentX) {
            moving = false
            return
        }
        if (!animated || !SystemTheme.animations) {
            view.contentX = to
            return
        }
        slide.from = view.contentX
        slide.to = to
        slide.start()
    }
    function scheduleReveal() {
        if (keepView)
            return
        revealRequested = true
        Qt.callLater(runReveal)
    }
    function runReveal() {
        if (revealRequested && shown)
            reveal()
    }
    function cancelReveal() {
        revealRequested = false
    }
    function reveal() {
        cancelReveal()
        scrollTo(firstShowing(chips.findIndex(c => c.type === current), firstChip), true)
    }
    // The first chip to show, from `first` on as few steps as can be, for chip `index` to be in full view.
    function firstShowing(index: int, first: int): int {
        const chip = chipAt(index)
        if (!chip)
            return first
        if (index < first)
            return index
        const right = chip.x + chip.keyWidth
        while (first < index && right - stopAt(first) > view.width + 0.5)
            ++first
        return first
    }
    onSlidePositionChanged: {
        if (slide.running)
            view.contentX = Math.round(slidePosition / scrollStep) * scrollStep
    }
    NumberAnimation {
        id: slide
        target: strip; property: "slidePosition"; duration: 150; easing.type: Easing.OutCubic
        onStopped: strip.moving = false
    }
    onCurrentChanged: scheduleReveal()
    onChipsChanged: {
        // Made anew (another scope, the settings) while one was dragged: that drag is over.
        settle.stop()
        settling = false
        dragFrom = -1
        dragTo = -1
        scheduleReveal()
    }
    onShowKeywordsChanged: scheduleReveal()

    // A chip after 不限 held and dragged along 文件's strip to another place:
    // the others make way (its width and a gap), it follows the pointer
    // inside the strip, and held at either end the chips scroll on
    // (edgeScroll). Let go, it glides into its place (settle), and the new
    // order goes to the settings (Launcher.moveFileChip); the chips are then
    // made anew where they already stand.
    readonly property bool reorderable: !folders && !apps
    property int dragFrom: -1 // the chip's index in chips; -1 for none
    property int dragTo: -1 // the index it takes when let go
    property real dragPointer: 0 // the pointer, in view
    property real dragGrab: 0 // where on the chip it was taken
    property bool settling: false
    property real settleLeft: 0
    property bool keepView: false // the chips made anew after a drop: no scrolling to the current one
    readonly property Item draggedChip: dragFrom >= 0 ? chipAt(dragFrom) : null
    readonly property real dragRoom: draggedChip ? draggedChip.width + row.spacing + spreadGap : 0
    // Where chip `index` rests in the row, and where the one dragged would rest at index `to`.
    function slotLeft(index: int): real {
        const chip = chipAt(index)
        return chip ? chip.x + chip.targetShift : 0
    }
    function restLeft(to: int): real {
        const chip = chipAt(to)
        return chip && draggedChip && to > dragFrom ? slotLeft(to) + chip.width - draggedChip.width : slotLeft(to)
    }
    // Its left edge in the row: under the pointer, after 不限, before the end, in view.
    readonly property real dragLeft: {
        if (!draggedChip)
            return 0
        const left = view.contentX + dragPointer - dragGrab
        const last = Math.min(restLeft(chipRepeater.count - 1), view.contentX + view.width - draggedChip.width)
        return Math.max(slotLeft(1), view.contentX, Math.min(left, last))
    }
    // The place of the last chip whose middle its leading edge has passed,
    // either way (by its middle, a chip wider than the next could not take
    // the place it is shown in).
    onDragLeftChanged: {
        if (!draggedChip || settling)
            return
        const right = dragLeft + draggedChip.width
        let to = dragFrom
        for (let i = dragFrom + 1; i < chipRepeater.count; ++i) {
            const chip = chipAt(i)
            if (!chip || right <= slotLeft(i) + chip.width / 2)
                break
            to = i
        }
        for (let i = dragFrom - 1; i >= 1; --i) {
            const chip = chipAt(i)
            if (!chip || dragLeft >= slotLeft(i) + chip.width / 2)
                break
            to = i
        }
        dragTo = to
    }
    function beginDrag(index: int, grab: real, pointer: real) {
        cancelReveal()
        dragGrab = grab
        dragPointer = pointer
        dragTo = index
        dragFrom = index
    }
    function endDrag(cancelled: bool) {
        if (dragFrom < 0 || settling)
            return
        if (cancelled)
            dragTo = dragFrom
        const to = restLeft(dragTo)
        settleLeft = dragLeft
        settling = true
        if (SystemTheme.animations && Math.abs(to - settleLeft) > 0.5) {
            settle.to = to
            settle.start()
        } else {
            Qt.callLater(finishDrag) // not under the handler that is ending: its chip goes with the drop
        }
    }
    function finishDrag() {
        if (!settling)
            return
        const from = dragFrom
        const to = dragTo
        settling = false
        dragFrom = -1
        dragTo = -1
        if (from >= 1 && to >= 1 && to !== from) {
            keepView = true
            launcher.moveFileChip(from - 1, to - 1) // the places after 不限
            row.forceLayout() // the new chips where they stand, before anything reads them
            keepView = false
        }
        // Whole chips again: from the first now wholly in view, with the one let go in view too.
        let first = 0
        while (first + 1 < chipRepeater.count && stopAt(first) < view.contentX - 0.5)
            ++first
        scrollTo(firstShowing(to, first), true)
    }
    NumberAnimation {
        id: settle
        target: strip; property: "settleLeft"; duration: 120; easing.type: Easing.OutCubic
        onFinished: strip.finishDrag()
    }
    Timer { // held at an end of the chips while dragging: they scroll on, a chip at a time
        id: edgeScroll
        readonly property int way: !strip.draggedChip || strip.settling || !strip.scrolls ? 0
            : strip.dragLeft <= view.contentX + 0.5 && strip.firstChip > 0 ? -1
            : strip.dragLeft + strip.draggedChip.width >= view.contentX + view.width - 0.5 && strip.firstChip < strip.lastFirst() ? 1
            : 0
        interval: 400
        repeat: true
        running: way !== 0
        onTriggered: strip.scrollTo(strip.firstChip + way, true)
    }

    // A mouse's wheel (or a touchpad's swipe) turns the chips a chip per
    // notch. Over the chips, taking only the wheel (no buttons, no hover).
    property real wheelRest: 0
    MouseArea {
        z: 5
        x: view.x
        y: view.y
        width: view.width
        height: view.height
        acceptedButtons: Qt.NoButton
        onWheel: (event) => {
            if (!strip.scrolls) {
                event.accepted = false
                return
            }
            strip.wheelRest += event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
            const steps = strip.wheelRest > 0 ? Math.floor(strip.wheelRest / 120) : Math.ceil(strip.wheelRest / 120)
            if (steps !== 0) {
                strip.wheelRest -= steps * 120
                strip.cancelReveal()
                strip.scrollTo(strip.firstChip - steps, true)
            }
        }
    }

    Flickable {
        id: view
        objectName: "searchTypeView"
        x: 12
        // Comes down from under the search box as the strip opens.
        y: strip.height - strip.fullHeight
        width: Math.min(contentWidth, strip.width - 24 - 4 - edit.width - hint.room - indexSwitch.room) // + stays in view
        height: strip.fullHeight - 6
        contentWidth: row.width
        contentHeight: height
        interactive: false // the wheel and Ctrl+Tab move it, by whole chips
        clip: true
        onWidthChanged: Qt.callLater(() => strip.scrollTo(strip.firstChip, false))
        opacity: strip.shown ? 1 : 0
        Behavior on opacity {
            enabled: strip.animated && SystemTheme.animations && strip.Window.window !== null && strip.Window.window.visible
            NumberAnimation { duration: 150 }
        }
        Row {
            id: row
            y: strip.onPixel((view.height - height) / 2)
            spacing: 4

            Repeater {
                id: chipRepeater
                model: strip.chips
                onItemAdded: ++strip.chipsMade
                onItemRemoved: ++strip.chipsMade

                // A chip on the style's chipSurface (role "type"); after 不限 a
                // short line parts it from the real filters (outside its hit area).
                delegate: Item {
                    id: chip
                    objectName: "searchTypeChip" + modelData.type

                    required property int index
                    required property var modelData
                    // 内容 narrowed to a kind of file (Launcher.folderChips' `kind`, GitHub #447):
                    // on, with 文件's icon and colour for it; a click takes it off. So is the
                    // chip of the files typed ("*.py"): `kind` -1 (no colour), its own `glyph`.
                    readonly property bool kindChip: strip.folders && modelData.kind !== undefined
                    // 不限 in 内容 is off while it is narrowed to kinds or to files typed (the
                    // chips before it, each with a `kind`): a click takes them off too.
                    readonly property bool active: kindChip || (!strip.folders && !strip.apps && modelData.type >= 0
                        ? strip.launcher.scope === Launcher.Files && strip.launcher.selectedFileTypes.includes(modelData.type)
                        : modelData.type === strip.current
                          && !(strip.folders && modelData.type === "" && strip.chips[0].kind !== undefined))
                    // 不限: in 内容 the chip without a folder's dot (Launcher.folderChips).
                    readonly property bool isAll: strip.apps || kindChip ? false : strip.folders ? !modelData.dot : modelData.type === -1
                    // The mode changes before the delegate model is replaced.
                    readonly property string glyph: kindChip ? (modelData.glyph ?? strip.typeGlyph((modelData.extensions ?? "").split(" ")))
                        : strip.folders ? (isAll ? "" : strip.folderGlyph) : (modelData.glyph ?? "")
                    readonly property int hue: kindChip ? modelData.kind : strip.folders ? (isAll ? -1 : -2) : (modelData.hue ?? -1)
                    // A line after 不限 (the first, or the first after the kinds), and after the last kind.
                    readonly property bool parted: index + 1 < strip.chips.length
                        && (kindChip ? strip.chips[index + 1].kind === undefined
                                     : isAll && (index === 0 || strip.chips[index - 1].kind !== undefined))
                    readonly property bool tipWanted: area.containsMouse && strip.dragFrom < 0
                    onTipWantedChanged: {
                        if (tipWanted) {
                            strip.hoveredChip = chip
                            strip.hoveredTip = modelData.tip
                        } else if (strip.hoveredChip === chip) {
                            strip.hoveredChip = null
                        }
                    }

                    // Whole pixels: a half one would leave the labels after it blurred.
                    // Whole device pixels too (150%: even), so the chips after it stay on them.
                    width: strip.stepUp(body.width + (parted ? row.spacing + 9 : 0))
                    readonly property real keyWidth: body.width
                    readonly property bool dragged: strip.dragFrom === index
                    // Making way for the chip dragged: its width and a gap, to the left or right.
                    readonly property real makeWay: {
                        const from = strip.dragFrom
                        const to = strip.dragTo
                        if (from < 0 || dragged)
                            return 0
                        if (from < to && index > from && index <= to)
                            return -strip.dragRoom
                        if (from > to && index >= to && index < from)
                            return strip.dragRoom
                        return 0
                    }
                    property real way: makeWay
                    Behavior on way {
                        enabled: SystemTheme.animations
                        NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                    }
                    z: dragged ? 2 : 0
                    // Whole or not at all where the chips scroll (strip.firstChip). While one
                    // is dragged all show, those making way past an edge cut off by the view,
                    // and whole chips again once it is let go (finishDrag).
                    readonly property bool whole: strip.dragFrom >= 0 || !strip.scrolls
                        || (x >= strip.restX - 0.5 && x + body.width <= strip.restX + view.width + 0.5)
                    opacity: whole ? 1 : 0
                    // Its share of strip.spreadGap; while the chips scroll, on its
                    // way there from where it was, with them. Whole device pixels.
                    readonly property real targetShift: strip.scrolls && index > strip.firstChip
                        ? (index - strip.firstChip) * strip.spreadGap : 0
                    property real fromShift: 0
                    readonly property real rawShift: strip.moving
                        ? fromShift + (targetShift - fromShift) * strip.slideProgress : targetShift
                    Connections {
                        target: strip
                        function onScrollStarting() { chip.fromShift = chip.rawShift }
                    }
                    transform: Translate {
                        x: chip.dragged ? strip.onPixel((strip.settling ? strip.settleLeft : strip.dragLeft) - chip.x)
                                        : strip.onPixel(chip.rawShift + chip.way)
                    }
                    Behavior on opacity {
                        enabled: SystemTheme.animations
                        NumberAnimation { duration: 120 }
                    }
                    height: Theme.typeChipHeight
                    Accessible.role: kindChip || (!strip.folders && !strip.apps && modelData.type >= 0) ? Accessible.CheckBox : Accessible.RadioButton
                    Accessible.name: badge.count >= 0 ? qsTr("%1，%2 个").arg(modelData.name).arg(badge.count) : modelData.name
                    Accessible.checked: active

                    Item {
                        id: body
                        // The style's own inset (a key with a lamp), 0 for no say.
                        readonly property real ownLead: surface.out("leadingInset", 0)
                        // The icon stands in a container (iconBox, an even square its ink never fills), and
                        // the chip is laid out by that: as far from the left edge as from the top and bottom,
                        // the name a fixed gap after it. Nothing here looks at the ink.
                        readonly property int iconBox: 2 * Math.round(height * 0.35) // 14, 16, 20, 22 for the four sizes of chip
                        readonly property int iconGap: 4
                        readonly property real iconInset: strip.onPixel((height - iconBox) / 2)
                        readonly property real lead: ownLead > 0 ? ownLead : chip.glyph.length > 0 ? iconInset : 12
                        readonly property real nameX: chip.glyph.length > 0 ? lead + iconBox + iconGap : lead
                        width: strip.stepUp(Math.ceil(nameX + labelSize.width + badge.room) + (chip.glyph.length > 0 ? 6 : 12))
                        height: parent.height

                        TextMetrics { id: labelSize; font: label.font; text: label.text }
                        TextMetrics { id: countSize; font: countLabel.font; text: countLabel.text }

                        Hook {
                            id: surface
                            anchors.fill: parent
                            part: Theme.chipSurface
                            checked: chip.active
                            hovered: area.containsMouse || chip.dragged
                            pressed: area.pressed || chip.dragged
                            isAll: chip.isAll
                            hueIndex: chip.hue
                            role: "type"
                            dpr: strip.dpr
                        }

                        Glyph {
                            id: icon
                            objectName: "searchTypeChipIcon"
                            visible: chip.glyph.length > 0
                            x: body.lead
                            y: strip.onPixel((body.height - height) / 2 + surface.out("contentOffsetY", 0))
                            glyph: chip.glyph
                            size: body.iconBox
                            color: surface.out("iconColor", chip.active && !chip.isAll ? Theme.accentText
                                                          : area.containsMouse ? Theme.subtext : Theme.faint)
                        }

                        Text {
                            id: label
                            objectName: "searchTypeChipName"
                            x: body.nameX
                            y: strip.onPixel((body.height - height) / 2 + surface.out("contentOffsetY", 0))
                            text: strip.showKeywords && (chip.modelData.keyword ?? "").length > 0 ? ":" + chip.modelData.keyword : chip.modelData.name
                            color: surface.out("foreground", chip.active && !chip.isAll ? Theme.accentText
                                                            : chip.active || area.containsMouse ? Theme.text : Theme.subtext)
                            font.pixelSize: Theme.uiCaption
                            font.weight: surface.out("fontWeight", chip.active ? Theme.fontStrong : Font.Normal)
                        }

                        // How many of what is typed this chip lists: a small tag after its
                        // name, in the name's own colour (each style's, on or off) over a
                        // faint wash of it.
                        Rectangle {
                            id: badge
                            objectName: "searchTypeChipCount"
                            readonly property int count: strip.countOf(chip.modelData.type)
                            // Its width with the gap before it; none without a number.
                            readonly property real room: count >= 0 ? 5 + width : 0
                            visible: count >= 0
                            // Whole device pixels, its edges and its number's (150%: no half ones).
                            x: strip.onPixel(label.x + Math.ceil(labelSize.width) + 5)
                            y: strip.onPixel((body.height - height) / 2 + surface.out("contentOffsetY", 0))
                            width: Theme.snapUp(Math.max(height, Math.ceil(countSize.width) + 8), strip.dpr)
                            height: 16
                            radius: Math.min(Theme.smallControlRadius, height / 2)
                            color: Qt.alpha(label.color, count > 0 ? 0.12 : 0.06)

                            Text {
                                id: countLabel
                                x: strip.onPixel((badge.width - width) / 2)
                                y: strip.onPixel((badge.height - height) / 2)
                                text: badge.count >= 0 ? strip.launcher.countLabel(badge.count) : ""
                                textFormat: Text.PlainText
                                color: label.color
                                opacity: badge.count > 0 ? 1 : 0.55 // none of that kind: there, but quiet
                                font.pixelSize: Theme.uiSmall
                            }
                        }

                        MouseArea {
                            id: area
                            objectName: "searchTypeChipArea"
                            anchors.fill: parent
                            // A chip cut off (hidden) takes no clicks. Only the area: the
                            // chip's own `enabled` changes a style's key, its width with it
                            // (tactile's checked key goes bold), which `whole` reads.
                            enabled: chip.whole
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (strip.apps)
                                    strip.launcher.appsView = chip.modelData.type
                                else if (strip.folders)
                                    strip.launcher.chooseFolderChip(chip.modelData.type)
                                else
                                    strip.launcher.toggleFileType(chip.modelData.type)
                            }

                            // Held and moved sideways past the drag distance: the chip goes
                            // with the pointer (strip.beginDrag) and the click is called off.
                            // In the area, so it sees the press before the area takes it.
                            DragHandler {
                                objectName: "searchTypeChipDrag"
                                target: null
                                yAxis.enabled: false
                                enabled: strip.reorderable && chip.modelData.type !== -1 && !strip.settling
                                // A little more than Windows' few pixels: a click that wavers stays a click.
                                dragThreshold: Math.max(8, Qt.styleHints.startDragDistance)
                                cursorShape: Qt.ClosedHandCursor
                                onActiveChanged: {
                                    if (active)
                                        strip.beginDrag(chip.index, chip.mapFromItem(null, centroid.scenePressPosition).x,
                                                        view.mapFromItem(null, centroid.scenePosition).x)
                                    else
                                        strip.endDrag(false)
                                }
                                onCentroidChanged: {
                                    if (active)
                                        strip.dragPointer = view.mapFromItem(null, centroid.scenePosition).x
                                }
                                onCanceled: strip.dragTo = strip.dragFrom // put back where it was
                            }
                        }
                    }

                    Loader { // 不限 | the filters
                        active: chip.parted
                        x: body.width + row.spacing
                        width: 9
                        height: 16
                        y: strip.onPixel((chip.height - height) / 2)
                        sourceComponent: Hook {
                            part: Theme.separator
                            orientation: Qt.Vertical
                            dpr: strip.dpr
                        }
                    }
                }
            }
        }
    }

    Item { // the kinds and their extensions, in the settings
        id: edit

        readonly property string tip: strip.folders ? qsTr("收藏常搜的文件夹，它们会出现在这里") : qsTr("添加或修改类型")
        readonly property bool tipWanted: editArea.containsMouse

        x: view.x + view.width + 4
        y: view.y + row.y
        width: strip.apps ? 0 : Theme.typeChipHeight // nothing to add to the apps' three
        height: Theme.typeChipHeight
        visible: !strip.apps
        opacity: view.opacity
        Accessible.role: Accessible.Button
        Accessible.name: tip

        Hook { // the style's key, flat until hovered, cornered as the chips
            id: editKey
            anchors.fill: parent
            part: Theme.keySurface
            hovered: editArea.containsMouse
            pressed: editArea.pressed
            flat: true
            corner: Theme.chipRadius
            role: "toolbar"
            dpr: strip.dpr
        }

        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: editKey.out("contentOffsetY", 0)
            inkScale: editKey.out("contentScale", 1)
            glyph: "\uE710" // Add
            size: 14
            color: editKey.out("foreground", editArea.containsMouse ? Theme.text : Theme.subtext)
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
        height: Theme.typeChipHeight
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
            dpr: strip.dpr
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
