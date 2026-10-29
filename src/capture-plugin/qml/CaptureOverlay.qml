pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The screen capture over one monitor (src/capture, Capture): the frozen
// picture shown pixel for pixel, the window or control under the mouse
// outlined, the selection with its size, handles and toolbar, the
// magnifier; over the picture the annotations (AnnotationCanvas,
// AnnotateLayer) and the toolbar's two rows (AnnotateToolbar,
// AnnotateStylePanel). All the state is CaptureScreen's, in the monitor's
// physical pixels; px() and py() turn them into this window's logical units
// with Qt's real buffer scale, so edges land on whole screen pixels. No
// animation of the overlay itself: it is there at once and gone at once
// (only the toolbar's panel, its stepping aside and the hints fade).
Window {
    id: overlay

    required property CaptureSession session
    required property CaptureScreen screenModel

    readonly property real sx: screenModel.scaleX
    readonly property real sy: screenModel.scaleY
    readonly property real dpr: screenModel.dpr
    // Lines one logical pixel thick, as a whole number of screen pixels.
    readonly property int line: Math.max(1, Math.round(dpr))
    readonly property rect selection: screenModel.selection
    readonly property bool selectedHere: selection.width > 0
    readonly property rect hover: screenModel.hover
    readonly property bool hoverHere: hover.width > 0 && !selectedHere
    readonly property rect shown: selectedHere ? selection : hover
    // The annotations (one model for all monitors; drawn where the selection is).
    readonly property AnnotationModel annotation: session.annotation
    onSelectedHereChanged: if (!selectedHere) toolbar.manuallyPlaced = false

    function px(v) { return v * sx }
    function py(v) { return v * sy }

    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    color: "black"
    title: qsTr("截图")

    CaptureImage { // 1:1: the picture's pixels on the screen's
        x: 0
        y: 0
        width: overlay.px(overlay.screenModel.pixels.width)
        height: overlay.py(overlay.screenModel.pixels.height)
        image: overlay.screenModel.image
    }

    AnnotationCanvas { // the annotations, over the same picture with the same scale
        model: overlay.annotation
        visible: overlay.selectedHere && model !== null
        sparse: true
        scaleX: overlay.sx
        scaleY: overlay.sy
        x: 0
        y: 0
        width: overlay.px(overlay.screenModel.pixels.width)
        height: overlay.py(overlay.screenModel.pixels.height)
    }

    Item { // outside the hover or selection; all of the other monitors
        id: shade

        readonly property color tint: Qt.rgba(0, 0, 0, 0.45)
        readonly property bool clearHere: overlay.selectedHere || overlay.hoverHere
        readonly property real edgeLeft: overlay.px(overlay.shown.x)
        readonly property real edgeTop: overlay.py(overlay.shown.y)
        readonly property real edgeRight: overlay.px(overlay.shown.x + overlay.shown.width)
        readonly property real edgeBottom: overlay.py(overlay.shown.y + overlay.shown.height)

        anchors.fill: parent
        visible: overlay.screenModel.dimmed

        Rectangle {
            width: parent.width
            height: shade.clearHere ? shade.edgeTop : parent.height
            color: shade.tint
        }
        Rectangle {
            visible: shade.clearHere
            y: shade.edgeBottom
            width: parent.width
            height: parent.height - shade.edgeBottom
            color: shade.tint
        }
        Rectangle {
            visible: shade.clearHere
            y: shade.edgeTop
            width: shade.edgeLeft
            height: shade.edgeBottom - shade.edgeTop
            color: shade.tint
        }
        Rectangle {
            visible: shade.clearHere
            x: shade.edgeRight
            y: shade.edgeTop
            width: parent.width - shade.edgeRight
            height: shade.edgeBottom - shade.edgeTop
            color: shade.tint
        }
    }

    Item { // the window or control a click picks: an outline just inside it
        visible: overlay.hoverHere
        x: overlay.px(overlay.hover.x)
        y: overlay.py(overlay.hover.y)
        width: overlay.px(overlay.hover.width)
        height: overlay.py(overlay.hover.height)

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: overlay.px(overlay.line)
            border.color: Theme.accent
        }
    }

    Item { // the selection's frame, just outside it, and its eight handles
        id: frame

        readonly property int handle: Math.round(7 * overlay.dpr) | 1 // odd: centred on the edge line

        visible: overlay.selectedHere
        x: overlay.px(overlay.selection.x)
        y: overlay.py(overlay.selection.y)
        width: overlay.px(overlay.selection.width)
        height: overlay.py(overlay.selection.height)

        Rectangle {
            x: -overlay.px(overlay.line)
            y: -overlay.py(overlay.line)
            width: parent.width + 2 * overlay.px(overlay.line)
            height: parent.height + 2 * overlay.py(overlay.line)
            color: "transparent"
            border.width: overlay.px(overlay.line)
            border.color: Theme.accent
        }

        Repeater {
            // Corners and edge middles, as fractions of the selection.
            model: [[0, 0], [0.5, 0], [1, 0], [0, 0.5], [1, 0.5], [0, 1], [0.5, 1], [1, 1]]

            delegate: Rectangle {
                required property var modelData

                // Centred on the edge line (between two screen pixels), on whole pixels.
                readonly property int lineX: Math.round(overlay.selection.width * modelData[0])
                readonly property int lineY: Math.round(overlay.selection.height * modelData[1])

                x: overlay.px(lineX - Math.floor(frame.handle / 2))
                y: overlay.py(lineY - Math.floor(frame.handle / 2))
                width: overlay.px(frame.handle)
                height: overlay.py(frame.handle)
                color: Theme.accentFill
                border.width: overlay.px(1)
                border.color: Theme.accentText
            }
        }
    }

    AnnotateLayer { // the selected object's handles, the text box, words by the selection
        anchors.fill: parent
        session: overlay.session
        screenModel: overlay.screenModel
        annot: overlay.annotation
        keyTarget: keys
        here: overlay.selectedHere
        barRect: Qt.rect(toolbar.x + bar.x, toolbar.y + bar.y, bar.width, bar.height)
        blockRect: toolbar.visibleRows
        toolbarSide: toolbar.side
        toolbarShown: toolbar.visible
    }

    Rectangle { // the size of what is picked, in screen pixels
        id: sizeLabel

        readonly property point place: overlay.screenModel.placeSizeLabel(overlay.shown, width, height)

        visible: overlay.selectedHere || overlay.hoverHere
        x: Math.round(place.x * overlay.dpr) / overlay.dpr
        y: Math.round(place.y * overlay.dpr) / overlay.dpr
        width: sizeText.implicitWidth + 12
        height: sizeText.implicitHeight + 4
        radius: Theme.smallControlRadius
        color: Theme.accentFill

        Text {
            id: sizeText
            anchors.centerIn: parent
            text: qsTr("%1 × %2").arg(overlay.shown.width).arg(overlay.shown.height)
            color: Theme.accentText
            font.pixelSize: Theme.fontCaption
        }
    }

    Item { // the toolbar (AnnotateToolbar) and the style panel on its side away from the selection
        id: toolbar
        objectName: "captureToolbar"

        readonly property int gap: 4 // logical px between the two rows
        readonly property int panelHeight: 40 // AnnotateStylePanel's one row, logical px
        property bool manuallyPlaced: false
        property point manualPosition: Qt.point(0, 0)
        property point dragOrigin: Qt.point(0, 0)
        property string manualSide: "below"
        // Placed as two rows always, so the toolbar stays put when the panel comes and goes.
        readonly property point place: overlay.screenModel.placeToolbar(overlay.selection, bar.width, bar.height + gap + panelHeight)
        // Where placeToolbar put it: below the selection, above it, or inside at its bottom.
        readonly property string side: manuallyPlaced ? manualSide
                                      : place.y >= overlay.py(overlay.selection.y + overlay.selection.height) ? "below"
                                      : place.y + height <= overlay.py(overlay.selection.y) ? "above" : "inside"
        readonly property bool panelShown: overlay.annotation !== null && overlay.annotation.panelVisible
        // A drawing, moving or reshaping gesture reaching the rows in view
        // (3.5): the toolbar fades out of the way until it ends.
        readonly property bool stepAside: {
            const m = overlay.annotation
            if (!m || !m.gestureActive)
                return false
            const g = Qt.rect(overlay.px(m.gestureBounds.x), overlay.py(m.gestureBounds.y),
                              overlay.px(m.gestureBounds.width), overlay.py(m.gestureBounds.height))
            return meets(g, visibleRows)
        }
        // The rows on the screen now (logical px, window): the toolbar, and the panel when shown.
        readonly property rect visibleRows: panel.visible
            ? Qt.rect(x + Math.min(bar.x, panel.x), y + Math.min(bar.y, panel.y),
                      Math.max(bar.x + bar.width, panel.x + panel.width) - Math.min(bar.x, panel.x),
                      Math.max(bar.y + bar.height, panel.y + panel.height) - Math.min(bar.y, panel.y))
            : Qt.rect(x + bar.x, y + bar.y, bar.width, bar.height)

        function meets(a, b) {
            return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height
        }
        function bounded(value, available, extent) {
            return Math.round(Math.max(0, Math.min(value, available - extent)) * overlay.dpr) / overlay.dpr
        }
        function beginMove() {
            dragOrigin = Qt.point(x, y)
            manualPosition = dragOrigin
            manualSide = side
            manuallyPlaced = true
        }
        function moveBy(dx, dy) {
            manualPosition = Qt.point(bounded(dragOrigin.x + dx, overlay.width, width),
                                      bounded(dragOrigin.y + dy, overlay.height, height))
        }

        Connections {
            target: overlay.screenModel
            function onGeometryChanged() { toolbar.manuallyPlaced = false }
        }

        visible: overlay.selectedHere && !overlay.screenModel.adjusting && !overlay.session.toolbarHidden
        x: manuallyPlaced ? bounded(manualPosition.x, overlay.width, width) : Math.round(place.x * overlay.dpr) / overlay.dpr
        y: manuallyPlaced ? bounded(manualPosition.y, overlay.height, height) : Math.round(place.y * overlay.dpr) / overlay.dpr
        width: bar.width
        height: bar.height + gap + panelHeight
        opacity: stepAside ? 0 : 1
        enabled: !stepAside

        Behavior on opacity { NumberAnimation { duration: 100 } }

        AnnotateToolbar {
            id: bar
            objectName: "captureToolbarBar"
            y: toolbar.side === "below" ? 0 : toolbar.panelHeight + toolbar.gap
            movable: true
            keyTarget: keys
            session: overlay.session
            annot: overlay.annotation
            dpr: overlay.dpr
            blockRect: toolbar.visibleRows
            opensDown: toolbar.side === "below"
            onMoveStarted: {
                overlay.session.mouseLeft(overlay.screenModel.index)
                toolbar.beginMove()
            }
            onMoveBy: (dx, dy) => toolbar.moveBy(dx, dy)
        }

        AnnotateSmartHint { // 智能打码's offer, in the style panel's place while no tool is on
            annot: overlay.annotation
            dpr: overlay.dpr
            visible: !toolbar.panelShown && found > 0
            x: Math.round(Math.max(4 - toolbar.x, Math.min(0, overlay.width - 4 - toolbar.x - width)) * overlay.dpr) / overlay.dpr
            y: panel.y + Math.round((toolbar.panelHeight - height) / 2 * overlay.dpr) / overlay.dpr
        }

        AnnotateStylePanel {
            id: panel
            visible: toolbar.panelShown
            // Left edges together; kept on the screen when it is wider than the toolbar.
            x: Math.round(Math.max(4 - toolbar.x, Math.min(0, overlay.width - 4 - toolbar.x - width)) * overlay.dpr) / overlay.dpr
            y: toolbar.side === "below" ? bar.height + toolbar.gap : 0
            height: toolbar.panelHeight
            annot: overlay.annotation
            dpr: overlay.dpr
            direction: toolbar.side === "below" ? 1 : -1
        }
    }

    Rectangle { // the magnifier: the pixels round the cursor, where it is, its colour
        id: magnifier

        readonly property int cells: 15 // CaptureScreen::kLoupeCells
        readonly property int cell: Math.max(1, Math.round(11 * overlay.dpr)) // screen pixels per picture pixel
        readonly property point place: overlay.screenModel.placeMagnifier(overlay.screenModel.cursor, width, height)
        // The edge in whole screen pixels, the picture just inside it: at 150% a
        // 1 px edge is 1.5 pixels, and the picture covered half of the right one.
        readonly property real edge: Math.max(1, Math.round(overlay.dpr)) / overlay.dpr

        visible: overlay.screenModel.magnifierShown && overlay.screenModel.hasImage
        x: Math.round(place.x * overlay.dpr) / overlay.dpr
        y: Math.round(place.y * overlay.dpr) / overlay.dpr
        // As wide as the picture, which fills it to the edges (a wider box left an empty strip at the right); the lines below elide
        width: loupe.width + 2 * edge
        height: loupe.height + 2 * edge + info.implicitHeight + 10
        color: Theme.background
        border.width: edge
        border.color: Theme.windowEdge

        CaptureLoupe {
            id: loupe
            objectName: "captureLoupe"
            x: magnifier.edge
            y: magnifier.edge
            width: overlay.px(magnifier.cells * magnifier.cell)
            height: overlay.py(magnifier.cells * magnifier.cell)
            image: overlay.screenModel.loupe

            Rectangle { // the cross through the middle row and column
                x: 0
                y: overlay.py((magnifier.cells - 1) / 2 * magnifier.cell)
                width: parent.width
                height: overlay.py(magnifier.cell)
                color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.25)
            }
            Rectangle {
                x: overlay.px((magnifier.cells - 1) / 2 * magnifier.cell)
                y: 0
                width: overlay.px(magnifier.cell)
                height: parent.height
                color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.25)
            }
            Rectangle { // the pixel under the cursor
                x: overlay.px((magnifier.cells - 1) / 2 * magnifier.cell)
                y: overlay.py((magnifier.cells - 1) / 2 * magnifier.cell)
                width: overlay.px(magnifier.cell)
                height: overlay.py(magnifier.cell)
                color: "transparent"
                border.width: overlay.px(1)
                border.color: Theme.accentText
            }
        }

        Column {
            id: info
            x: 8
            y: Theme.snap(loupe.y + loupe.height + 5, overlay.dpr) // 5 px is 7.5 device pixels: the lines of text on a half one
            width: parent.width - 16
            spacing: 2

            Text { // the screen pixel, in the virtual desktop's coordinates
                id: position
                width: parent.width
                elide: Text.ElideRight
                text: qsTr("位置 %1, %2").arg(overlay.screenModel.cursor.x + overlay.screenModel.origin.x)
                                          .arg(overlay.screenModel.cursor.y + overlay.screenModel.origin.y)
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
            Row {
                id: colorRow
                spacing: 6

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    color: overlay.screenModel.color.length > 0 ? overlay.screenModel.color : "transparent"
                    border.width: 1
                    border.color: Theme.windowEdge
                }
                Text {
                    width: Math.min(implicitWidth, info.width - 18)
                    elide: Text.ElideRight
                    text: overlay.screenModel.colorText
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
            }
            Text {
                id: hint
                width: parent.width
                text: overlay.screenModel.flash.length > 0 ? overlay.screenModel.flash : qsTr("C 复制 · Shift 换格式")
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
        }
    }

    CapturePointer { // under the toolbar: its buttons take their own clicks
        anchors.fill: parent
        z: -1
        shape: overlay.screenModel.cursorShape
        dpr: overlay.dpr

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: undefined // inherit CapturePointer's RGBA crosshair
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton | Qt.BackButton | Qt.ForwardButton
            onPositionChanged: (mouse) => overlay.session.mouseMoved(overlay.screenModel.index, mouse.x, mouse.y)
            onExited: overlay.session.mouseLeft(overlay.screenModel.index) // onto the toolbar: no magnifier there
            onEntered: overlay.session.mouseMoved(overlay.screenModel.index, mouseX, mouseY) // the toolbar gone from under it too
            onPressed: (mouse) => overlay.session.mousePressed(overlay.screenModel.index, mouse.x, mouse.y, mouse.button)
            onReleased: (mouse) => overlay.session.mouseReleased(overlay.screenModel.index, mouse.x, mouse.y, mouse.button)
            // Only the left button's: a second middle or side click is two presses of their own
            onDoubleClicked: (mouse) => {
                if (mouse.button === Qt.LeftButton)
                    overlay.session.mouseDoubleClicked(overlay.screenModel.index, mouse.x, mouse.y)
            }
            onWheel: (wheel) => overlay.session.wheeled(overlay.screenModel.index, wheel.x, wheel.y, wheel.angleDelta.y)
        }
    }

    Item { // the keys; no input method here, so R, T, 1 reach the session even with Chinese input on
        id: keys
        focus: true
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape && bar.closeKeys()) { // the key list first
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_Shift && event.isAutoRepeat) { // held down: still the one press
                event.accepted = true
                return
            }
            event.accepted = overlay.session.keyPressed(event.key, event.modifiers)
        }
        Keys.onReleased: (event) => {
            if (!event.isAutoRepeat)
                overlay.session.keyReleased(event.key) // a Shift alone: the colour's next format
        }
    }
}
