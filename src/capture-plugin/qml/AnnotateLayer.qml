pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// What lies over the annotation canvas in a capture overlay (docs/design-
// capture-roadmap.md, 3.5-3.9): the selected objects, the text box and the
// number box (AnnotateObjects), and the short words by the selection (the
// first tool's hint, "H shows the toolbar", the session's notices).
// Positions from the model are image px (the monitor's physical pixels);
// px() and py() make them this window's logical units, as CaptureOverlay
// does. Nothing here takes the mouse except the objects' boxes and the word
// bars: the rest passes to the overlay's MouseArea beneath.
Item {
    id: over

    required property CaptureSession session
    required property CaptureScreen screenModel
    required property AnnotationModel annot
    // The overlay's key handler: focus goes back to it when typing ends.
    required property Item keyTarget
    // The selection is on this monitor.
    property bool here: false
    // The toolbar's first row and its rows on the screen now (with the
    // style panel when it shows), logical px in this window, and where it
    // sits: "below", "above" or "inside" the selection. Shown:
    // CaptureOverlay shows it now.
    property rect barRect
    property rect blockRect
    property string toolbarSide: "below"
    property bool toolbarShown: false

    // ---- geometry, logical px unless named image px ----
    readonly property int hintGap: 8 // a word bar off the toolbar or the selection
    readonly property int margin: 4 // a word bar off the screen's edge
    readonly property int noticeTop: 16 // a notice with no selection here: from the top
    readonly property int barHeight: 28 // a word bar
    readonly property int barPad: 10 // each side of a word bar's words
    readonly property int hintMs: 6000 // the first tool's hint stays this long
    readonly property int fadeInMs: 120
    readonly property int fadeOutMs: 200

    readonly property real sx: screenModel.scaleX
    readonly property real sy: screenModel.scaleY
    readonly property real dpr: screenModel.dpr
    readonly property rect selection: screenModel.selection

    function px(v) { return v * sx }
    function py(v) { return v * sy }
    function snap(v) { return Math.round(v * dpr) / dpr }
    function clampX(x, w) { return Math.max(margin, Math.min(x, width - w - margin)) }
    function clampY(y, h) { return Math.max(margin, Math.min(y, height - h - margin)) }

    // ---- the selected object, the text box, the number box ----

    AnnotateObjects {
        anchors.fill: parent
        annot: over.annot
        keyTarget: over.keyTarget
        here: over.here
        sx: over.sx
        sy: over.sy
        dpr: over.dpr
        onKeyForwarded: (key, modifiers) => over.session.keyPressed(key, modifiers)
    }

    // ---- words by the toolbar and the selection ----

    // The first time a tool is chosen (once ever: session.toolHintSeen):
    // left of the toolbar, or past it on the side away from the selection.
    FadeBar {
        id: toolHint

        readonly property bool leftRoom: over.barRect.x - over.hintGap - width >= over.margin

        allowed: over.here && over.toolbarShown
        words: qsTr("再按一次关掉 · [ ] 改粗细 · H 隐藏工具条")
        x: over.snap(leftRoom ? over.barRect.x - over.hintGap - width
                               : over.clampX(over.barRect.x + over.barRect.width - width, width))
        y: over.snap(leftRoom ? over.barRect.y + (over.barRect.height - height) / 2
                               : over.clampY(over.toolbarSide === "below" ? over.blockRect.y + over.blockRect.height + over.hintGap
                                                                            : over.blockRect.y - over.hintGap - height, height))
        onClicked: shown = false
    }

    Timer {
        id: hintTimer
        interval: over.hintMs
        onTriggered: toolHint.shown = false
    }

    Connections {
        target: over.annot
        function onToolChanged() {
            if (!over.here)
                return
            if (over.annot.tool < 0) {
                hintTimer.stop()
                toolHint.shown = false
            } else if (!over.session.toolHintSeen && over.toolbarShown) {
                over.session.toolHintSeen = true
                toolHint.shown = true
                hintTimer.restart()
            }
        }
    }

    // H hid the toolbar: where it was, a word on how to have it back.
    FadeBar {
        allowed: over.here
        shown: over.here && over.session.toolbarHidden && !over.screenModel.adjusting
        words: qsTr("按 H 显示工具条")
        x: over.snap(over.clampX(over.barRect.x + over.barRect.width - width, width))
        y: over.snap(over.clampY(over.toolbarSide === "below" ? over.barRect.y
                                                                 : over.barRect.y + over.barRect.height - height, height))
    }

    // The session's word for the user (screenModel.notice), on the side of
    // the selection away from the toolbar; with no selection here, at the
    // top of the screen.
    FadeBar {
        id: noticeBar

        readonly property string notice: over.screenModel.notice
        readonly property real selTop: over.py(over.selection.y)
        readonly property real selBottom: over.py(over.selection.y + over.selection.height)
        readonly property bool under: over.toolbarSide === "above"

        shown: notice.length > 0
        x: over.snap(over.here ? over.clampX(over.px(over.selection.x + over.selection.width / 2) - width / 2, width)
                                 : (over.width - width) / 2)
        y: over.snap(!over.here ? over.noticeTop
                      : under ? (selBottom + over.hintGap + height <= over.height - over.margin ? selBottom + over.hintGap
                                                                                                   : selBottom - over.hintGap - height)
                      : (selTop - over.hintGap - height >= over.margin ? selTop - over.hintGap - height
                                                                         : selTop + over.hintGap))
        // The words stay while it fades out.
        onNoticeChanged: if (notice.length > 0) words = notice
        Component.onCompleted: if (notice.length > 0) words = notice
    }

    // A short line of words in a small card (Theme.background, a 1 px grey
    // edge): fades in over fadeInMs, out over fadeOutMs, the fade-out at once.
    component FadeBar: Rectangle {
        id: fade

        property string words
        property bool shown: false
        property bool allowed: true

        signal clicked()

        width: Math.ceil((label.implicitWidth + 2 * over.barPad) / 4) * 4
        height: over.barHeight
        radius: Theme.controlRadius
        color: Theme.background
        border.width: 1
        border.color: Theme.windowEdge
        opacity: 0
        visible: opacity > 0 && allowed
        Component.onCompleted: if (shown) opacity = 1
        onShownChanged: {
            if (shown) {
                fadeOut.stop()
                fadeIn.restart()
            } else {
                fadeIn.stop()
                fadeOut.restart()
            }
        }

        NumberAnimation { id: fadeIn; target: fade; property: "opacity"; to: 1; duration: over.fadeInMs }
        NumberAnimation { id: fadeOut; target: fade; property: "opacity"; to: 0; duration: over.fadeOutMs }

        Text {
            id: label
            anchors.centerIn: parent
            text: fade.words
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }

        MouseArea { // its presses stay with it, not on the picture beneath
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.ArrowCursor
            onClicked: fade.clicked()
        }
    }
}
