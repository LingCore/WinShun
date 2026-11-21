import QtQuick

// One timer per window; short Ctrl gestures and other chords must not flash the rows.
//
// Letters (GitHub #175, 字母直达, off unless the setting is on): the key
// tapped by itself, down and up within tapMs with nothing else pressed,
// puts a letter on every row in view; the letter then opens (or pastes) its
// row with one hand, also the rows past the ninth. The tap again, Esc or
// any other key puts the numbers back. Which key (`trigger`) and which
// letters (`letters`, digits allowed) are the user's (GitHub #357); a key
// counts by its place on the keyboard (the virtual key), so Shift+1 is 1.
Item {
    id: hints

    property string mode: "held"
    property int key: -1
    property bool listening: false
    property bool held: false
    property bool ready: false
    // When number keys are off and letter jump is enabled, `mode` ("always" |
    // "held" | "off") previews the rows' letters calmly before the jump key is
    // tapped; tapping `jumpKey` arms `jumping` and switches them to keycaps.
    readonly property int hintKey: key >= 0 ? key : jumpEnabled ? jumpKey : -1
    readonly property bool shown: hintKey >= 0 && !jumping && !picking
                                  && (mode === "always" || (mode === "held" && listening && ready))
    // Keep opacity warm across armed <-> passive letter toggle in "always" mode
    // so cancelling jump mode swaps badge -> plain letter without blinking out.
    property real hintOpacity: shown || (key < 0 && jumpEnabled && mode === "always" && jumping) ? 1 : 0

    property bool jumpEnabled: false
    property string trigger: "auto" // auto: the number keys' modifier | ctrl | alt | shift
    readonly property int jumpKey: trigger === "ctrl" ? Qt.Key_Control : trigger === "alt" ? Qt.Key_Alt
                                 : trigger === "shift" ? Qt.Key_Shift : key >= 0 ? key : Qt.Key_Control
    readonly property string jumpKeyName: jumpKey === Qt.Key_Alt ? "Alt"
                                        : jumpKey === Qt.Key_Shift ? "Shift" : "Ctrl"
    property bool jumping: false
    // Shift+letter in the search box (GitHub #357): the letters are on the
    // buttons of the row it picked (the current one), not on the rows.
    property bool picking: false
    readonly property bool letterPreview: key < 0 && jumpEnabled && mode !== "off"
                                          && !jumping && !picking && (shown || hintOpacity > 0)
    readonly property bool lettersShown: jumping || letterPreview
    property bool tapArmed: false
    property double tapAt: 0
    readonly property int tapMs: 350
    // The first row's first; the home row unless the user chose others.
    property string letters: "ASDFGHJKLQWERTYUIOPZXCVBNM"

    Behavior on hintOpacity { NumberAnimation { duration: hints.shown ? 80 : 60 } }

    function reset() {
        delay.stop()
        held = false
        ready = false
    }
    // The letter of the row that many rows after the first in view; "" for none.
    function letterOf(offset: int): string {
        return lettersShown && offset >= 0 && offset < letters.length ? letters[offset] : ""
    }
    // The letter or digit of a key event ("" for none): by its virtual key,
    // so that Shift (or a layout) does not make 1 a "!"; by Qt's key for a
    // key that has none.
    function charOf(event): string {
        const vk = event.nativeVirtualKey
        if ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A))
            return String.fromCharCode(vk)
        if (vk >= 0x60 && vk <= 0x69) // the number pad's
            return String.fromCharCode(vk - 0x30)
        const key = event.key
        return (key >= Qt.Key_A && key <= Qt.Key_Z) || (key >= Qt.Key_0 && key <= Qt.Key_9) ? String.fromCharCode(key) : ""
    }
    // How many rows after the first in view that key's letter picks; -1: not one of the letters.
    function offsetOf(event): int {
        const c = charOf(event)
        return c.length > 0 ? letters.indexOf(c) : -1
    }
    function press(event) {
        if (!event.isAutoRepeat) {
            const tapModifier = jumpKey === Qt.Key_Control ? Qt.ControlModifier
                              : jumpKey === Qt.Key_Shift ? Qt.ShiftModifier : Qt.AltModifier
            tapArmed = jumpEnabled && listening && event.key === jumpKey && (event.modifiers & ~tapModifier) === 0
            if (tapArmed)
                tapAt = Date.now()
        }
        if (!listening || mode !== "held" || hintKey < 0 || event.isAutoRepeat)
            return
        if (event.key !== hintKey) {
            reset()
            return
        }
        const modifier = hintKey === Qt.Key_Control ? Qt.ControlModifier
                       : hintKey === Qt.Key_Shift ? Qt.ShiftModifier : Qt.AltModifier
        if ((event.modifiers & ~modifier) !== 0)
            return
        if (!held) {
            held = true
            delay.start()
        }
    }
    function release(event) {
        if (event.isAutoRepeat)
            return
        if (event.key === jumpKey) {
            if (tapArmed && jumpEnabled && listening && Date.now() - tapAt <= tapMs) {
                if (picking)
                    leave()
                else
                    jumping = !jumping
            }
            tapArmed = false
        }
        if (event.key === hintKey)
            reset()
    }
    function leave() {
        jumping = false
        picking = false
        tapArmed = false
    }
    function pick() {
        jumping = false
        picking = true
    }

    onListeningChanged: { reset(); leave() }
    onModeChanged: reset()
    onKeyChanged: { reset(); leave() }
    onTriggerChanged: { reset(); leave() }
    onLettersChanged: leave()
    onJumpEnabledChanged: { reset(); leave() }

    Timer {
        id: delay
        interval: 180
        onTriggered: hints.ready = hints.held && hints.listening
    }
}
