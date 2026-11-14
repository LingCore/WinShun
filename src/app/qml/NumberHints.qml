import QtQuick

// One timer per window; short Ctrl gestures and other chords must not flash the rows.
//
// Letters (GitHub #175, 字母直达, off unless the setting is on): the key
// tapped by itself, down and up within tapMs with nothing else pressed,
// puts a letter on every row in view; the letter then opens (or pastes) its
// row with one hand, also the rows past the ninth. The tap again, Esc or
// any other key puts the numbers back.
Item {
    id: hints

    property string mode: "held"
    property int key: -1
    property bool listening: false
    property bool held: false
    property bool ready: false
    readonly property bool shown: key >= 0 && !jumping && (mode === "always" || (mode === "held" && listening && ready))
    property real hintOpacity: shown ? 1 : 0

    property bool jumpEnabled: false
    readonly property int jumpKey: key >= 0 ? key : Qt.Key_Control
    property bool jumping: false
    property bool tapArmed: false
    property double tapAt: 0
    readonly property int tapMs: 350
    // The home row first: the nearest keys for the first rows.
    readonly property string letters: "ASDFGHJKLQWERTYUIOPZXCVBNM"

    Behavior on hintOpacity { NumberAnimation { duration: hints.shown ? 80 : 60 } }

    function reset() {
        delay.stop()
        held = false
        ready = false
    }
    // The letter of the row that many rows after the first in view; "" for none.
    function letterOf(offset: int): string {
        return jumping && offset >= 0 && offset < letters.length ? letters[offset] : ""
    }
    // How many rows after the first in view that key's letter picks; -1: not a letter.
    function offsetOf(key: int): int {
        return key >= Qt.Key_A && key <= Qt.Key_Z ? letters.indexOf(String.fromCharCode(key)) : -1
    }
    function press(event) {
        if (!event.isAutoRepeat) {
            const tapModifier = jumpKey === Qt.Key_Control ? Qt.ControlModifier : Qt.AltModifier
            tapArmed = jumpEnabled && listening && event.key === jumpKey && (event.modifiers & ~tapModifier) === 0
            if (tapArmed)
                tapAt = Date.now()
        }
        if (!listening || mode !== "held" || key < 0 || event.isAutoRepeat)
            return
        if (event.key !== key) {
            reset()
            return
        }
        const modifier = key === Qt.Key_Control ? Qt.ControlModifier : Qt.AltModifier
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
            if (tapArmed && jumpEnabled && listening && Date.now() - tapAt <= tapMs)
                jumping = !jumping
            tapArmed = false
        }
        if (event.key === key)
            reset()
    }
    function leave() {
        jumping = false
        tapArmed = false
    }

    onListeningChanged: { reset(); leave() }
    onModeChanged: reset()
    onKeyChanged: { reset(); leave() }
    onJumpEnabledChanged: leave()

    Timer {
        id: delay
        interval: 180
        onTriggered: hints.ready = hints.held && hints.listening
    }
}
