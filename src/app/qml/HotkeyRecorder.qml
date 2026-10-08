import QtQuick
import WinShun

// Click, then press the key combination. Esc cancels, Backspace clears.
// While it records, the keys come straight from the keyboard (SettingsEditor
// starts a ShortcutCapture), so that Alt+Space, Win+… and combinations other
// programs registered arrive here too.
Rectangle {
    id: recorder

    property string hotkey
    readonly property bool recording: activeFocus
    property string partial // modifiers held so far, or a hint
    property bool partialIsHint: false

    signal recorded(string hotkey)

    function modifierNames(modifiers) {
        const names = []
        if (modifiers & Qt.ControlModifier) names.push("Ctrl")
        if (modifiers & Qt.AltModifier) names.push("Alt")
        if (modifiers & Qt.ShiftModifier) names.push("Shift")
        if (modifiers & Qt.MetaModifier) names.push("Win")
        return names
    }

    // The key part, in the spelling the app's hotkey parser understands.
    function keyName(key) {
        if (key >= Qt.Key_A && key <= Qt.Key_Z) return String.fromCharCode(key)
        if (key >= Qt.Key_0 && key <= Qt.Key_9) return String.fromCharCode(key)
        if (key >= Qt.Key_F1 && key <= Qt.Key_F24) return "F" + (key - Qt.Key_F1 + 1)
        if (key === Qt.Key_Space) return "Space"
        if (key === Qt.Key_QuoteLeft || key === Qt.Key_AsciiTilde) return "`"
        const shiftedDigit = ")!@#$%^&*(".indexOf(String.fromCharCode(key)) // from Qt, without the capture, Shift+1 arrives as "!"
        if (key < 128 && shiftedDigit >= 0) return String(shiftedDigit)
        return ""
    }

    function stop() {
        partial = ""
        partialIsHint = false
        focus = false
    }

    implicitWidth: Math.max(150, label.implicitWidth + 28)
    implicitHeight: 36
    radius: 4
    activeFocusOnTab: true
    color: recording ? Theme.inputFocused : area.containsMouse ? Theme.controlHover : Theme.control
    border.width: recording ? 2 : 1
    border.color: recording ? Theme.accent : Theme.controlBorder

    onRecordingChanged: if (!recording) { partial = ""; partialIsHint = false }

    Keys.onPressed: (event) => {
        event.accepted = true
        const mods = modifierNames(event.modifiers)
        switch (event.key) {
        case Qt.Key_Control:
        case Qt.Key_Alt:
        case Qt.Key_Shift:
        case Qt.Key_Meta:
            partial = mods.join(" + ") + " + …"
            partialIsHint = false
            return
        case Qt.Key_Escape:
            stop()
            return
        case Qt.Key_Backspace:
        case Qt.Key_Delete:
            if (mods.length === 0) {
                recorded("")
                stop()
                return
            }
            break
        case Qt.Key_Tab:
        case Qt.Key_Backtab:
            if (mods.length === 0) { // keep Tab for moving the focus
                event.accepted = false
                return
            }
            break
        }
        const key = keyName(event.key)
        const isFunctionKey = event.key >= Qt.Key_F1 && event.key <= Qt.Key_F24
        if (key.length === 0) {
            partial = qsTr("这个键不能用，换一个试试")
            partialIsHint = true
        } else if (mods.length === 0 && !isFunctionKey) {
            partial = qsTr("请同时按住 Ctrl、Alt、Shift 或 Win")
            partialIsHint = true
        } else {
            recorded(mods.concat([key]).join("+"))
            stop()
        }
    }
    Keys.onReleased: (event) => {
        event.accepted = true
        if (!partialIsHint)
            partial = modifierNames(event.modifiers).length > 0 ? partial : ""
    }

    Text {
        id: label
        anchors.centerIn: parent
        text: recorder.recording
              ? (recorder.partial.length > 0 ? recorder.partial : qsTr("请按下组合键…"))
              : (recorder.hotkey.length > 0 ? recorder.hotkey.split("+").join(" + ") : qsTr("未设置"))
        color: recorder.recording && recorder.partialIsHint ? Theme.danger
             : recorder.recording || recorder.hotkey.length > 0 ? Theme.text : Theme.faint
        font.pixelSize: Theme.fontBody
        font.weight: !recorder.recording && recorder.hotkey.length > 0 ? Font.DemiBold : Font.Normal
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: recorder.recording ? recorder.stop() : recorder.forceActiveFocus()
    }
}
