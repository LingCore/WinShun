pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The presenter's toolbar (GitHub #189, docs/design-presenter.md 2.3): a
// window of its own that never takes the focus, so it can be clicked while
// the ink lets the mouse through to the programs beneath, and the program in
// front stays in front. The capture toolbar's look: the tools, the laser,
// the colour (the style panel below), undo, redo, clear, the boards,
// through, and the way out. H folds it to its grip. Below it, one line at a
// time: a button's name and key while the mouse rests on it, else a hint
// shown once. The window is as large as what shows, so it takes no clicks
// beside it.
Window {
    id: window

    required property Presenter presenter

    readonly property AnnotationModel annot: presenter.model
    readonly property real dpr: Screen.devicePixelRatio
    readonly property bool through: presenter.through
    readonly property bool folded: !presenter.toolbarShown
    property bool colorsOpen: false
    // The button the mouse rests on (its name and keys change with what it
    // does: fold, through), in the line below.
    property var tipItem: null
    readonly property string tipText: tipItem ? tipItem.name : ""
    readonly property string tipKeys: tipItem ? tipItem.keys : ""
    readonly property string lineText: tipText.length > 0 ? tipText : presenter.hint
    // What shows, in window logical px: the window takes clicks only there
    // (PresenterHost sets it as the window's mask), so beside the shorter
    // line the clicks reach the program beneath.
    readonly property var inputRects: [bar, stylePanel, line].filter(item => item.visible)
        .map(item => Qt.rect(column.x + item.x, item.y, item.width, item.height))

    // ---- geometry, logical px: the capture toolbar's ----
    readonly property int pad: 4
    readonly property int spacing: 4
    readonly property int buttonHeight: 36
    readonly property int toolWidth: 32
    readonly property int gripWidth: 20
    readonly property int iconSize: 16
    readonly property int separatorWidth: 8
    readonly property int separatorHeight: 20
    readonly property int dotSize: 14 // the colour button's dot
    readonly property int boardSize: 14 // the board buttons' square
    readonly property int laserDot: 8
    readonly property int gap: 4 // between the bar, the panel and the line
    readonly property int linePadX: 10
    readonly property int lineHeight: 28
    readonly property int showMs: 150
    readonly property int hintMs: 8000
    // The tools offered, in the toolbar's order (AnnotationStyles' kinds):
    // pen, marker, arrow, line, rectangle, ellipse, text, number.
    readonly property var toolOrder: [4, 5, 2, 3, 0, 1, 6, 7]
    // ToolIcon's drawing, as the capture toolbar's.
    readonly property real iconInset: 2.5
    readonly property real arrowHead: 5.5
    readonly property real ringInset: 1.5
    readonly property real oneTop: 4.5
    readonly property real oneFlag: 2
    readonly property int thinUpTo: 20
    readonly property int midUpTo: 28

    function snap(v) { return Math.round(v * dpr) / dpr }
    function centre(outer, inner) { return snap((outer - inner) / 2) } // on whole pixels
    function hover(on, item) {
        if (on)
            tipItem = item
        else if (tipItem === item)
            tipItem = null
    }

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    title: qsTr("演示工具条")
    width: Math.ceil(column.width)
    height: Math.ceil(column.height)

    onVisibleChanged: {
        if (!visible) {
            colorsOpen = false
            tipItem = null
            return
        }
        showing.restart()
    }
    onFoldedChanged: if (folded) colorsOpen = false
    onLineTextChanged: if (presenter.hint.length > 0 && tipText.length === 0) hintTimer.restart()

    Timer {
        id: hintTimer
        interval: window.hintMs
        onTriggered: window.presenter.dismissHint()
    }

    ParallelAnimation {
        id: showing
        OpacityAnimator { target: column; from: 0; to: 1; duration: window.showMs; easing.type: Easing.OutCubic }
        YAnimator { target: column; from: -4; to: 0; duration: window.showMs; easing.type: Easing.OutCubic }
    }

    Column {
        id: column
        spacing: window.gap

        Item {
            id: bar
            width: Theme.snapUp(row.width + 2 * window.pad, window.dpr)
            height: Theme.snapUp(window.buttonHeight + 2 * window.pad, window.dpr)

            Hook { // the style's floating panel (Theme.menuFrame)
                anchors.fill: parent
                part: Theme.menuFrame
                extra: ({ floating: true })
                dpr: window.dpr
            }

            MouseArea { // presses between the buttons stay here
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
            }

            Row {
                id: row
                x: window.pad
                y: window.pad
                spacing: window.spacing

                Item { // the grip: drags the toolbar
                    id: grip
                    readonly property string name: qsTr("按住拖动工具条")
                    readonly property string keys: ""
                    width: window.gripWidth
                    height: window.buttonHeight
                    Accessible.role: Accessible.Grip
                    Accessible.name: grip.name

                    KeyFace {
                        id: gripFace
                        hovered: gripArea.containsMouse
                        pressed: gripArea.pressed
                    }
                    Glyph {
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: gripFace.out("contentOffsetY", 0)
                        glyph: String.fromCharCode(0xE76F) // GripperBarHorizontal
                        size: window.iconSize
                        color: gripFace.out("foreground", Theme.subtext)
                    }

                    MouseArea {
                        id: gripArea
                        // Moved here, not by the system's move: that makes the toolbar the
                        // active window, and the keys (H, W, Esc...) no longer reach the ink.
                        property point pressedAt
                        property point startedAt
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeAllCursor
                        onPressed: (mouse) => {
                            pressedAt = mapToGlobal(mouse.x, mouse.y)
                            startedAt = Qt.point(window.x, window.y)
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed)
                                return
                            const at = mapToGlobal(mouse.x, mouse.y)
                            window.x = Math.round(startedAt.x + at.x - pressedAt.x)
                            window.y = Math.round(startedAt.y + at.y - pressedAt.y)
                        }
                        onContainsMouseChanged: window.hover(containsMouse, grip)
                    }
                }

                Repeater {
                    model: window.folded ? [] : window.toolOrder
                    delegate: BarButton {
                        id: toolButton
                        required property int modelData
                        readonly property var info: window.annot ? window.annot.tools[modelData] : null
                        checked: !window.presenter.laser && window.presenter.tool === modelData
                        name: info ? info.name : ""
                        keys: info ? info.letter + " / " + info.digit : ""
                        onClicked: {
                            window.presenter.tool = checked ? -1 : modelData
                            window.presenter.draw()
                        }

                        ToolIcon {
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: toolButton.markOffsetY
                            kind: toolButton.modelData
                            color: toolButton.foreground
                        }
                    }
                }

                Separator { visible: !window.folded }

                BarButton { // the laser pointer
                    id: laserButton
                    visible: !window.folded
                    checked: window.presenter.laser
                    name: qsTr("激光笔：按住拖动，留下渐隐的红色轨迹")
                    keys: "X"
                    onClicked: {
                        window.presenter.laser = !checked
                        window.presenter.draw()
                    }

                    Rectangle {
                        x: window.centre(parent.width, width)
                        y: window.centre(parent.height, height) + parent.markOffsetY
                        width: window.laserDot + 6
                        height: width
                        radius: width / 2
                        color: "transparent"
                        border.width: window.snap(1.5)
                        border.color: laserButton.foreground
                        opacity: 0.6
                    }
                    Rectangle {
                        x: window.centre(parent.width, width)
                        y: window.centre(parent.height, height) + parent.markOffsetY
                        width: window.laserDot
                        height: width
                        radius: width / 2
                        color: laserButton.checked ? Theme.accentText : window.presenter.laserColor
                    }
                }

                BarButton { // the colour and width: the style panel below
                    id: colorButton
                    visible: !window.folded
                    enabled: !window.presenter.laser && window.annot !== null && window.annot.panelVisible
                    checked: window.colorsOpen && enabled
                    name: qsTr("颜色和粗细")
                    keys: "[ ]"
                    onClicked: window.colorsOpen = !window.colorsOpen

                    Rectangle {
                        x: window.centre(parent.width, width)
                        y: window.centre(parent.height, height) + parent.markOffsetY
                        width: window.dotSize
                        height: width
                        radius: width / 2
                        color: window.annot && window.annot.panel.color !== undefined ? window.annot.panel.color : Theme.subtext
                        border.width: Theme.edge(window.dpr)
                        border.color: colorButton.checked ? Theme.accentText : Theme.windowEdge
                    }
                }

                Separator { visible: !window.folded }

                BarButton {
                    visible: !window.folded
                    enabled: window.presenter.canUndo
                    glyph: String.fromCharCode(0xE7A7) // Undo
                    name: qsTr("撤销")
                    keys: "Ctrl+Z"
                    onClicked: window.presenter.undo()
                }
                BarButton {
                    visible: !window.folded
                    enabled: window.presenter.canRedo
                    glyph: String.fromCharCode(0xE7A6) // Redo
                    name: qsTr("重做")
                    keys: "Ctrl+Y"
                    onClicked: window.presenter.redo()
                }
                BarButton {
                    visible: !window.folded
                    enabled: window.presenter.canClear
                    glyph: String.fromCharCode(0xE74D) // Delete
                    name: qsTr("清屏（可撤销）")
                    keys: "Delete"
                    onClicked: window.presenter.clear()
                }

                Separator { visible: !window.folded }

                BarButton { // the white board
                    id: whiteButton
                    visible: !window.folded
                    checked: window.presenter.board === PresenterState.Board.White
                    name: qsTr("白板")
                    keys: "W"
                    onClicked: window.presenter.toggleBoard(PresenterState.Board.White)

                    Rectangle {
                        x: window.centre(parent.width, width)
                        y: window.centre(parent.height, height) + parent.markOffsetY
                        width: window.boardSize
                        height: window.boardSize
                        radius: Theme.smallControlRadius
                        color: window.presenter.whiteBoardColor
                        border.width: Theme.edge(window.dpr)
                        border.color: whiteButton.checked ? Theme.accentText : Theme.controlBorder
                    }
                }
                BarButton { // the black board
                    id: blackButton
                    visible: !window.folded
                    checked: window.presenter.board === PresenterState.Board.Black
                    name: qsTr("黑板")
                    keys: "K"
                    onClicked: window.presenter.toggleBoard(PresenterState.Board.Black)

                    Rectangle {
                        x: window.centre(parent.width, width)
                        y: window.centre(parent.height, height) + parent.markOffsetY
                        width: window.boardSize
                        height: window.boardSize
                        radius: Theme.smallControlRadius
                        color: window.presenter.blackBoardColor
                        border.width: Theme.edge(window.dpr)
                        border.color: blackButton.checked ? Theme.accentText : Theme.controlBorder
                    }
                }

                Separator { visible: !window.folded }

                BarButton { // through: the mouse and keys to the programs beneath, the ink kept
                    visible: !window.folded
                    enabled: window.through || window.presenter.canThrough
                    checked: window.through
                    glyph: String.fromCharCode(0xE7C9) // TouchPointer (a cursor)
                    name: window.through ? qsTr("回到画笔") : qsTr("穿透：操作下面的程序，笔迹留着")
                    keys: window.through ? "" : "Esc"
                    onClicked: window.presenter.toggleThrough()
                }

                Separator { visible: !window.folded }

                BarButton { // folds the toolbar to its grip, or opens it again
                    glyph: String.fromCharCode(window.folded ? 0xE70D : 0xE70E) // ChevronDown, ChevronUp
                    name: window.folded ? qsTr("展开工具条") : qsTr("收起工具条")
                    keys: "H"
                    onClicked: window.presenter.toggleToolbar()
                }
                BarButton {
                    glyph: String.fromCharCode(0xE711) // Cancel
                    name: qsTr("结束演示（笔迹不保留）")
                    onClicked: window.presenter.close()
                }
            }
        }

        AnnotateStylePanel {
            id: stylePanel
            visible: window.colorsOpen && !window.folded && !window.presenter.laser && window.annot !== null && window.annot.panelVisible
            annot: window.annot
            dpr: window.dpr
            picker: false
        }

        Item { // a name and key, or the hint
            id: line
            visible: window.lineText.length > 0
            width: Theme.snapUp(lineRow.width + 2 * window.linePadX, window.dpr)
            height: Theme.snapUp(window.lineHeight, window.dpr)

            Hook { // the style's floating panel (Theme.menuFrame)
                anchors.fill: parent
                part: Theme.menuFrame
                dpr: window.dpr
            }

            MouseArea { // a click puts the hint away
                anchors.fill: parent
                onClicked: window.presenter.dismissHint()
            }

            Row {
                id: lineRow
                x: window.linePadX
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Text {
                    text: window.lineText
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
                Text {
                    visible: window.tipText.length > 0 && window.tipKeys.length > 0
                    text: window.tipKeys
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
            }
        }
    }

    // ---- the parts ----

    component Separator: Item {
        width: window.separatorWidth
        height: window.buttonHeight

        Hook { // the style's separator
            y: window.snap((window.buttonHeight - window.separatorHeight) / 2)
            width: window.separatorWidth
            height: window.separatorHeight
            part: Theme.separator
            orientation: Qt.Vertical
            dpr: window.dpr
        }
    }

    // A toolbar key's face (Theme.keySurface): flat until the pointer is on it.
    component KeyFace: Hook {
        anchors.fill: parent
        part: Theme.keySurface
        flat: true
        role: "toolbar"
        dpr: window.dpr
    }

    // A flat button; checked: the style's chosen key (accentFill with a white mark in fluent). A glyph, or what is put in it.
    component BarButton: Item {
        id: button

        property string glyph
        property string name
        property string keys
        property bool checked: false
        readonly property color foreground: face.out("foreground", checked ? Theme.accentText : Theme.text)
        // How far the marks sit from the middle (a pressed key sinks a little).
        readonly property real markOffsetY: face.out("contentOffsetY", 0)
        signal clicked()

        width: window.toolWidth
        height: window.buttonHeight
        opacity: enabled ? 1 : face.out("disabledOpacity", 0.45)
        Accessible.role: Accessible.Button
        Accessible.name: name
        Accessible.onPressAction: if (enabled) button.clicked()

        KeyFace {
            id: face
            hovered: area.containsMouse && button.enabled
            pressed: area.pressed
            checked: button.checked
        }

        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: button.markOffsetY
            visible: button.glyph.length > 0
            glyph: button.glyph
            size: window.iconSize
            color: button.foreground
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: if (button.enabled) button.clicked()
            onContainsMouseChanged: window.hover(containsMouse, button)
        }
    }

    // A tool's 16 logical px icon, as the capture toolbar's (AnnotateToolbar's ToolIcon).
    component ToolIcon: Item {
        id: icon

        property int kind: 0
        property color color: Theme.text
        readonly property string glyph: kind === 0 ? String.fromCharCode(0xE739) // Checkbox (a square)
                                      : kind === 1 ? String.fromCharCode(0xEA3A) // CircleRing
                                      : kind === 4 ? String.fromCharCode(0xE76D) // InkingTool
                                      : kind === 5 ? String.fromCharCode(0xE7E6) // Highlight
                                      : ""

        width: window.iconSize
        height: window.iconSize

        Glyph {
            anchors.centerIn: parent
            visible: icon.glyph.length > 0
            glyph: icon.glyph
            size: window.iconSize
            color: icon.color
        }

        Canvas {
            id: drawn

            x: 1 / 64
            y: 1 / 64
            width: window.iconSize
            height: window.iconSize
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
                target: window
                function onDprChanged() { drawn.requestPaint() }
            }

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                if (!visible)
                    return
                const n = Math.round(width * window.dpr)
                const s = n <= window.thinUpTo ? 1 : n <= window.midUpTo ? 1.5 : 2
                const u = n / 16
                ctx.scale(width / n, height / n)
                ctx.strokeStyle = icon.color
                ctx.lineWidth = s
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                const lo = Math.round(window.iconInset * u) + s / 2
                const hi = n - lo
                ctx.beginPath()
                if (icon.kind === 2) { // arrow, up to the right
                    const head = Math.round(window.arrowHead * u)
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
                    const mid = Math.floor((n - s) / 2) + s / 2
                    ctx.moveTo(lo, lo)
                    ctx.lineTo(hi, lo)
                    ctx.moveTo(mid, lo)
                    ctx.lineTo(mid, hi)
                    ctx.stroke()
                } else if (icon.kind === 7) { // a disc's ring with a 1 in it
                    const c = n / 2
                    ctx.arc(c, c, c - Math.round(window.ringInset * u) - s / 2, 0, 2 * Math.PI)
                    ctx.stroke()
                    ctx.beginPath()
                    const one = Math.floor((n - s) / 2) + s / 2
                    const top = Math.round(window.oneTop * u)
                    ctx.moveTo(one - Math.round(window.oneFlag * u), top + Math.round(window.oneFlag * u))
                    ctx.lineTo(one, top)
                    ctx.lineTo(one, n - top)
                    ctx.stroke()
                }
            }
        }
    }
}
