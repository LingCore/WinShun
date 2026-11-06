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
    // A transform, not y: the TextInput sets the delegate's position itself whenever the caret moves.
    transform: Translate { y: caret.markHeight > 0 && caret.textInput ? caret.markY - caret.textInput.cursorRectangle.y : 0 }
    width: Math.max(1, Math.round(dpr)) / dpr
    height: markHeight > 0 ? markHeight : textInput ? textInput.cursorRectangle.height : 0
    visible: wanted && lit
    property bool lit: true

    function wake() {
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
