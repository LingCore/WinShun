import QtQuick
import WinShun

// Click, then press the key combination. Esc cancels, Backspace clears.
// While it records, the keys come straight from the keyboard (SettingsEditor
// starts a ShortcutCapture), so that Alt+Space, Win+… and combinations other
// programs registered arrive here too.
Rectangle {
    id: recorder

    property string hotkey
    property bool previewShortcut: false
    readonly property bool recording: activeFocus
    property string partial // modifiers held so far, or a hint
    property bool partialIsHint: false

    signal recorded(string hotkey)

    function modifierNames(modifiers) {
        const names = [] // in Windows' own order: Win + Ctrl + Alt + Shift
        if (modifiers & Qt.MetaModifier) names.push("Win")
        if (modifiers & Qt.ControlModifier) names.push("Ctrl")
        if (modifiers & Qt.AltModifier) names.push("Alt")
        if (modifiers & Qt.ShiftModifier) names.push("Shift")
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

    // Wide enough for the longest label it shows itself, so it does not
    // change width while recording.
    TextMetrics {
        id: widest
        font.pixelSize: Theme.fontBody
        font.weight: Theme.fontStrong
        text: qsTr("这个键不能用，换一个试试")
    }
    TextMetrics {
        id: widest2
        font.pixelSize: Theme.fontBody
        font.weight: Theme.fontStrong
        text: qsTr("请同时按住 Ctrl、Alt 或 Win")
    }
    implicitWidth: Math.max(150, widest.width, widest2.width) + 28
    implicitHeight: 36
    radius: Theme.controlRadius
    activeFocusOnTab: true
    // For screen readers and UI Automation: a button that records, named
    // by what it holds.
    Accessible.role: Accessible.Button
    Accessible.name: recording ? qsTr("正在录制快捷键") : hotkey.length > 0 ? hotkey : qsTr("录制快捷键")
    Accessible.onPressAction: recording ? stop() : forceActiveFocus()
    color: recording ? Theme.inputFocused : area.containsMouse ? Theme.controlHover : Theme.control
    border.width: 1
    border.color: Theme.controlBorder

    onRecordingChanged: if (!recording) { partial = ""; partialIsHint = false }
    // Keeping the focus on another page, or in a window closed or left, it
    // would go on taking the keys (Win among them) as soon as the window is
    // in front again. `focus`, not `recording`: in a window left, no item has
    // the active focus any more.
    onVisibleChanged: if (!visible && focus) stop()

    Connections {
        target: recorder.Window.window
        function onActiveChanged() {
            if (!recorder.Window.window.active && recorder.focus)
                recorder.stop()
        }
    }

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
        const combination = mods.concat([key]).join("+")
        if (key.length === 0) {
            partial = qsTr("这个键不能用，换一个试试")
            partialIsHint = true
        } else if (!(previewShortcut ? HotkeyRule.previewAllowed(combination) : HotkeyRule.allowed(combination))) {
            partial = previewShortcut ? qsTr("这个快捷键与其他操作冲突") : qsTr("请同时按住 Ctrl、Alt 或 Win")
            partialIsHint = true
        } else {
            recorded(combination)
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
        width: Math.min(implicitWidth, parent.width - 28)
        elide: Text.ElideRight
        text: recorder.recording
              ? (recorder.partial.length > 0 ? recorder.partial : qsTr("请按下组合键…"))
              : (recorder.hotkey.length > 0 ? recorder.hotkey.split("+").join(" + ") : qsTr("未设置"))
        color: recorder.recording && recorder.partialIsHint ? Theme.danger
             : recorder.recording || recorder.hotkey.length > 0 ? Theme.text : Theme.faint
        font.pixelSize: Theme.fontBody
        font.weight: !recorder.recording && recorder.hotkey.length > 0 ? Theme.fontStrong : Font.Normal
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: recorder.recording ? recorder.stop() : recorder.forceActiveFocus()
    }

    Rectangle {
        visible: recorder.recording
        anchors.bottom: parent.bottom
        x: Theme.edge(Screen.devicePixelRatio) // inside the field's border
        width: parent.width - 2 * x
        height: Theme.snap(2, Screen.devicePixelRatio) // 2.5 device pixels at 125 %
        radius: Theme.lineRadius
        color: Theme.accent
    }
}
