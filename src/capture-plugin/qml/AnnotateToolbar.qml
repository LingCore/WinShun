pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The capture toolbar (docs/design-capture-roadmap.md, 3.5). In the capture
// overlay it is two cards, left edges together, each as wide as its buttons:
// the common row (grip, select, rectangle, ellipse, arrow, pen, text, number, mosaic;
// undo, redo; pin, save; and at the right end cancel and copy, the check mark,
// the primary one) lies next to the selection; the other row (line, highlighter;
// OCR, QR; long, record, delay; the "?" that lists every key) on its far side.
// The editing window (EditWindow) has one row with all of them, and no
// session: its buttons then say what they want through action(), and the key
// list has the editor's keys. CaptureOverlay places the whole box and the
// style panel (AnnotateStylePanel) beside it.
Item {
    id: bar

    // The capture's; none in the editing window, which answers action().
    property CaptureSession session: null
    required property AnnotationModel annot
    property bool editor: false
    property bool movable: false
    property Item keyTarget: null
    readonly property bool dragging: toolbarDrag.active
    property bool ocrAvailable: session ? session.ocrAvailable : false
    // The action buttons the settings leave off (CaptureSession.hiddenButtons; the
    // editing window shows them all): their keys work all the same.
    readonly property var hidden: session ? session.hiddenButtons : []
    readonly property bool historyShown: shows("undo") || shows("redo")
    // The groups after the tools: undo and redo; pin and save; cancel and copy;
    // and in the second row OCR with QR, then long, record and delay.
    readonly property bool recognizeShown: shows("ocr") || shows("formula") || shows("qr") || shows("translate")
    readonly property bool modesShown: shows("long") || shows("record") || shows("delay")
    readonly property bool outputShown: shows("save") || (pinnable && shows("pin"))
    readonly property bool finishShown: shows("copy") || shows("cancel")
    // What the last button, the check mark, says in its tip (the editing
    // window: 完成 on a pin) and whether there is a 贴图 button (not when the picture is a pin already).
    property string copyText: qsTr("复制")
    property string sourceFileName: ""
    property bool pinnable: true
    // The toolbar's rows on the screen now (this one, and the style panel
    // when it shows), logical px in the window, and whether the toolbar
    // lies below the selection: the key list opens on the side away from it.
    property rect blockRect
    property bool opensDown: true
    property real dpr: 1
    // In a pin's editing window (GitHub #343) the window is the card: no cards of its own.
    property bool flat: false
    // What has screenArea() and prepareMenuWindow() (CaptureEditor): the key
    // list, and a tip with no room, go into windows of their own beside the
    // window, which is as short as the pin there.
    property QtObject popupHost: null
    readonly property bool keysDetached: popupHost !== null
    // A pin's editing window: Enter and Esc as they are there, and the pin's 编辑 key (CaptureKeyList).
    property bool pin: false
    property string pinEditKey: ""
    property bool pinEditFinishes: false

    // The key list: the rows both have, then the overlay's or the editor's
    // (CaptureKeyList, which the settings' 按键 tab lists too).
    readonly property CaptureKeyList keyList: CaptureKeyList {
        editor: bar.editor
        annot: bar.annot
        pin: bar.pin
        pinEditKey: bar.pinEditKey
        pinEditFinishes: bar.pinEditFinishes
        sourceFileName: bar.sourceFileName
    }
    readonly property var keyRows: keyList.rows

    // ---- geometry, logical px ----
    readonly property int pad: 4 // card edge to the buttons
    readonly property int spacing: 4 // between buttons
    readonly property int buttonHeight: 36 // as FlatButton
    readonly property int toolWidth: 32 // a tool, an action
    readonly property int helpWidth: 24 // the "?"
    readonly property int iconSize: 18 // drawn tool icons and glyphs
    readonly property int separatorWidth: 8 // a 1 device px line in the middle
    readonly property int separatorHeight: 20
    readonly property int rowGap: 4 // between the two cards
    readonly property int popupGap: 4 // the key list off the toolbar
    readonly property int popupMargin: 4 // the key list off the screen's edge
    readonly property int popupPadX: 16
    readonly property int popupPadY: 12
    readonly property int popupTitleGap: 8 // the key list's title to its rows
    readonly property int popupColumnGap: 16 // keys to what they do
    readonly property int popupRowGap: 6

    // A CaptureSession.Action asked for.
    signal action(int which)
    signal moveStarted()
    signal moveBy(real dx, real dy)
    signal keyForwarded(var event)
    signal keyReleased(var event)

    function act(which) {
        if (session)
            session.act(which)
        else
            action(which)
    }

    // The tool or button the mouse is on, for the one HoverTip.
    property var tip: null // item and tool change together, including when a drag crosses buttons
    property bool keysOpen: false
    // The window's WindowFrame, where it has one (the editor's, as `frame`;
    // the capture overlay has none). In a short editor window the open key
    // list goes up over the title bar: a popup to the frame, it takes the
    // presses there, not the drag area or the maximise button.
    readonly property var hostWindow: bar.Window.window
    onKeysOpenChanged: {
        const frame = hostWindow ? (hostWindow.frame ?? null) : null
        if (keysOpen && frame && !keysDetached)
            frame.addPopup(keysCard)
    }
    // A key list in a window of its own goes when this window is left.
    Connections {
        target: bar.hostWindow
        enabled: bar.keysDetached
        function onActiveChanged() {
            if (!bar.hostWindow.active)
                bar.keysOpen = false
        }
    }
    // Beside the window, on the side away from the picture (opensDown: the
    // toolbar lies below it), within the work area there; right-aligned with the toolbar.
    function placeKeysWindow(popup) {
        const host = hostWindow
        if (!popup || !host || !popupHost)
            return
        const at = host.contentItem.mapToGlobal(blockRect.x + blockRect.width, blockRect.y)
        const area = popupHost.screenArea(at)
        const below = host.y + host.height + popupGap
        const above = host.y - popupGap - popup.height
        const fitsBelow = below + popup.height <= area.y + area.height
        const fitsAbove = above >= area.y
        const y = opensDown ? (fitsBelow || !fitsAbove ? below : above) : (fitsAbove || !fitsBelow ? above : below)
        popup.x = Math.round(Math.max(area.x, Math.min(at.x - popup.width, area.x + area.width - popup.width)))
        popup.y = Math.round(Math.max(area.y, Math.min(y, area.y + area.height - popup.height)))
    }

    function shows(id) { return hidden.indexOf(id) < 0 }
    // Logical to whole device pixels.
    function snap(v) { return Math.round(v * dpr) / dpr }
    // Esc while the key list is open closes it (CaptureOverlay asks first).
    function closeKeys() {
        if (!keysOpen)
            return false
        keysOpen = false
        return true
    }
    function hover(item, hovered, tool) {
        if (hovered) {
            tip = {item: item, tool: tool}
        } else if (tip && tip.item === item) {
            tip = null
        }
    }

    // Two cards in the overlay, one in the editor. The first is the row next
    // to the selection (opensDown: the toolbar lies below it, so it is on top).
    readonly property bool twoRows: !editor
    readonly property bool flipped: twoRows && !opensDown
    // The cards as rects in this item, the first row first.
    readonly property var rowRects: twoRows ? [Qt.rect(first.x, first.y, first.width, first.height),
                                               Qt.rect(second.x, second.y, second.width, second.height)]
                                            : [Qt.rect(first.x, first.y, first.width, first.height)]

    implicitWidth: twoRows ? Math.max(first.width, second.width) : first.width
    implicitHeight: twoRows ? first.height + rowGap + second.height : first.height
    onVisibleChanged: if (!visible) keysOpen = false

    // The line, highlighter and spotlight (Kind::Line, Kind::Marker, Kind::Spotlight) are in the second row.
    function inSecondRow(kind) { return twoRows && (kind === 3 || kind === 5 || kind === 9) }

    Card {
        id: first
        objectName: "captureToolbarRow1"
        y: bar.flipped ? second.height + bar.rowGap : 0

        Item {
            id: grip
            objectName: "captureToolbarGrip"
            property string tipText: qsTr("按住拖动工具条")
            property string tipKeys: ""
            visible: bar.movable
            width: bar.helpWidth
            height: bar.buttonHeight
            activeFocusOnTab: true
            onVisibleChanged: if (!visible && activeFocus && bar.keyTarget) bar.keyTarget.forceActiveFocus()
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("移动工具条")
            Accessible.description: qsTr("按住拖动；聚焦后可用方向键移动，Shift 加速")
            Accessible.onPressAction: forceActiveFocus()

            KeyFace {
                id: gripFace
                hovered: gripHover.hovered
                pressed: toolbarDrag.active
            }
            Rectangle { // keyboard focus
                anchors.fill: parent
                visible: grip.activeFocus
                radius: Theme.controlRadius
                color: "transparent"
                border.width: 1
                border.color: Theme.text
            }

            Glyph {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: gripFace.out("contentOffsetY", 0)
                iconName: "dots-six-vertical"
                size: bar.iconSize
                color: gripFace.out("foreground", gripHover.hovered || toolbarDrag.active ? Theme.text : Theme.subtext)
            }
            HoverHandler {
                id: gripHover
                cursorShape: Qt.OpenHandCursor
                onHoveredChanged: bar.hover(grip, hovered, -1)
            }
            DragHandler {
                id: toolbarDrag
                target: null
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.ClosedHandCursor
                onActiveChanged: if (active) {
                    bar.keysOpen = false
                    bar.moveStarted()
                    bar.moveBy(centroid.scenePosition.x - centroid.scenePressPosition.x,
                               centroid.scenePosition.y - centroid.scenePressPosition.y)
                }
                onActiveTranslationChanged: if (active) {
                    bar.moveBy(centroid.scenePosition.x - centroid.scenePressPosition.x,
                               centroid.scenePosition.y - centroid.scenePressPosition.y)
                }
            }
            Keys.onPressed: (event) => {
                const dx = event.key === Qt.Key_Left ? -1 : event.key === Qt.Key_Right ? 1 : 0
                const dy = event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0
                if (!dx && !dy) {
                    if (event.key === Qt.Key_Shift && event.isAutoRepeat) {
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Escape && bar.closeKeys()) {
                        event.accepted = true
                        return
                    }
                    event.accepted = bar.session ? bar.session.keyPressed(event.key, event.modifiers, event.isAutoRepeat) : false
                    return
                }
                const step = event.modifiers & Qt.ShiftModifier ? 10 : 1
                bar.moveStarted()
                bar.moveBy(dx * step, dy * step)
                event.accepted = true
            }
            Keys.onReleased: (event) => {
                if (bar.session && !event.isAutoRepeat)
                    bar.session.keyReleased(event.key)
            }
        }

        SelectButton { objectName: "captureToolbarSelect" }

        Repeater {
            model: bar.annot ? bar.annot.tools : []
            delegate: ToolButton { visible: !bar.inSecondRow(modelData.kind) }
        }

        Separator { visible: bar.historyShown }

        ActionButton {
            id: undoButton
            visible: bar.shows("undo")
            glyph: String.fromCharCode(0xE7A7) // Undo
            name: qsTr("撤销")
            tipText: bar.annot && bar.annot.undoLabel.length > 0 ? qsTr("撤销 %1").arg(bar.annot.undoLabel) : qsTr("撤销")
            tipKeys: "Ctrl+Z"
            enabled: bar.annot !== null && bar.annot.canUndo
            onClicked: bar.annot.undo()
        }
        ActionButton {
            id: redoButton
            visible: bar.shows("redo")
            glyph: String.fromCharCode(0xE7A6) // Redo
            name: qsTr("重做")
            tipText: bar.annot && bar.annot.redoLabel.length > 0 ? qsTr("重做 %1").arg(bar.annot.redoLabel) : qsTr("重做")
            tipKeys: "Ctrl+Y"
            enabled: bar.annot !== null && bar.annot.canRedo
            onClicked: bar.annot.redo()
        }

        Separator { visible: bar.editor && (bar.shows("ocr") || bar.shows("formula")) }
        OcrButton { visible: bar.editor && bar.shows("ocr") }
        FormulaButton { visible: bar.editor && bar.shows("formula") }

        Separator { visible: bar.outputShown }

        ActionButton {
            id: pinButton
            visible: bar.pinnable && bar.shows("pin")
            glyph: String.fromCharCode(0xE718) // Pin
            name: qsTr("贴图")
            tipText: qsTr("贴图：留在屏幕上，可拖动、缩放")
            tipKeys: bar.annot ? bar.annot.pinKeyText : ""
            onClicked: bar.act(CaptureSession.Pin)
        }
        CaptureSaveButton {
            visible: bar.shows("save")
            keyText: bar.annot ? bar.annot.saveKeyText : ""
            direct: bar.annot ? bar.annot.saveDirect : false
            sourceFileName: bar.sourceFileName
            tipsEnabled: !bar.keysOpen && !bar.dragging
            onSaveRequested: bar.act(CaptureSession.Save)
            onSavePreviousRequested: bar.act(CaptureSession.SavePrevious)
            onSaveAsRequested: bar.act(CaptureSession.SaveAs)
            onKeyForwarded: (event) => bar.keyForwarded(event)
            onKeyReleased: (event) => bar.keyReleased(event)
        }

        Separator { visible: bar.editor }
        HelpButton { visible: bar.editor }

        Separator { visible: bar.finishShown }

        ActionButton {
            id: cancelButton
            visible: bar.shows("cancel")
            glyph: String.fromCharCode(0xE711) // Cancel
            name: qsTr("取消")
            tipText: qsTr("取消")
            tipKeys: "Esc"
            onClicked: bar.act(CaptureSession.Cancel)
        }
        ActionButton { // the way out that matters: accentFill, a white check
            id: copyButton
            visible: bar.shows("copy")
            primary: true
            glyph: String.fromCharCode(0xE73E) // CheckMark
            name: bar.copyText
            tipText: bar.copyText
            tipKeys: "Enter"
            onClicked: bar.act(CaptureSession.Copy)
        }
    }

    Card {
        id: second
        objectName: "captureToolbarRow2"
        visible: bar.twoRows
        y: bar.flipped ? 0 : first.height + bar.rowGap

        Repeater {
            model: bar.annot ? bar.annot.tools : []
            delegate: ToolButton { visible: bar.inSecondRow(modelData.kind) }
        }

        Separator { visible: bar.recognizeShown }

        OcrButton { visible: bar.shows("ocr") }
        FormulaButton { visible: bar.shows("formula") }
        ActionButton { // GitHub #191: a pin where the selection is, its words translated in place
            id: translateButton
            visible: bar.shows("translate")
            glyph: String.fromCharCode(0xE8C1) // Translate
            name: qsTr("翻译")
            tipText: qsTr("翻译：把图里的文字翻译好，贴在原来的位置；用“翻译”插件里设好的服务")
            onClicked: bar.act(CaptureSession.OcrTranslate)
        }
        ActionButton {
            id: qrButton
            visible: bar.shows("qr")
            glyph: String.fromCharCode(0xED14) // QRCode
            name: qsTr("二维码")
            tipText: qsTr("二维码：识别选区里的二维码，可复制文字或打开链接")
            tipKeys: "Q"
            onClicked: bar.act(CaptureSession.Qr)
        }

        Separator { visible: bar.modesShown }

        ActionButton {
            id: longButton
            visible: bar.shows("long")
            glyph: String.fromCharCode(0xEC8F)
            name: qsTr("长截图")
            tipText: qsTr("长截图：滚动着截下选区里更长的内容")
            tipKeys: bar.annot ? bar.annot.longKeyText : ""
            onClicked: bar.act(CaptureSession.Long)
        }
        ActionButton {
            id: recordButton
            visible: bar.shows("record")
            glyph: String.fromCharCode(0xE714)
            name: qsTr("录屏")
            tipText: qsTr("区域录屏：录制选区里的操作，保存为 MP4 或 GIF")
            tipKeys: bar.annot ? bar.annot.recordKeyText : ""
            onClicked: bar.act(CaptureSession.Record)
        }
        ActionButton {
            id: delayButton
            visible: bar.shows("delay")
            glyph: String.fromCharCode(0xE916) // Stopwatch
            name: qsTr("延时")
            tipText: qsTr("延时：关掉这次截图，倒计时后重新截（秒数在设置里选）")
            tipKeys: "Ctrl+Shift+D"
            onClicked: bar.act(CaptureSession.Delay)
        }

        Separator { visible: bar.shows("help") }

        HelpButton { objectName: "captureToolbarHelp" }
    }

    HoverTip {
        target: bar.visible && !bar.keysOpen && !bar.dragging && bar.tip ? bar.tip.item : null
        text: {
            const a = bar.annot
            const t = bar.tip
            if (t && t.tool >= 0 && a)
                return a.tools[t.tool].name
            return t ? t.item.tipText : ""
        }
        shortcut: {
            const a = bar.annot
            const tip = bar.tip
            if (tip && tip.tool >= 0 && a) {
                const t = a.tools[tip.tool]
                // The digit alone: R and A pick the area in a capture, and a letter may be the user's own key's (#455).
                if ((!bar.editor && (t.digit === 1 || t.digit === 3)) || t.letter.length === 0)
                    return String(t.digit)
                return t.digit > 0 ? t.letter + " / " + t.digit : t.letter
            }
            return tip ? tip.item.tipKeys : ""
        }
        host: bar.popupHost
    }

    // ---- the key list: on the window, above everything, until a click anywhere ----

    MouseArea { // a press anywhere else closes the list, and does nothing more
        parent: bar.Window.contentItem
        anchors.fill: parent
        z: 899
        visible: bar.keysOpen
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        cursorShape: Qt.ArrowCursor
        onPressed: bar.keysOpen = false
        onWheel: (wheel) => { wheel.accepted = true }
    }

    // The key list's own window, while it is open (keysDetached).
    Loader {
        id: keysWindow
        active: bar.keysOpen && bar.keysDetached
        sourceComponent: Window {
            objectName: "captureKeysWindow"
            flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
            color: Theme.menuBackground
            width: keysCard.width
            height: keysCard.height
            Component.onCompleted: { // placed first, then shown: no flash where it was made
                bar.popupHost.prepareMenuWindow(this)
                bar.placeKeysWindow(this)
                visible = true
            }
        }
    }

    Item {
        id: keysCard

        readonly property bool detached: bar.keysDetached && keysWindow.item !== null
        readonly property real roomBelow: parent ? parent.height - (bar.blockRect.y + bar.blockRect.height) : 0
        readonly property bool down: bar.opensDown ? roomBelow >= height + bar.popupGap + bar.popupMargin
                                                   : bar.blockRect.y < height + bar.popupGap + bar.popupMargin

        parent: detached ? keysWindow.item.contentItem : bar.Window.contentItem
        z: 900
        visible: bar.keysOpen
        width: Math.ceil((keysGrid.width + 2 * bar.popupPadX) / 4) * 4
        height: Math.ceil((keysTitle.height + bar.popupTitleGap + keysGrid.height + 2 * bar.popupPadY) / 4) * 4
        x: !parent || detached ? 0 : bar.snap(Math.max(bar.popupMargin, Math.min(bar.blockRect.x + bar.blockRect.width - width,
                                                                                  parent.width - width - bar.popupMargin)))
        y: !parent || detached ? 0 : bar.snap(Math.max(bar.popupMargin, Math.min(down ? bar.blockRect.y + bar.blockRect.height + bar.popupGap
                                                                                       : bar.blockRect.y - bar.popupGap - height,
                                                                                  parent.height - height - bar.popupMargin)))

        Hook { // the style's floating panel (Theme.menuFrame)
            anchors.fill: parent
            part: Theme.menuFrame
            extra: ({ floating: true })
            dpr: bar.dpr
        }

        MouseArea { // clicks on the list keep it open
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.ArrowCursor
            onWheel: (wheel) => { wheel.accepted = true }
        }

        Text {
            id: keysTitle
            x: bar.popupPadX
            y: bar.popupPadY
            text: qsTr("按键")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: Theme.fontStrong
        }

        Grid {
            id: keysGrid
            x: bar.popupPadX
            y: bar.popupPadY + keysTitle.height + bar.popupTitleGap
            columns: 2
            columnSpacing: bar.popupColumnGap
            rowSpacing: bar.popupRowGap

            Repeater {
                // [keys, what they do] flattened: even cells are keys.
                model: bar.keyRows
                delegate: Text {
                    required property int index
                    required property string modelData
                    text: modelData
                    color: index % 2 === 0 ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    font.weight: index % 2 === 0 ? Theme.fontStrong : Font.Normal
                }
            }
        }
    }

    // ---- the parts ----

    // A thin upright line between groups (the style's separator), on whole device pixels.
    component Separator: Item {
        width: bar.separatorWidth
        height: bar.buttonHeight

        Hook {
            y: bar.snap((bar.buttonHeight - bar.separatorHeight) / 2)
            width: bar.separatorWidth
            height: bar.separatorHeight
            part: Theme.separator
            orientation: Qt.Vertical
            dpr: bar.dpr
        }
    }

    // A toolbar key's face (Theme.keySurface): flat until the pointer is on it.
    // The marks on it take the colour the face gives (`foreground`).
    component KeyFace: Hook {
        anchors.fill: parent
        part: Theme.keySurface
        flat: true
        role: "toolbar"
        dpr: bar.dpr
    }

    // One card of the toolbar: its buttons go into the row inside.
    component Card: Item {
        default property alias content: cardRow.data

        implicitWidth: cardRow.width + 2 * bar.pad
        implicitHeight: bar.buttonHeight + 2 * bar.pad

        Hook { // the style's floating panel (Theme.menuFrame)
            anchors.fill: parent
            visible: !bar.flat
            part: Theme.menuFrame
            extra: ({ floating: true })
            dpr: bar.dpr
        }

        MouseArea { // presses between the buttons stay here, not on the picture beneath
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.ArrowCursor
            onWheel: (wheel) => { wheel.accepted = true }
        }

        Row {
            id: cardRow
            x: bar.pad
            y: bar.pad
            spacing: bar.spacing
        }
    }

    // OCR: without the models it still says why, in its tip and the OCR window.
    component OcrButton: ActionButton {
        glyph: String.fromCharCode(0xE8D2) // Font
        name: qsTr("识别文字")
        tipText: bar.ocrAvailable ? qsTr("识别文字：识别图里的文字，可复制或拿去搜索")
                                  : qsTr("识别文字：安装增强文字识别后继续，安装后可离线使用")
        onClicked: bar.act(CaptureSession.Ocr)
    }
    component FormulaButton: ActionButton {
        glyph: "ph:function"
        name: qsTr("公式")
        tipText: qsTr("公式识别：把图里的公式转成 LaTeX，可改、可复制为 Word 公式等")
        tipKeys: bar.editor ? "" : "Shift+F"
        onClicked: bar.act(CaptureSession.Formula)
    }

    // "?": every key, in a list by the toolbar.
    component HelpButton: Item {
        id: help

        property string tipText: qsTr("全部按键")
        property string tipKeys: ""

        visible: bar.shows("help")
        onVisibleChanged: if (!visible) {
            bar.keysOpen = false
            bar.hover(help, false, -1)
        }

        width: bar.helpWidth
        height: bar.buttonHeight
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("全部按键")

        KeyFace {
            id: helpFace
            hovered: helpArea.containsMouse
            pressed: helpArea.pressed
            current: bar.keysOpen
        }
        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: helpFace.out("contentOffsetY", 0)
            iconName: "question"
            size: bar.iconSize
            color: helpFace.out("foreground", helpArea.containsMouse || bar.keysOpen ? Theme.text : Theme.subtext)
        }
        MouseArea {
            id: helpArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: bar.keysOpen = !bar.keysOpen
            onContainsMouseChanged: bar.hover(help, containsMouse, -1)
        }
    }

    // An icon-only action: flat, as wide as a tool; primary: accentFill and a
    // white mark. The tip (name, keys) is read by the one HoverTip.
    component ActionButton: Item {
        id: action

        property string glyph
        property string name
        property string tipText
        property string tipKeys
        property bool primary: false
        signal clicked()

        width: bar.toolWidth
        height: bar.buttonHeight
        opacity: enabled ? 1 : actionFace.out("disabledOpacity", 0.45)
        Accessible.role: Accessible.Button
        Accessible.name: name
        Accessible.onPressAction: if (enabled) action.clicked()

        KeyFace {
            id: actionFace
            hovered: actionArea.containsMouse
            pressed: actionArea.pressed
            highlighted: action.primary
            flat: !action.primary // the primary one is a filled key at rest
        }
        Glyph {
            x: bar.snap((bar.toolWidth - bar.iconSize) / 2)
            y: bar.snap((bar.buttonHeight - bar.iconSize) / 2 + actionFace.out("contentOffsetY", 0))
            width: bar.iconSize
            height: bar.iconSize
            glyph: action.glyph
            size: bar.iconSize
            color: actionFace.out("foreground", action.primary ? Theme.accentText : Theme.text)
        }

        MouseArea {
            id: actionArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: action.clicked()
            onContainsMouseChanged: bar.hover(action, containsMouse, -1)
        }
    }

    // No tool: the mouse picks what was drawn, to move, change or delete it
    // (and moves the selection, crops the picture). The first of the tools and
    // on while none of them is, so the way back from drawing is in sight: Esc,
    // a right click and a second click on the tool do the same, but who would
    // know.
    component SelectButton: Item {
        id: select

        readonly property bool current: bar.annot !== null && bar.annot.tool < 0
        property string tipText: qsTr("选择：点选画好的标注，再移动、修改或删除")
        property string tipKeys: "Esc"

        width: bar.toolWidth
        height: bar.buttonHeight
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("选择")
        Accessible.onPressAction: if (bar.annot) bar.annot.tool = -1

        KeyFace {
            id: selectFace
            hovered: selectArea.containsMouse
            pressed: selectArea.pressed
            checked: select.current
        }
        Glyph {
            x: bar.snap((bar.toolWidth - bar.iconSize) / 2)
            y: bar.snap((bar.buttonHeight - bar.iconSize) / 2 + selectFace.out("contentOffsetY", 0))
            width: bar.iconSize
            height: bar.iconSize
            iconName: "cursor"
            size: bar.iconSize
            color: selectFace.out("foreground", select.current ? Theme.accentText : Theme.text)
        }

        MouseArea {
            id: selectArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: bar.annot.tool = -1
            onContainsMouseChanged: bar.hover(select, containsMouse, -1)
        }
    }

    // A tool: accentFill and a white icon while it is on; a click turns it
    // on, or off again when it is on (the key that opens it closes it too).
    component ToolButton: Item {
        id: tool

        required property int index
        required property var modelData
        readonly property bool current: bar.annot !== null && bar.annot.tool === index

        width: bar.toolWidth
        height: bar.buttonHeight
        Accessible.role: Accessible.Button
        Accessible.name: modelData.name

        KeyFace {
            id: toolFace
            hovered: toolArea.containsMouse
            pressed: toolArea.pressed
            checked: tool.current
        }
        AnnotateToolIcon {
            x: bar.snap((bar.toolWidth - bar.iconSize) / 2)
            y: bar.snap((bar.buttonHeight - bar.iconSize) / 2 + toolFace.out("contentOffsetY", 0))
            kind: tool.modelData.kind
            size: bar.iconSize
            color: toolFace.out("foreground", tool.current ? Theme.accentText : Theme.text)
        }

        MouseArea {
            id: toolArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: bar.annot.tool = tool.current ? -1 : tool.index
            onContainsMouseChanged: bar.hover(tool, containsMouse, tool.index)
        }
    }
}
