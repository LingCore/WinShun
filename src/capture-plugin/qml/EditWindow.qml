pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The editing window (src/capture, CaptureEditor; docs/design-capture-
// roadmap.md, 2.2): one window, made the first time and shown and hidden
// after. Over the picture it holds what the capture overlay does, the same
// model, canvas, toolbar and style panel, and adds the zoom, panning, the
// crop and selecting several objects. CaptureEditor does the arithmetic and
// the input; this places things where it says. Positions of the picture are
// in the window's logical units, put on whole device pixels by the editor,
// so that at 100 % one image pixel is one screen pixel.
Window {
    id: window

    required property CaptureEditor editor
    required property WindowFrame frame

    readonly property AnnotationModel annotation: editor.model
    readonly property real dpr: editor.dpr
    readonly property int margin: 12 // the toolbar's and the panel's edge to the window's

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)
    color: Theme.page
    title: qsTr("编辑图片") // shown as "编辑图片 - Win顺"
    width: 1000
    height: 700
    // The toolbar's width; and while the style panel shows, its width and the size and zoom beside it
    // (the number tool's panel is wider than a default window: live test 2026-10-22).
    minimumWidth: Math.ceil(Math.max(bar.implicitWidth + 2 * margin,
                                     panel.visible ? panel.implicitWidth + status.implicitWidth + 3 * margin : 0))
    minimumHeight: 360

    onActiveChanged: if (active && editor.open) keys.forceActiveFocus()


    Connections {
        target: window.editor
        function onOpenChanged() {
            if (window.editor.open)
                keys.forceActiveFocus()
        }
    }

    TitleBar {
        id: titleBar
        z: 1
        width: parent.width
        frame: window.frame
        title: window.title + " - " + Qt.application.displayName
        // Not the window hidden at once, which would leave the canvas its tiles and
        // textures: the editor empties and hides it (CaptureEditor::close).
        closeIntercepted: true
        onCloseRequested: window.editor.close()
    }

    // ---- the toolbar: the tools, undo, the ways out ----

    AnnotateToolbar {
        id: bar
        x: window.margin
        y: window.onPixel(titleBar.height + (window.editor.toolbarRow - height) / 2)
        z: 2
        annot: window.annotation
        editor: true
        dpr: window.dpr
        copyText: window.editor.copyText
        pinnable: !window.editor.fromPin
        ocrAvailable: window.editor.ocrAvailable
        blockRect: Qt.rect(x, y, width, height)
        opensDown: true
        onAction: (which) => window.editor.act(which)
    }

    // ---- the picture ----

    Item {
        id: view

        // The picture's part of the window.
        y: window.editor.topHeight
        width: parent.width
        height: Math.max(0, parent.height - y - window.editor.bottomHeight)
        clip: true

        Rectangle { // round the picture
            anchors.fill: parent
            color: Theme.track
        }

        MouseArea { // under the picture's own boxes (the text, the number): their presses stay with them
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            cursorShape: window.editor.cursorShape
            onPressed: (m) => window.editor.mousePressed(view.x + m.x, view.y + m.y, m.button, m.modifiers)
            onPositionChanged: (m) => window.editor.mouseMoved(view.x + m.x, view.y + m.y, m.modifiers)
            onReleased: (m) => window.editor.mouseReleased(view.x + m.x, view.y + m.y, m.button)
            onDoubleClicked: (m) => {
                if (m.button === Qt.LeftButton)
                    window.editor.mouseDoubleClicked(view.x + m.x, view.y + m.y)
            }
            onWheel: (w) => {
                window.editor.wheeled(view.x + w.x, view.y + w.y, w.angleDelta.y, w.angleDelta.x, w.modifiers)
                w.accepted = true
            }
        }

        HoverHandler { id: hover }

        Item { // the whole picture, at the zoom; its top left on a device pixel
            id: content

            readonly property rect crop: window.annotation.crop
            readonly property size image: window.editor.imageSize
            readonly property bool cropped: crop.x !== 0 || crop.y !== 0 || crop.width !== image.width
                                            || crop.height !== image.height
            readonly property real sx: window.editor.scaleX
            readonly property real sy: window.editor.scaleY
            // The pointer is near the crop's edge: its handles show.
            readonly property real near: 28
            readonly property bool nearEdge: {
                if (!hover.hovered || window.annotation.tool >= 0)
                    return false
                // The pointer is the view's; this item is x, y in it.
                const px = hover.point.position.x - x
                const py = hover.point.position.y - y
                const l = crop.x * sx
                const t = crop.y * sy
                const r = (crop.x + crop.width) * sx
                const b = (crop.y + crop.height) * sy
                const inside = px > l - near && px < r + near && py > t - near && py < b + near
                const deep = px > l + near && px < r - near && py > t + near && py < b - near
                return inside && !deep
            }
            readonly property bool framed: cropped || nearEdge || window.annotation.cropping

            x: window.editor.contentOrigin.x - view.x
            y: window.editor.contentOrigin.y - view.y
            width: window.editor.contentSize.width
            height: window.editor.contentSize.height

            AnnotationCanvas {
                width: parent.width
                height: parent.height
                model: window.annotation
                limited: true
                viewport: window.editor.visible
                linear: window.editor.linear
                scaleX: content.sx
                scaleY: content.sy
                visible: model !== null
            }

            Item { // outside the crop, darker
                id: shade

                readonly property color tint: Qt.rgba(0, 0, 0, 0.45)
                readonly property real edgeLeft: content.crop.x * content.sx
                readonly property real edgeTop: content.crop.y * content.sy
                readonly property real edgeRight: (content.crop.x + content.crop.width) * content.sx
                readonly property real edgeBottom: (content.crop.y + content.crop.height) * content.sy

                anchors.fill: parent
                visible: content.cropped

                Rectangle {
                    width: parent.width
                    height: shade.edgeTop
                    color: shade.tint
                }
                Rectangle {
                    y: shade.edgeBottom
                    width: parent.width
                    height: parent.height - shade.edgeBottom
                    color: shade.tint
                }
                Rectangle {
                    y: shade.edgeTop
                    width: shade.edgeLeft
                    height: shade.edgeBottom - shade.edgeTop
                    color: shade.tint
                }
                Rectangle {
                    x: shade.edgeRight
                    y: shade.edgeTop
                    width: parent.width - shade.edgeRight
                    height: shade.edgeBottom - shade.edgeTop
                    color: shade.tint
                }
            }

            Item { // the crop's frame, just outside it, and its eight handles
                id: frameBox

                readonly property int handle: Math.round(7 * window.dpr) | 1 // odd: centred on the edge line
                readonly property real ux: window.editor.unitX
                readonly property real uy: window.editor.unitY

                visible: content.framed
                x: content.crop.x * content.sx
                y: content.crop.y * content.sy
                width: content.crop.width * content.sx
                height: content.crop.height * content.sy

                Rectangle {
                    x: -frameBox.ux
                    y: -frameBox.uy
                    width: parent.width + 2 * frameBox.ux
                    height: parent.height + 2 * frameBox.uy
                    color: "transparent"
                    border.width: frameBox.ux
                    border.color: Theme.accent
                }

                Repeater {
                    // Corners and edge middles, as fractions of the crop.
                    model: content.nearEdge || window.annotation.cropping ? [[0, 0], [0.5, 0], [1, 0], [0, 0.5], [1, 0.5], [0, 1], [0.5, 1], [1, 1]] : []

                    delegate: Rectangle {
                        required property var modelData

                        // Centred on the edge line (between two pixels), on whole device pixels.
                        readonly property real lineX: Math.round(content.crop.width * modelData[0]) * content.sx
                        readonly property real lineY: Math.round(content.crop.height * modelData[1]) * content.sy

                        x: Math.round(lineX / frameBox.ux - frameBox.handle / 2) * frameBox.ux
                        y: Math.round(lineY / frameBox.uy - frameBox.handle / 2) * frameBox.uy
                        width: frameBox.handle * frameBox.ux
                        height: frameBox.handle * frameBox.uy
                        color: Theme.accentFill
                        border.width: frameBox.ux
                        border.color: Theme.accentText
                    }
                }
            }

            AnnotateObjects { // the selected objects' handles, the text box, the number box
                anchors.fill: parent
                annot: window.annotation
                keyTarget: keys
                sx: content.sx
                sy: content.sy
                dpr: window.dpr
                unitX: window.editor.unitX
                unitY: window.editor.unitY
                onKeyForwarded: (key, modifiers) => window.editor.keyPressed(key, modifiers)
            }

            Rectangle { // the box being dragged to select objects
                readonly property rect box: window.annotation.marquee

                visible: box.width > 0
                x: box.x * content.sx
                y: box.y * content.sy
                width: box.width * content.sx
                height: box.height * content.sy
                color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.12)
                border.width: window.editor.unitX
                border.color: Theme.accent
            }
        }

        Rectangle { // a word for the user: the changes given up
            id: noticeBar

            property string words

            anchors.horizontalCenter: parent.horizontalCenter
            y: 12
            width: Math.ceil((label.implicitWidth + 20) / 4) * 4
            height: 28
            radius: Theme.controlRadius
            color: Theme.background
            border.width: 1
            border.color: Theme.windowEdge
            opacity: window.editor.notice.length > 0 ? 1 : 0
            visible: opacity > 0

            Behavior on opacity { NumberAnimation { duration: 120 } }

            Connections {
                target: window.editor
                function onNoticeChanged() {
                    if (window.editor.notice.length > 0)
                        noticeBar.words = window.editor.notice
                }
            }

            Text {
                id: label
                anchors.centerIn: parent
                text: noticeBar.words
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    // ---- below the picture: the style panel, the size and the zoom ----

    Item {
        id: strip

        y: parent.height - height
        width: parent.width
        height: window.editor.bottomHeight

        Rectangle { // between the picture and this row
            width: parent.width
            height: Math.max(1, Math.round(window.dpr)) / window.dpr
            color: Theme.divider
        }

        AnnotateSmartHint { // 智能打码's offer, in the style panel's place while no tool is on
            annot: window.annotation
            dpr: window.dpr
            visible: !panel.visible && found > 0
            x: window.margin
            y: window.onPixel((strip.height - height) / 2)
        }

        AnnotateStylePanel {
            id: panel
            x: window.margin
            y: window.onPixel((strip.height - height) / 2)
            visible: window.annotation.panelVisible
            annot: window.annotation
            dpr: window.dpr
            direction: -1
        }

        Row {
            id: status
            anchors.right: parent.right
            anchors.rightMargin: window.margin
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12

            Text {
                text: window.editor.sizeText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }

            Rectangle { // the zoom: a click is Z (the whole picture, or 100 %)
                id: zoomButton
                width: zoomLabel.implicitWidth + 16
                height: 28
                radius: Theme.controlRadius
                color: zoomArea.pressed ? Theme.controlPressed : zoomArea.containsMouse ? Theme.controlHover : "transparent"

                Text {
                    id: zoomLabel
                    anchors.centerIn: parent
                    text: window.editor.zoomText
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
                MouseArea {
                    id: zoomArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        window.editor.toggleFit()
                        keys.forceActiveFocus()
                    }
                }
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("缩放")
            }
        }
    }

    Item { // the keys; no input method, so R, T, 1 are tool keys even with Chinese input on
        id: keys
        focus: true
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape && bar.closeKeys()) { // the key list first
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_Space) {
                if (!event.isAutoRepeat)
                    window.editor.spaceHeld(true)
                event.accepted = true
                return
            }
            event.accepted = window.editor.keyPressed(event.key, event.modifiers)
        }
        Keys.onReleased: (event) => {
            if (event.key === Qt.Key_Space && !event.isAutoRepeat) {
                window.editor.spaceHeld(false)
                event.accepted = true
            }
        }
    }

    WindowEdge {}
}
