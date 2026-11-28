import QtQuick

// The caret of a field whose style gives it a colour of its own (the
// searchField's or inputField's caretColor): TextInput { cursorDelegate:
// Component { Caret { color: …; dpr: … } } }. A delegate replaces Qt's caret
// whole, blinking and all, so this one blinks as Qt's does (the system's
// cursor flash time), solid again on each move, and shows only while the
// field takes typing (cursorVisible); steady when that is without the focus.
Rectangle {
    id: caret

    property real dpr: 1
    // Not "field": the hosts name their part hooks so, and a binding in
    // Caret { … } would see this property first.
    readonly property TextInput textInput: parent as TextInput
    readonly property bool wanted: textInput !== null && textInput.cursorVisible
    readonly property int flash: Qt.styleHints.cursorFlashTime

    // A host that draws its selection itself (SearchBar) gives the caret the
    // same height and top (markHeight > 0, markY in the field's own
    // coordinates): then the two always match. Otherwise the caret is as tall
    // as Qt's own, on its line.
    property real markHeight: 0
    property real markY: 0
    // TextInput places only x (one pixel before the character); the rest as its own caret.
    y: textInput ? textInput.cursorRectangle.y : 0
    // A transform, not x and y: the TextInput sets the delegate's position
    // itself whenever the caret moves. Across, onto the nearest whole device
    // pixel of the window (align()): one device pixel wide, a caret half a
    // pixel off lit the pixel before; at the start of a field that began
    // half a pixel in (125 %), that pixel was outside the field's clip and
    // the empty box showed no caret at all (GitHub #501).
    transform: Translate {
        x: caret.shift
        y: caret.markHeight > 0 && caret.textInput ? caret.markY - caret.textInput.cursorRectangle.y : 0
    }
    property real shift: 0
    function align() {
        if (!textInput || dpr <= 0)
            return
        const device = textInput.mapToItem(null, x, 0).x * dpr
        shift = (Math.round(device) - device) / dpr
    }
    onXChanged: align()
    onDprChanged: align()
    width: Math.max(1, Math.round(dpr)) / dpr
    height: markHeight > 0 ? markHeight : textInput ? textInput.cursorRectangle.height : 0
    visible: wanted && lit
    property bool lit: true

    function wake() {
        align() // the field may have moved since (shown again, another size)
        lit = true
        if (blink.running)
            blink.restart()
    }
    onWantedChanged: wake()

    Timer {
        id: blink
        interval: Math.max(1, caret.flash / 2)
        repeat: true
        running: caret.wanted && caret.flash > 0 && caret.textInput.activeFocus
        onTriggered: caret.lit = !caret.lit
    }
    Connections {
        target: caret.textInput
        function onCursorPositionChanged() { caret.wake() }
        function onTextChanged() { caret.wake() }
        function onActiveFocusChanged() { caret.wake() }
    }
}
