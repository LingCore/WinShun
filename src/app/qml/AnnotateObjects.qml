pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// What lies on the picture of the annotations themselves (docs/design-
// capture-roadmap.md, 3.5-3.9), in a capture overlay (AnnotateLayer) and in
// the editing window (EditWindow): the selected objects' outlines and
// handles, the text box while a text is typed, the small box that renumbers
// a number. Positions from the model are image px; px() and py() make them
// logical units of this item with the picture's scale (sx, sy: logical px
// per image px, the zoom included); unitX and unitY are one device pixel
// in logical px, which borders and handles keep whatever the zoom. Nothing
// here takes the mouse except the text box and the number box: the rest
// passes to what lies beneath.
Item {
    id: over

    required property AnnotationModel annot
    // The key handler: focus goes back to it when typing ends.
    required property Item keyTarget
    // The annotations are shown here (the selection is on this monitor).
    property bool here: true
    property real sx: 1
    property real sy: 1
    property real dpr: 1
    // One device pixel, logical px: the scale itself when one image pixel is one.
    property real unitX: sx
    property real unitY: sy

    // Ctrl+S and F3 pressed in the text box, for whoever owns the keys.
    signal keyForwarded(int key, int modifiers)

    // ---- geometry, logical px unless named image px ----
    readonly property int margin: 4 // a box off the item's edge
    readonly property int numberWidth: 64 // the renumbering box
    readonly property int numberHeight: 32
    readonly property int numberPad: 6 // inside it, each side
    // Handles: an odd number of device pixels, centred on the point, as the
    // selection's own (CaptureOverlay's frame).
    readonly property int handle: Math.round(7 * dpr) | 1

    function px(v) { return v * sx }
    function py(v) { return v * sy }
    function snap(v) { return Math.round(v * dpr) / dpr }
    function clampX(x, w) { return Math.max(margin, Math.min(x, width - w - margin)) }
    function clampY(y, h) { return Math.max(margin, Math.min(y, height - h - margin)) }

    // ---- the selected object ----

    Repeater {
        model: over.here && over.annot ? over.annot.selection : []

        delegate: Item {
            id: picked

            required property var modelData
            // The outline's pixels (image px), just outside the object's bounds.
            readonly property int edgeLeft: Math.floor(modelData.x)
            readonly property int edgeTop: Math.floor(modelData.y)
            readonly property int edgeRight: Math.ceil(modelData.x + modelData.width)
            readonly property int edgeBottom: Math.ceil(modelData.y + modelData.height)

            Rectangle { // a line has only its two end handles
                visible: picked.modelData.box || picked.modelData.handles.length === 0
                x: over.px(picked.edgeLeft) - over.unitX
                y: over.py(picked.edgeTop) - over.unitY
                width: over.px(picked.edgeRight - picked.edgeLeft) + 2 * over.unitX
                height: over.py(picked.edgeBottom - picked.edgeTop) + 2 * over.unitY
                color: "transparent"
                border.width: over.unitX
                border.color: Theme.accent
            }

            Repeater {
                model: picked.modelData.curve && picked.modelData.handles.length === 4 ? [0, 1] : []
                delegate: Rectangle {
                    required property int modelData
                    readonly property var endpoint: picked.modelData.handles[modelData]
                    readonly property var control: picked.modelData.handles[modelData + 2]
                    readonly property real dx: over.px(control.x - endpoint.x)
                    readonly property real dy: over.py(control.y - endpoint.y)
                    x: over.px(endpoint.x)
                    y: over.py(endpoint.y)
                    width: Math.hypot(dx, dy)
                    height: over.unitY
                    rotation: Math.atan2(dy, dx) * 180 / Math.PI
                    transformOrigin: Item.TopLeft
                    color: Theme.accent
                }
            }

            Repeater {
                model: picked.modelData.handles

                delegate: Rectangle {
                    required property var modelData

                    // Centred on the point: a box's edge (a whole pixel line)
                    // or a line's end (a pixel's centre), on whole pixels.
                    // The handle is a screen size: at another zoom it spans
                    // another number of image pixels (the same, 1, in the overlay).
                    x: over.px(Math.round(modelData.x - over.handle / 2 * over.unitX / over.sx))
                    y: over.py(Math.round(modelData.y - over.handle / 2 * over.unitY / over.sy))
                    width: over.handle * over.unitX
                    height: over.handle * over.unitY
                    radius: modelData.control ? width / 2 : 0
                    color: modelData.control ? Theme.background : Theme.accentFill
                    border.width: over.unitX
                    border.color: modelData.control ? Theme.accent : Theme.accentText
                }
            }
        }
    }

    // ---- the text being typed ----

    Item {
        id: textBox

        readonly property var ed: over.annot ? over.annot.editing : ({})
        readonly property bool active: over.here && ed.fontPx !== undefined
        // A new edit: another text, or the same one again after it ended.
        readonly property string editKey: active ? ed.id + "," + ed.x + "," + ed.y : ""
        // Image px, as the compositor measures the text (annot::layoutText).
        readonly property real descent: ed.descent !== undefined ? ed.descent : (ed.pitch ?? 0) - (ed.ascent ?? 0)
        readonly property int lines: Math.max(1, edit.lineCount)
        readonly property int inkWidth: Math.ceil(edit.contentWidth / over.sx - 0.01)

        function start() {
            // Set once: the model's map changes again while typing (a style
            // change on the panel), and setting the words again would move
            // the caret and drop what the input method is composing.
            edit.text = ed.text ?? ""
            Qt.callLater(textBox.focusCaret) // once the font and the layout follow the new map
        }

        // The window active, the box focused, the caret where the press was
        // (at the end for a new text).
        function focusCaret() {
            if (!active)
                return
            const w = over.Window.window
            if (w)
                w.requestActivate()
            edit.forceActiveFocus()
            if (ed.clickX < 0)
                edit.cursorPosition = edit.length
            else
                edit.cursorPosition = edit.positionAt(over.px(ed.x + ed.clickX) - edit.x, over.py(ed.y + ed.clickY) - edit.y)
        }

        function end() {
            edit.clear()
            over.keyTarget.forceActiveFocus()
        }

        anchors.fill: parent
        visible: active
        onEditKeyChanged: editKey.length > 0 ? start() : end()

        Rectangle { // the box behind boxed words, as the compositor will draw it
            visible: textBox.ed.boxed === true
            x: over.px(textBox.ed.x - textBox.ed.pad)
            y: over.py(textBox.ed.y - textBox.ed.pad)
            width: over.px(textBox.inkWidth + 2 * textBox.ed.pad)
            height: over.py(textBox.ed.ascent + textBox.descent + (textBox.lines - 1) * textBox.ed.pitch + 2 * textBox.ed.pad)
            radius: over.px(textBox.ed.radius)
            color: textBox.ed.fill ?? "transparent"
        }

        // The outline the words get when committed, under the glyphs: copies of
        // the words round a circle of the stroke's radius, the same font and place
        // as the box, so the ring moves with the typed glyphs (the committed one is the
        // compositor's; the box and the compositor already differ by one device pixel vertically).
        Repeater {
            id: ring

            readonly property real radius: textBox.active ? (textBox.ed.stroke ?? 0) * over.sx : 0
            // Copies a pixel or so apart on the circle; a wide outline (4 image px and more) also gets half a ring
            // inside it, which closes the small gaps the circle leaves at the dots of i and j and at full stops.
            readonly property int outer: radius > 0 ? Math.max(12, Math.min(64, Math.ceil(2 * Math.PI * radius))) : 0
            readonly property int inner: radius > 0 && (textBox.ed.stroke ?? 0) >= 4 ? Math.ceil(outer / 2) : 0

            model: outer + inner
            delegate: Text {
                required property int index
                readonly property bool onInner: index >= ring.outer
                readonly property real angle: onInner ? 2 * Math.PI * (index - ring.outer + 0.5) / ring.inner
                                                      : 2 * Math.PI * index / ring.outer
                readonly property real reach: onInner ? ring.radius / 2 : ring.radius

                x: edit.x + Math.cos(angle) * reach
                y: edit.y + Math.sin(angle) * reach
                text: edit.text
                textFormat: Text.PlainText
                renderType: Text.NativeRendering
                font: edit.font
                color: textBox.ed.strokeColor ?? "#FFFFFF"
            }
        }

        TextEdit {
            id: edit

            // The first baseline where the compositor puts it (top + ascent,
            // image px). Later lines keep Qt's own spacing, which may differ
            // from the model's pitch by a fraction of a pixel per line.
            x: over.px(textBox.ed.x ?? 0)
            y: over.py((textBox.ed.y ?? 0) + (textBox.ed.ascent ?? 0)) - metrics.ascent
            textFormat: TextEdit.PlainText
            renderType: TextEdit.NativeRendering
            wrapMode: TextEdit.NoWrap
            font.family: over.annot ? over.annot.fontFamily : ""
            // In points: font.pixelSize takes whole logical px only (3.8).
            // Not followed while nothing is typed: at each zoom of the editing window the
            // text box would load a font of another size (60 ms the first time) for nothing.
            font.pointSize: textBox.active ? Math.max(1, (textBox.ed.fontPx ?? 18) * over.sx * 0.75) : 12
            font.bold: textBox.ed.bold === true
            font.hintingPreference: Font.PreferVerticalHinting // as annot::annotationFont
            color: textBox.ed.color ?? "#000000"
            selectionColor: Theme.textSelection
            selectedTextColor: color

            // The model reads the words from here when the session commits
            // them (a press outside, another tool, Ctrl+S, F3, the hotkey).
            onTextChanged: if (textBox.active && over.annot) over.annot.setEditingText(text)

            Keys.onPressed: (event) => {
                const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
                const shift = (event.modifiers & Qt.ShiftModifier) !== 0
                const enter = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                // A number's words are one line: Enter ends them (Shift+Enter breaks the line).
                if (enter && !ctrl && edit.inputMethodComposing)
                    return // the input method's: it takes the pinyin
                if ((ctrl && enter) || (enter && !shift && textBox.ed.number === true)) {
                    over.annot.commitText(edit.text)
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    if (edit.inputMethodComposing)
                        return // the input method's: it drops the pinyin
                    over.annot.commitText(edit.text)
                    event.accepted = true
                } else if ((ctrl && event.key === Qt.Key_S) || event.key === Qt.Key_F3) {
                    const key = event.key
                    const modifiers = event.modifiers
                    over.annot.commitText(edit.text)
                    over.keyForwarded(key, modifiers)
                    event.accepted = true
                }
            }
        }

        Text { // an empty number's words: what Esc does (tool keys type into the box, so Esc first)
            visible: textBox.active && textBox.ed.number === true && edit.length === 0 && edit.preeditText.length === 0
            x: edit.x + 4 // clear of the caret
            y: edit.y
            text: qsTr("输入文字，Esc 跳过")
            color: Theme.subtext
            font.family: edit.font.family
            font.pointSize: edit.font.pointSize
            font.hintingPreference: Font.PreferVerticalHinting
            renderType: Text.NativeRendering
        }

        FontMetrics {
            id: metrics
            font: edit.font
        }
    }

    // ---- a number being renumbered (double click) ----

    Rectangle {
        id: numberBox

        readonly property var ne: over.annot ? over.annot.numberEdit : ({})
        readonly property bool active: over.here && ne.id !== undefined
        readonly property string editKey: active ? String(ne.id) : ""

        visible: active
        x: over.snap(over.clampX(over.px(ne.x ?? 0) - width / 2, width))
        y: over.snap(over.clampY(over.py(ne.y ?? 0) - height / 2, height))
        width: over.numberWidth
        height: over.numberHeight
        radius: Theme.controlRadius
        color: Theme.background
        border.width: 1
        border.color: Theme.windowEdge
        onEditKeyChanged: {
            if (editKey.length > 0) {
                numberInput.text = String(ne.shown ?? "")
                Qt.callLater(numberBox.focusInput)
            } else if (numberInput.activeFocus) {
                over.keyTarget.forceActiveFocus()
            }
        }

        function focusInput() {
            if (!active)
                return
            const w = over.Window.window
            if (w)
                w.requestActivate()
            numberInput.forceActiveFocus()
            numberInput.selectAll()
        }

        MouseArea { // presses on the box stay with it
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.IBeamCursor
            onPressed: numberInput.forceActiveFocus()
        }

        TextInput {
            id: numberInput
            anchors.fill: parent
            leftPadding: over.numberPad
            rightPadding: over.numberPad
            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
            validator: IntValidator { bottom: 0; top: 99999 }
            inputMethodHints: Qt.ImhDigitsOnly
            selectByMouse: true
            color: Theme.text
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            font.pixelSize: Theme.fontBody

            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    // Empty: counted again (setNumber takes "").
                    const t = numberInput.text.trim()
                    over.annot.setNumber(numberBox.ne.id, t.length > 0 ? parseInt(t, 10) : "")
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    over.annot.closeNumberEdit()
                    event.accepted = true
                }
            }
            // Focus gone elsewhere (another window, a press the model did
            // not see): the box closes, as a press on the picture closes it.
            onActiveFocusChanged: if (!activeFocus && numberBox.active) over.annot.closeNumberEdit()
        }
    }
}
