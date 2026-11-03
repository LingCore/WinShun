pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The shortcut hints over a capture (PixPin's corner list, made to follow
// what can be done now): CaptureSession.keyHints' rows as keycaps and what
// they do, in the bottom-left corner of the monitor, or the bottom-right one
// when the selection, its toolbar or its size are in the way there; gone
// when both are, and while a drag, a stroke or a text is under way. The
// pointer or the magnifier near it: see-through, so the picture shows; on
// its button it comes back for the click. ? (or F1) shows or hides it for
// this capture; the button turns the setting off (不再显示), or on (一直显示)
// when ? showed it with the setting off. Nothing but the button takes the
// mouse: a drag that starts in the corner still selects.
Item {
    id: hints
    objectName: "captureKeyHints"

    required property CaptureSession session
    required property AnnotationModel annot
    // The overlay's key handler: it keeps the keys after a click here.
    required property Item keyTarget
    property bool here: false // the monitor it shows on
    property bool suppressed: false // the toolbar's own key list is open
    property real dpr: 1
    // This window's logical px: what it must not cover (the selection, its
    // toolbar's rows, its size), what may lie over it see-through (the
    // magnifier), and the pointer (far off when it is on another monitor).
    property var blockers: []
    property var overlaps: []
    property point pointer: Qt.point(-100000, -100000)
    // Where it may go, logical px: the monitor without its taskbar.
    property rect area: Qt.rect(0, 0, width, height)

    // ---- geometry, logical px ----
    readonly property int margin: 16 // off the screen's edges and the taskbar
    readonly property int gap: 12 // off the selection and the toolbar
    readonly property int nearPointer: 32 // the pointer this near: see-through
    readonly property real seeThrough: 0.15
    readonly property int fadeMs: 150
    readonly property int fadeOutMs: 100
    readonly property int padX: 12
    readonly property int padY: 10
    readonly property int rowGap: 6
    readonly property int columnGap: 12 // keycaps to what they do
    readonly property int capHeight: 20
    readonly property int capPad: 6 // each side of a keycap's label
    readonly property int capGap: 4 // between the keycaps of a row, round the "/"
    readonly property int capFont: 12
    readonly property int footerGap: 8 // the rows to the line, the line to the footer
    readonly property int buttonHeight: 24
    readonly property int buttonPad: 8
    readonly property real edge: Theme.edge(dpr)

    function snap(v) { return Theme.snap(v, dpr) }
    function snapUp(v) { return Theme.snapUp(v, dpr) }

    // ---- what it shows ----
    readonly property var rows: session.keyHints
    // The rows last shown: kept while it fades out (the rows are none then).
    property var shownRows: []
    onRowsChanged: if (rows.length > 0) shownRows = rows
    readonly property var cells: {
        const list = []
        for (const id of shownRows) {
            const r = describe(id)
            list.push({ keys: r.keys })
            list.push({ text: r.text })
        }
        return list
    }

    // The overlay's own keys: R and A pick the area there, so the rectangle and the arrow are 1 and 3.
    readonly property var tool: annot && annot.tool >= 0 ? annot.tools[annot.tool] : null
    readonly property string toolKey: !tool ? "" : tool.letter === "R" ? "1" : tool.letter === "A" ? "3" : tool.letter
    // annot::Kind: 0 rectangle, 1 ellipse, 2 arrow, 3 line, 4 pen, 5 marker, 6 text, 7 number, 8 redaction.
    readonly property int toolKind: tool ? tool.kind : -1

    function describe(id) {
        switch (id) {
        case "wheel": return { keys: [qsTr("滚轮")], text: qsTr("扩大 / 缩小框选范围") }
        case "wasd": return { keys: ["WASD"], text: qsTr("微调光标 1 像素，加 Shift 10 像素") }
        case "recall": return { keys: ["R", "Shift+R"], text: qsTr("恢复最近选区") }
        case "history": return { keys: [",", "."], text: qsTr("翻看之前的截图") }
        case "ocr": return { keys: ["Shift+C"], text: qsTr("识别文字并复制") }
        case "quit": return { keys: ["Esc"], text: qsTr("退出截图") }
        case "copy": return { keys: ["Enter", "Ctrl+C"], text: qsTr("复制") }
        case "save": return { keys: ["Ctrl+S"], text: qsTr("另存为") }
        case "pin": return { keys: ["F3"], text: qsTr("贴到屏幕上") }
        case "arrows": return { keys: ["← ↑ ↓ →"], text: qsTr("移动选区，加 Shift 改大小") }
        case "long": return { keys: ["Ctrl+L", "Ctrl+R"], text: qsTr("长截图 / 录屏") }
        case "recalled": return { keys: ["R", "Shift+R"],
                                  text: qsTr("最近选区 %1/%2").arg(session.recalledSelection + 1).arg(session.selectionHistoryCount) }
        case "shift": return { keys: ["Shift"], text: toolKind <= 1 ? qsTr("按住画正方形、正圆")
                                                   : toolKind <= 3 ? qsTr("按住让线吸附 15°") : qsTr("按住画直线") }
        case "width": return { keys: ["[", "]"], text: toolKind === 6 || toolKind === 7 ? qsTr("字号小一档 / 大一档")
                                                   : toolKind === 8 ? qsTr("格子小一档 / 大一档") : qsTr("细一档 / 粗一档") }
        case "undo": return { keys: ["Ctrl+Z", "Ctrl+Y"], text: qsTr("撤销 / 重做") }
        case "delete": return { keys: ["Delete"], text: qsTr("删除选中的对象") }
        case "toolOff": return { keys: [toolKey], text: tool ? qsTr("关掉“%1”").arg(tool.name) : "" }
        case "hide": return { keys: ["H"], text: qsTr("隐藏工具条") }
        }
        return { keys: [], text: "" }
    }

    // ---- where, and how much of it ----
    // 0: bottom left, 1: bottom right; changed only while it is not seen.
    property int corner: 0
    function cornerX(c) { return c === 0 ? area.x + margin : area.x + area.width - margin - card.width }
    readonly property real cornerY: area.y + area.height - margin - card.height
    function cornerRect(c) {
        return Qt.rect(cornerX(c), cornerY, card.width, card.height)
    }
    function meets(a, b, grow) {
        return a.x - grow < b.x + b.width && b.x < a.x + a.width + grow
            && a.y - grow < b.y + b.height && b.y < a.y + a.height + grow
    }
    function blocked(c) {
        const r = cornerRect(c)
        for (const b of blockers) {
            if (b.width > 0 && b.height > 0 && meets(r, b, gap))
                return true
        }
        return false
    }
    function covered(c) {
        const r = cornerRect(c)
        if (meets(r, Qt.rect(pointer.x, pointer.y, 0, 0), nearPointer))
            return true
        for (const o of overlaps) {
            if (o.width > 0 && o.height > 0 && meets(r, o, 0))
                return true
        }
        return false
    }
    readonly property bool wanted: here && session.active && session.keyHintsShown && rows.length > 0 && !suppressed
    readonly property bool leftFree: !blocked(0)
    readonly property bool rightFree: !blocked(1)
    readonly property bool cornerFree: corner === 0 ? leftFree : rightFree
    // In view now (also see-through): the overlay leaves the recalled region's index to it.
    readonly property bool showing: wanted && cornerFree
    readonly property bool showsRecalled: showing && shownRows.indexOf("recalled") >= 0
    readonly property real target: !showing ? 0 : covered(corner) && !button.containsMouse ? seeThrough : 1

    // Out of sight: to the left corner when it is free, else the right one.
    function place() {
        if (card.opacity > 0)
            return
        if (leftFree)
            corner = 0
        else if (rightFree)
            corner = 1
    }
    onLeftFreeChanged: place()
    onRightFreeChanged: place()
    onWantedChanged: place()

    // A capture begins or ends: no fade from the last one (the session decides when they come).
    property bool instant: true
    Connections {
        target: hints.session
        function onActiveChanged() {
            hints.instant = true
            hints.shownRows = hints.rows
            hints.corner = hints.blocked(0) && !hints.blocked(1) ? 1 : 0
            Qt.callLater(() => { hints.instant = false })
        }
    }
    Component.onCompleted: {
        shownRows = rows
        Qt.callLater(() => { hints.instant = false })
    }

    Rectangle {
        id: card
        objectName: "captureKeyHintsCard"

        readonly property real innerWidth: Math.max(grid.width, footer.implicitWidth)

        x: hints.snap(hints.cornerX(hints.corner))
        y: hints.snap(hints.cornerY)
        width: hints.snapUp(innerWidth + 2 * hints.padX)
        height: hints.snapUp(grid.height + 2 * hints.footerGap + hints.edge + hints.capHeight + 2 * hints.padY)
        radius: Theme.controlRadius
        color: Theme.background
        border.width: hints.edge
        border.color: Theme.windowEdge
        opacity: hints.target
        visible: opacity > 0
        onOpacityChanged: if (opacity === 0) hints.place()

        // In at fadeMs, away quicker (a press, a key): out of the way before the hand moves on.
        Behavior on opacity {
            id: fade
            enabled: SystemTheme.animations && !hints.instant
            NumberAnimation { duration: fade.targetValue === 0 ? hints.fadeOutMs : hints.fadeMs; easing.type: Easing.OutCubic }
        }

        Grid {
            id: grid
            x: hints.snap(hints.padX)
            y: hints.snap(hints.padY)
            columns: 2
            columnSpacing: hints.columnGap
            rowSpacing: hints.rowGap

            Repeater {
                model: hints.cells

                delegate: Item {
                    id: cell

                    required property var modelData
                    readonly property bool isKeys: modelData.keys !== undefined

                    width: isKeys ? caps.implicitWidth : label.implicitWidth
                    height: hints.capHeight

                    Row {
                        id: caps
                        visible: cell.isKeys
                        spacing: hints.capGap

                        Repeater {
                            model: cell.isKeys ? cell.modelData.keys : []

                            delegate: Row {
                                id: alternative

                                required property int index
                                required property string modelData

                                spacing: hints.capGap

                                Text {
                                    visible: alternative.index > 0
                                    y: hints.snap((hints.capHeight - height) / 2)
                                    text: "/"
                                    color: Theme.faint
                                    font.pixelSize: hints.capFont
                                }
                                Cap { label: alternative.modelData }
                            }
                        }
                    }

                    Text {
                        id: label
                        visible: !cell.isKeys
                        y: hints.snap((hints.capHeight - height) / 2)
                        text: cell.isKeys ? "" : cell.modelData.text
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }
        }

        Rectangle { // the line above the footer, one device pixel
            x: grid.x
            y: hints.snap(grid.y + grid.height + hints.footerGap)
            width: hints.snap(card.innerWidth)
            height: hints.edge
            color: Theme.controlBorder
        }

        Item {
            id: footer

            // As tall as a row of keycaps; the button's hover reaches a little round its words.
            implicitWidth: toggleHint.width + hints.columnGap + buttonText.implicitWidth
            x: grid.x
            y: hints.snap(grid.y + grid.height + 2 * hints.footerGap + hints.edge)
            width: hints.snap(card.innerWidth)
            height: hints.capHeight

            Row {
                id: toggleHint
                spacing: hints.capGap + 2

                Cap { label: "?" }
                Text {
                    y: hints.snap((hints.capHeight - height) / 2)
                    text: qsTr("隐藏提示")
                    color: Theme.subtext
                    font.pixelSize: hints.capFont
                }
            }

            // 不再显示: the setting off, for this capture too; 一直显示 when ? showed it with the setting off.
            MouseArea {
                id: button
                objectName: "captureKeyHintsKeep"

                // Its words end where the line does.
                x: hints.snap(footer.width - width + hints.buttonPad)
                y: hints.snap((hints.capHeight - height) / 2)
                width: hints.snapUp(buttonText.implicitWidth + 2 * hints.buttonPad)
                height: hints.buttonHeight
                enabled: hints.showing
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton // a right press here is not Esc's step back
                cursorShape: Qt.ArrowCursor
                Accessible.role: Accessible.Button
                Accessible.name: buttonText.text
                onClicked: (mouse) => {
                    if (mouse.button !== Qt.LeftButton)
                        return
                    hints.session.keepKeyHints(!hints.session.keyHintsEnabled)
                    hints.keyTarget.forceActiveFocus()
                }

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.controlRadius
                    color: button.pressed ? Theme.controlPressed : button.containsMouse ? Theme.controlHover : "transparent"
                }
                Text {
                    id: buttonText
                    x: hints.snap((button.width - width) / 2)
                    y: hints.snap((button.height - height) / 2)
                    text: hints.session.keyHintsEnabled ? qsTr("不再显示") : qsTr("一直显示")
                    color: Theme.accent
                    font.pixelSize: hints.capFont
                }
            }
        }
    }

    // A key, raised a little: the controls' fill and edge, one device pixel.
    component Cap: Rectangle {
        id: cap

        property string label

        width: Math.max(height, hints.snapUp(capText.implicitWidth + 2 * hints.capPad))
        height: hints.capHeight
        radius: Theme.smallControlRadius
        color: Theme.control
        border.width: hints.edge
        border.color: Theme.controlBorder

        Text {
            id: capText
            x: hints.snap((cap.width - width) / 2)
            y: hints.snap((cap.height - height) / 2)
            text: cap.label
            color: Theme.text
            font.pixelSize: hints.capFont
        }
    }
}
