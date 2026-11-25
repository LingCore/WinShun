pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// One shortcut of 选中文字时 (a "key:" rule), a row as RuleRow's: the keys
// in the recorder that changes them (click, press others), what it does as
// the control on the right, lined up with the gestures' above, and a delete
// button that shows while the pointer is on the row. What there is to say
// of it goes on a line under both, the row growing for it: keys another
// program holds or that cannot be used (in red), one that does not need
// selected text, an action that counts down first.
Item {
    id: row

    // The rules model's roles.
    required property int sourceRow
    required property string on
    required property string arrows // the keys: "Ctrl+Alt+T"
    required property string description
    required property string kind
    required property string actionTitle
    required property string actionNote
    required property string actionIcon
    required property string actionId

    required property GestureEditor editor
    required property ActionPicker picker
    property real dpr: Screen.devicePixelRatio
    readonly property real edge: Theme.edge(row.dpr)
    readonly property bool deleteShown: rowHover.hovered || removeArea.containsMouse
    readonly property bool taken: row.editor.taken.indexOf(row.on) >= 0
    property string problem // what was wrong with the keys pressed last

    function askAction() {
        row.picker.ask(actionButton, { kind: row.kind, id: row.actionId },
            (kind, value) => row.editor.setAction(row.sourceRow, kind, value, ""), () => {})
    }

    function flash() {
        if (SystemTheme.animations) {
            flashAnimation.restart()
        } else {
            flashMark.opacity = 0.14
            flashTimer.restart()
        }
    }

    width: parent ? parent.width : 0
    height: Theme.snap(note.visible ? note.y + note.implicitHeight + 12 : 56, row.dpr)
    // Deleted while recording: the plugin takes its shortcuts back all the same.
    Component.onDestruction: if (recorder.recording) row.editor.setRecording(false)

    Hook { // separates rows inside a card (Theme.separator); the first row has none above it
        x: row.edge
        width: parent.width - 2 * row.edge
        height: row.edge
        part: row.y > 0 ? Theme.separator : null
        extra: ({ card: true })
        orientation: Qt.Horizontal
        dpr: row.dpr
    }

    HoverHandler { id: rowHover }

    Rectangle { // see flash()
        id: flashMark
        anchors.fill: parent
        anchors.margins: row.edge
        radius: Theme.insetCardRadius
        color: Theme.accent
        opacity: 0
        visible: opacity > 0

        SequentialAnimation {
            id: flashAnimation
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.16; duration: 160; easing.type: Easing.OutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.04; duration: 360; easing.type: Easing.InOutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.16; duration: 200; easing.type: Easing.InOutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0; duration: 700; easing.type: Easing.InQuad }
        }
        Timer {
            id: flashTimer
            interval: 1600
            onTriggered: flashMark.opacity = 0
        }
    }

    HotkeyRecorder {
        id: recorder
        objectName: "shortcutRecorder"
        x: 18
        y: Theme.snap((56 - height) / 2, row.dpr)
        hotkey: row.arrows
        onRecordingChanged: {
            row.editor.setRecording(recording)
            if (recording)
                row.problem = ""
        }
        onRecorded: (hotkey) => {
            if (hotkey.length > 0) // Backspace: the row's delete button takes it away, not this
                row.problem = row.editor.setShortcut(row.sourceRow, hotkey)
        }
    }

    Text {
        id: note
        objectName: "shortcutNote"
        readonly property bool warning: row.problem.length > 0 || row.taken
        x: recorder.x + 2
        y: recorder.y + recorder.height + 6
        width: actionButton.x + actionButton.width - x
        visible: text.length > 0
        text: row.problem.length > 0 ? row.problem
            : row.taken ? qsTr("别的程序占用了这个快捷键，换一个试试")
            : [row.description, row.actionNote].filter(s => s.length > 0).join(" · ")
        wrapMode: Text.Wrap
        color: warning ? Theme.danger : Theme.subtext
        font.pixelSize: Theme.fontCaption
    }

    ActionButton {
        id: actionButton
        objectName: "shortcutActionButton"
        anchors.right: removeButton.left
        anchors.rightMargin: 6
        anchors.verticalCenter: recorder.verticalCenter
        width: row.width < 560 ? 176 : 224
        dpr: row.dpr
        open: row.picker.open && row.picker.anchor === actionButton
        unset: row.kind === "nothing"
        invalid: row.kind === "invalid"
        icon: row.actionIcon
        text: row.kind === "nothing" ? qsTr("什么都不做") : row.actionTitle
        onClicked: row.askAction()
    }

    Item { // delete: shown while the pointer is on the row
        id: removeButton
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: recorder.verticalCenter
        width: 28
        height: 28
        visible: row.deleteShown

        Hook {
            id: removePart
            anchors.fill: parent
            part: Theme.keySurface
            hovered: removeArea.containsMouse
            pressed: removeArea.pressed
            flat: true
            corner: Theme.smallControlRadius
            role: "toolbar"
            dpr: row.dpr
        }
        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: removePart.out("contentOffsetY", 0)
            glyph: "ph:trash"
            size: 16
            color: removeArea.containsMouse ? Theme.danger : Theme.subtext
        }
        MouseArea {
            id: removeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: row.editor.remove(row.sourceRow, "")
        }
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("删除这个快捷键")
    }
}
