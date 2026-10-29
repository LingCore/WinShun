pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The capture toolbar's first row (docs/design-capture-roadmap.md, 3.5): the
// nine annotation tools in AnnotationModel.tools' order, undo and redo, then
// the capture's own actions as icons (long, delay, QR, OCR; pin, save), a "?"
// that lists every key, and at the right end cancel and copy (the check mark,
// the primary one). A card of its own: CaptureOverlay
// places it and the style panel (AnnotateStylePanel) beside it. The editing
// window (EditWindow) uses it too, without a session: its buttons then say
// what they want through action(), and the key list has the editor's keys.
Rectangle {
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
    // The groups after the tools: undo and redo; long, delay, QR, OCR; pin and save; cancel and copy.
    readonly property bool toolsShown: shows("ocr") || (!editor && (shows("long") || shows("record") || shows("qr") || shows("delay")))
    readonly property bool outputShown: shows("save") || (pinnable && shows("pin"))
    readonly property bool finishShown: shows("copy") || shows("cancel")
    // What the last button, the check mark, says in its tip (the editing
    // window: 完成 on a pin) and whether there is a 贴图 button (not when the picture is a pin already).
    property string copyText: qsTr("复制")
    property bool pinnable: true
    // The toolbar's rows on the screen now (this one, and the style panel
    // when it shows), logical px in the window, and whether the toolbar
    // lies below the selection: the key list opens on the side away from it.
    property rect blockRect
    property bool opensDown: true
    property real dpr: 1

    // The key list: the rows both have, then the overlay's or the editor's.
    readonly property var commonRows: [
        qsTr("R E A L P M T N B 或 1–9"), qsTr("选工具：矩形、椭圆、箭头、直线、画笔、荧光笔、文字、序号、打码"),
        qsTr("工具键再按一次"), qsTr("关掉工具"),
        "Ctrl+Z", qsTr("撤销"),
        "Ctrl+Y、Ctrl+Shift+Z", qsTr("重做"),
        "Delete、Backspace", qsTr("删除选中的对象"),
        "[  ]", qsTr("粗细、字号、格子小一档 / 大一档"),
        "Shift+B", qsTr("智能打码：自动找出手机号、证件号、卡号、邮箱、密钥，一次全部打码"),
        "Ctrl+[  Ctrl+]", qsTr("下移一层 / 上移一层，加 Shift 到底 / 到顶"),
        qsTr("画的时候按住 Shift"), qsTr("正方形、正圆；线吸附 15°；画笔、荧光笔画直线"),
        qsTr("双击"), qsTr("文字：改文字；序号：改号，序号旁的字：改字"),
        "Ctrl+Enter、Esc", qsTr("写完文字（Enter 换行；序号旁的字 Enter 就写完）"),
        "Ctrl+C、Enter", qsTr("复制"),
        "Ctrl+S", qsTr("保存"),
        "F3", qsTr("贴图")
    ]
    readonly property var overlayRows: [
        qsTr("滚轮"), qsTr("有工具或选中对象时改粗细"),
        qsTr("方向键"), qsTr("挪动选中的对象，按住 Shift 一次 10 像素；没有选中时微调选区"),
        "H", qsTr("隐藏 / 显示工具条"),
        "C、Shift", qsTr("复制颜色并结束截图；单按 Shift 换颜色格式（HEX、RGB、HSL）"),
        "Shift+C", qsTr("识别选区里的文字并复制"),
        "Shift+R", qsTr("还没选区时：用上一次的选区"),
        "Q", qsTr("识别选区里的二维码"),
        "D", qsTr("延时截图：关掉这次，倒计时后再截"),
        "Ctrl+L", qsTr("长截图：选区里的内容一边滚动一边截"),
        "Ctrl+R", qsTr("区域录屏：把选区里的操作录成 MP4 或 GIF"),
        qsTr("Esc、右键"), qsTr("退一步：写完文字、放弃标注、关闭截图")
    ]
    readonly property var editorRows: [
        qsTr("方向键"), qsTr("挪动选中的对象，按住 Shift 一次 10 像素；没有选中时滚动图片"),
        qsTr("Shift+点击、拖空白处"), qsTr("多选；Ctrl+A 全选，Ctrl+D 复制一份"),
        "I", qsTr("吸管：点图上的一点，取它的颜色"),
        qsTr("拖图片的边"), qsTr("裁剪（没有工具时；有工具时从图外拖）"),
        qsTr("滚轮"), qsTr("上下滚动，Shift 左右；Alt+滚轮改粗细"),
        qsTr("Ctrl+滚轮、Ctrl+加号 / 减号"), qsTr("缩放"),
        "Z", qsTr("适合窗口 / 100%"),
        qsTr("空格 + 拖动，或中键拖动"), qsTr("平移"),
        qsTr("Esc"), qsTr("写完文字；有改动时放弃全部（Ctrl+Z 恢复）；没有改动时关闭")
    ]
    readonly property var keyRows: commonRows.concat(editor ? editorRows : overlayRows)

    // ---- geometry, logical px ----
    readonly property int pad: 4 // card edge to the buttons
    readonly property int spacing: 4 // between buttons
    readonly property int buttonHeight: 36 // as FlatButton
    readonly property int toolWidth: 32 // a tool, an action
    readonly property int helpWidth: 24 // the "?"
    readonly property int iconSize: 16 // drawn tool icons and glyphs
    readonly property int separatorWidth: 8 // a 1 device px line in the middle
    readonly property int separatorHeight: 20
    readonly property int popupGap: 4 // the key list off the toolbar
    readonly property int popupMargin: 4 // the key list off the screen's edge
    readonly property int popupPadX: 16
    readonly property int popupPadY: 12
    readonly property int popupTitleGap: 8 // the key list's title to its rows
    readonly property int popupColumnGap: 16 // keys to what they do
    readonly property int popupRowGap: 6
    // The drawn tool icons, in sixteenths of the icon (scaled to its device
    // pixels, then rounded to whole ones): the inset of the arrow, line and T;
    // the arrowhead's sides; the numbered disc's ring inset, the 1's top and
    // flag; the mosaic's inset.
    readonly property real iconInset: 2.5
    readonly property real arrowHead: 5.5
    readonly property real ringInset: 1.5
    readonly property real oneTop: 4.5
    readonly property real oneFlag: 2
    readonly property real mosaicInset: 2
    // Their stroke, device px: 1 up to 20 device px across (100 %, 125 %),
    // 1.5 up to 28 (150 %, 175 %), 2 above, as Fluent icons step it.
    readonly property int thinUpTo: 20
    readonly property int midUpTo: 28

    // A CaptureSession.Action asked for.
    signal action(int which)
    signal moveStarted()
    signal moveBy(real dx, real dy)

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
        if (keysOpen && frame)
            frame.addPopup(keysCard)
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

    implicitWidth: row.width + 2 * pad
    implicitHeight: buttonHeight + 2 * pad
    radius: Theme.controlRadius
    color: Theme.background
    border.width: 1
    border.color: Theme.windowEdge
    onVisibleChanged: if (!visible) keysOpen = false

    MouseArea { // presses between the buttons stay here, not on the picture beneath
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.ArrowCursor
        onWheel: (wheel) => { wheel.accepted = true }
    }

    Row {
        id: row
        x: bar.pad
        y: bar.pad
        spacing: bar.spacing

        Rectangle {
            id: grip
            objectName: "captureToolbarGrip"
            property string tipText: qsTr("按住拖动工具条")
            property string tipKeys: ""
            visible: bar.movable
            width: bar.helpWidth
            height: bar.buttonHeight
            radius: Theme.controlRadius
            color: toolbarDrag.active ? Theme.controlPressed : gripHover.hovered ? Theme.controlHover : "transparent"
            activeFocusOnTab: true
            onVisibleChanged: if (!visible && activeFocus && bar.keyTarget) bar.keyTarget.forceActiveFocus()
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("移动工具条")
            Accessible.description: qsTr("按住拖动；聚焦后可用方向键移动，Shift 加速")
            Accessible.onPressAction: forceActiveFocus()
            border.width: activeFocus ? 1 : 0
            border.color: Theme.text

            Column {
                anchors.centerIn: parent
                spacing: 3
                Repeater {
                    model: 3
                    Rectangle {
                        width: 3
                        height: 3
                        radius: 1.5
                        color: gripHover.hovered || toolbarDrag.active ? Theme.text : Theme.subtext
                    }
                }
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
                    event.accepted = bar.session ? bar.session.keyPressed(event.key, event.modifiers) : false
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

        Repeater {
            model: bar.annot ? bar.annot.tools : []
            delegate: ToolButton {}
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

        Separator { visible: bar.toolsShown }

        ActionButton {
            id: longButton
            visible: !bar.editor && bar.shows("long")
            glyph: String.fromCharCode(0xEC8F)
            name: qsTr("长截图")
            tipText: qsTr("长截图：滚动着截下选区里更长的内容")
            tipKeys: "Ctrl+L"
            onClicked: bar.act(CaptureSession.Long)
        }
        ActionButton {
            id: recordButton
            visible: !bar.editor && bar.shows("record")
            glyph: String.fromCharCode(0xE714)
            name: qsTr("录屏")
            tipText: qsTr("区域录屏：录制选区里的操作，保存为 MP4 或 GIF")
            tipKeys: "Ctrl+R"
            onClicked: bar.act(CaptureSession.Record)
        }
        ActionButton {
            id: delayButton
            visible: !bar.editor && bar.shows("delay")
            glyph: String.fromCharCode(0xE916) // Stopwatch
            name: qsTr("延时")
            tipText: qsTr("延时：关掉这次截图，倒计时后重新截（秒数在设置里选）")
            tipKeys: "D"
            onClicked: bar.act(CaptureSession.Delay)
        }
        ActionButton {
            id: qrButton
            visible: !bar.editor && bar.shows("qr")
            glyph: String.fromCharCode(0xED14) // QRCode
            name: qsTr("二维码")
            tipText: qsTr("二维码：识别选区里的二维码，可复制文字或打开链接")
            tipKeys: "Q"
            onClicked: bar.act(CaptureSession.Qr)
        }
        ActionButton {
            id: ocrButton
            visible: bar.shows("ocr")
            // Without the models it still says why, in its tip and the OCR window.
            opacity: bar.ocrAvailable ? 1 : 0.45
            glyph: String.fromCharCode(0xE8D2) // Font
            name: qsTr("识别文字")
            tipText: bar.ocrAvailable ? qsTr("识别文字：识别图里的文字，可复制或拿去搜索")
                                      : qsTr("识别文字：没有安装文字识别模型（Win顺 文件夹里的 ocr 文件夹），无法识别文字")
            onClicked: bar.act(CaptureSession.Ocr)
        }

        Separator { visible: bar.outputShown }

        ActionButton {
            id: pinButton
            visible: bar.pinnable && bar.shows("pin")
            glyph: String.fromCharCode(0xE718) // Pin
            name: qsTr("贴图")
            tipText: qsTr("贴图：留在屏幕上，可拖动、缩放")
            tipKeys: "F3"
            onClicked: bar.act(CaptureSession.Pin)
        }
        ActionButton {
            id: saveButton
            visible: bar.shows("save")
            glyph: String.fromCharCode(0xE74E) // Save
            name: qsTr("保存")
            tipText: qsTr("保存")
            tipKeys: "Ctrl+S"
            onClicked: bar.act(CaptureSession.Save)
        }

        Separator {}

        Rectangle { // "?": every key, in a list by the toolbar
            id: helpButton
            property string tipText: qsTr("全部按键")
            property string tipKeys: ""
            width: bar.helpWidth
            height: bar.buttonHeight
            radius: Theme.controlRadius
            color: helpArea.pressed ? Theme.controlPressed : helpArea.containsMouse || bar.keysOpen ? Theme.controlHover : "transparent"
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("全部按键")

            Text {
                anchors.centerIn: parent
                text: "?"
                color: helpArea.containsMouse || bar.keysOpen ? Theme.text : Theme.subtext
                font.pixelSize: Theme.fontBody
            }
            MouseArea {
                id: helpArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: bar.keysOpen = !bar.keysOpen
                onContainsMouseChanged: bar.hover(helpButton, containsMouse, -1)
            }
        }

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
                return t.letter + " / " + t.digit
            }
            return tip ? tip.item.tipKeys : ""
        }
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

    Rectangle {
        id: keysCard

        readonly property real roomBelow: parent ? parent.height - (bar.blockRect.y + bar.blockRect.height) : 0
        readonly property bool down: bar.opensDown ? roomBelow >= height + bar.popupGap + bar.popupMargin
                                                   : bar.blockRect.y < height + bar.popupGap + bar.popupMargin

        parent: bar.Window.contentItem
        z: 900
        visible: bar.keysOpen
        width: Math.ceil((keysGrid.width + 2 * bar.popupPadX) / 4) * 4
        height: Math.ceil((keysTitle.height + bar.popupTitleGap + keysGrid.height + 2 * bar.popupPadY) / 4) * 4
        x: parent ? bar.snap(Math.max(bar.popupMargin, Math.min(bar.blockRect.x + bar.blockRect.width - width,
                                                                 parent.width - width - bar.popupMargin))) : 0
        y: parent ? bar.snap(Math.max(bar.popupMargin, Math.min(down ? bar.blockRect.y + bar.blockRect.height + bar.popupGap
                                                                      : bar.blockRect.y - bar.popupGap - height,
                                                                 parent.height - height - bar.popupMargin))) : 0
        radius: Theme.controlRadius
        color: Theme.background
        border.width: 1
        border.color: Theme.windowEdge

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
            font.weight: Font.DemiBold
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
                    font.weight: index % 2 === 0 ? Font.DemiBold : Font.Normal
                }
            }
        }
    }

    // ---- the parts ----

    // A thin upright line between groups, on a whole device pixel.
    component Separator: Item {
        width: bar.separatorWidth
        height: bar.buttonHeight

        Rectangle {
            x: bar.snap(bar.separatorWidth / 2)
            y: bar.snap((bar.buttonHeight - bar.separatorHeight) / 2)
            width: Math.max(1, Math.round(bar.dpr)) / bar.dpr
            height: bar.separatorHeight
            color: Theme.controlBorder
        }
    }

    // An icon-only action: flat, as wide as a tool; primary: accentFill and a
    // white mark. The tip (name, keys) is read by the one HoverTip.
    component ActionButton: Rectangle {
        id: action

        property string glyph
        property string name
        property string tipText
        property string tipKeys
        property bool primary: false
        signal clicked()

        width: bar.toolWidth
        height: bar.buttonHeight
        radius: Theme.controlRadius
        color: primary
               ? (actionArea.pressed ? Qt.darker(Theme.accentFill, 1.15) : actionArea.containsMouse ? Qt.lighter(Theme.accentFill, 1.08) : Theme.accentFill)
               : (actionArea.pressed ? Theme.controlPressed : actionArea.containsMouse ? Theme.controlHover : "transparent")
        opacity: enabled ? 1 : 0.45
        Accessible.role: Accessible.Button
        Accessible.name: name
        Accessible.onPressAction: if (enabled) action.clicked()

        Glyph {
            x: bar.snap((bar.toolWidth - bar.iconSize) / 2)
            y: bar.snap((bar.buttonHeight - bar.iconSize) / 2)
            width: bar.iconSize
            height: bar.iconSize
            glyph: action.glyph
            size: bar.iconSize
            color: action.primary ? Theme.accentText : Theme.text
        }

        MouseArea {
            id: actionArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: action.clicked()
            onContainsMouseChanged: bar.hover(action, containsMouse, -1)
        }
    }

    // A tool: accentFill and a white icon while it is on; a click turns it
    // on, or off again when it is on (the key that opens it closes it too).
    component ToolButton: Rectangle {
        id: tool

        required property int index
        required property var modelData
        readonly property bool current: bar.annot !== null && bar.annot.tool === index

        width: bar.toolWidth
        height: bar.buttonHeight
        radius: Theme.controlRadius
        color: current
               ? (toolArea.pressed ? Qt.darker(Theme.accentFill, 1.15) : toolArea.containsMouse ? Qt.lighter(Theme.accentFill, 1.08) : Theme.accentFill)
               : (toolArea.pressed ? Theme.controlPressed : toolArea.containsMouse ? Theme.controlHover : "transparent")
        Accessible.role: Accessible.Button
        Accessible.name: modelData.name

        ToolIcon {
            x: bar.snap((bar.toolWidth - bar.iconSize) / 2)
            y: bar.snap((bar.buttonHeight - bar.iconSize) / 2)
            kind: tool.modelData.kind
            color: tool.current ? Theme.accentText : Theme.text
        }

        MouseArea {
            id: toolArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: bar.annot.tool = tool.current ? -1 : tool.index
            onContainsMouseChanged: bar.hover(tool, containsMouse, tool.index)
        }
    }

    // A tool's 16 logical px icon. The rectangle, ellipse, pen and marker are
    // glyphs (Phosphor, through Glyph.qml); the arrow, line, T, numbered disc and mosaic
    // are drawn in device pixels, as Glyph.qml draws its own: 1 px strokes up
    // to 20 device px (100 %, 125 %), 1.5 px up to 28 (150 %, 175 %), 2 above.
    component ToolIcon: Item {
        id: icon

        property int kind: 0
        property color color: Theme.text
        // Kind -> glyph (AnnotationStyles: Rect, Ellipse, Arrow, Line, Pen, Marker, Text, Number, Redact).
        readonly property string glyph: kind === 0 ? String.fromCharCode(0xE739) // Checkbox (a square)
                                      : kind === 1 ? String.fromCharCode(0xEA3A) // CircleRing
                                      : kind === 4 ? String.fromCharCode(0xE76D) // InkingTool
                                      : kind === 5 ? String.fromCharCode(0xE7E6) // Highlight
                                      : ""

        width: bar.iconSize
        height: bar.iconSize

        Glyph {
            anchors.centerIn: parent
            visible: icon.glyph.length > 0
            glyph: icon.glyph
            size: bar.iconSize
            color: icon.color
        }

        Canvas {
            id: drawn

            // Nudged off the exact half pixel, as Glyph.qml's drawn icons.
            x: 1 / 64
            y: 1 / 64
            width: bar.iconSize
            height: bar.iconSize
            visible: icon.glyph.length === 0
            antialiasing: true
            smooth: false
            onVisibleChanged: requestPaint()

            Connections {
                target: icon
                function onColorChanged() { drawn.requestPaint() }
                function onKindChanged() { drawn.requestPaint() }
            }
            Connections {
                target: bar
                function onDprChanged() { drawn.requestPaint() }
            }

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                if (!visible)
                    return
                // n device pixels across; draw in them.
                const n = Math.round(width * bar.dpr)
                const s = n <= bar.thinUpTo ? 1 : n <= bar.midUpTo ? 1.5 : 2
                const u = n / 16
                ctx.scale(width / n, height / n)
                ctx.strokeStyle = icon.color
                ctx.fillStyle = icon.color
                ctx.lineWidth = s
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                const lo = Math.round(bar.iconInset * u) + s / 2 // an outer edge on a whole pixel
                const hi = n - lo
                ctx.beginPath()
                if (icon.kind === 2) { // arrow, up to the right
                    const head = Math.round(bar.arrowHead * u)
                    ctx.moveTo(lo, hi)
                    ctx.lineTo(hi, lo)
                    ctx.moveTo(hi - head, lo)
                    ctx.lineTo(hi, lo)
                    ctx.lineTo(hi, lo + head)
                    ctx.stroke()
                } else if (icon.kind === 3) { // line
                    ctx.moveTo(lo, hi)
                    ctx.lineTo(hi, lo)
                    ctx.stroke()
                } else if (icon.kind === 6) { // T
                    const mid = Math.floor((n - s) / 2) + s / 2 // the stem's left edge on a whole pixel
                    ctx.moveTo(lo, lo)
                    ctx.lineTo(hi, lo)
                    ctx.moveTo(mid, lo)
                    ctx.lineTo(mid, hi)
                    ctx.stroke()
                } else if (icon.kind === 7) { // a disc's ring with a 1 in it
                    const c = n / 2
                    ctx.arc(c, c, c - Math.round(bar.ringInset * u) - s / 2, 0, 2 * Math.PI)
                    ctx.stroke()
                    ctx.beginPath()
                    const one = Math.floor((n - s) / 2) + s / 2
                    const top = Math.round(bar.oneTop * u)
                    ctx.moveTo(one - Math.round(bar.oneFlag * u), top + Math.round(bar.oneFlag * u))
                    ctx.lineTo(one, top)
                    ctx.lineTo(one, n - top)
                    ctx.stroke()
                } else if (icon.kind === 8) { // mosaic: a 3 x 3 checker of whole-pixel cells
                    const cell = Math.floor((n - 2 * Math.round(bar.mosaicInset * u)) / 3)
                    const m = Math.floor((n - 3 * cell) / 2)
                    // The corners and the middle (no loop: qmllint 6.12 stops
                    // checking the whole file at a nested `for (let …)`).
                    ctx.fillRect(m, m, cell, cell)
                    ctx.fillRect(m + 2 * cell, m, cell, cell)
                    ctx.fillRect(m + cell, m + cell, cell, cell)
                    ctx.fillRect(m, m + 2 * cell, cell, cell)
                    ctx.fillRect(m + 2 * cell, m + 2 * cell, cell, cell)
                    ctx.lineWidth = s
                    ctx.strokeRect(m + s / 2, m + s / 2, 3 * cell - s, 3 * cell - s)
                }
            }
        }
    }
}
