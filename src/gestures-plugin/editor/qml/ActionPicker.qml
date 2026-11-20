pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// What a rule does, chosen in one panel: the categories of built-in actions
// on the left, the chosen one's actions on the right; under them the other
// kinds (keys to press, a program or file or address to open, words to
// type, nothing). Keys are recorded here, in the window; Win+… and the like
// that Windows keeps for itself can be written instead.
//
// ask() opens it for a button: `choose(kind, value)` gets what was chosen
// (kind "builtin" with the action's id, "keys", "open", "type", "nothing"),
// `cancel()` runs when it is closed without a choice.
PickerPanel {
    id: picker

    required property GestureEditor editor
    // What the rule does now ({kind, id}, as RulesModel's roles give it), to mark it.
    property var current: ({ kind: "none", id: "" })
    property string stage: "menu" // menu | keys | open | url | type
    property int category: 0
    property var chooseHandler: null
    property var cancelHandler: null
    property bool decided: false
    property string keysProblem
    property string typed // the words of the type box, once they were taken from it
    property bool typedTaken: false

    readonly property int leftWidth: 192
    readonly property int rightWidth: 312
    readonly property int pad: Theme.menuPadding
    readonly property int rows: 12 // as many as the longest category

    function ask(button: Item, now: var, choose: var, cancel: var) {
        if (open && anchor === button) {
            open = false
            return
        }
        if (open)
            open = false
        decided = false
        typedTaken = false
        chooseHandler = choose
        cancelHandler = cancel
        current = now ?? ({ kind: "none", id: "" })
        anchor = button
        stage = "menu"
        keysProblem = ""
        const here = editor.categories.findIndex(c => c.items.some(item => item.id === current.id))
        category = current.kind === "builtin" && here >= 0 ? here : 0
        open = true
    }

    function choose(kind: string, value: string) {
        const handler = chooseHandler // closing lets go of it
        decided = true
        open = false
        if (handler)
            handler(kind, value)
    }

    function stageFor(next: string) {
        stage = next
        keysProblem = ""
        if (next === "keys") {
            Qt.callLater(() => recorder.forceActiveFocus())
        } else if (next === "open") {
            Qt.callLater(() => pathInput.takeFocus())
        } else if (next === "url") {
            Qt.callLater(() => urlInput.takeFocus())
        }
    }

    boxWidth: leftWidth + 1 + rightWidth + 2 * pad
    boxHeight: rows * Theme.menuItemHeight + 2 * pad

    // The rules were replaced (the plugin sent others): the row it was for is not there any more.
    Connections {
        target: picker.editor.rules
        function onModelReset() { picker.open = false }
    }

    onClosed: {
        if (!decided && cancelHandler)
            cancelHandler()
        chooseHandler = null
        cancelHandler = null
    }

    // ---- the left column: categories, then the other kinds ----
    Column {
        id: left
        x: picker.pad
        y: picker.pad
        width: picker.leftWidth

        Repeater {
            model: picker.editor.categories

            delegate: PickRow {
                id: categoryRow

                required property var modelData
                required property int index

                width: left.width
                icon: modelData.icon
                text: modelData.title
                chevron: true
                active: picker.stage === "menu" && picker.category === index
                onEntered: if (picker.stage === "menu") picker.category = index
                onClicked: {
                    picker.stage = "menu"
                    picker.category = index
                }
            }
        }

        Item { // a line between the built-in actions and the others
            width: left.width
            height: 9

            Hook {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 20
                height: Theme.edge(picker.dpr)
                part: Theme.separator
                extra: ({ card: true })
                orientation: Qt.Horizontal
                dpr: picker.dpr
            }
        }

        PickRow {
            width: left.width
            icon: "ph:keyboard"
            text: qsTr("按下按键…")
            marked: picker.current.kind === "keys"
            active: picker.stage === "keys"
            onClicked: picker.stageFor("keys")
        }
        PickRow {
            width: left.width
            icon: "ph:arrow-square-out"
            text: qsTr("打开程序或文件…")
            marked: picker.current.kind === "open" && picker.current.id.indexOf("://") < 0
            active: picker.stage === "open"
            onClicked: picker.stageFor("open")
        }
        PickRow {
            width: left.width
            icon: "ph:globe-simple"
            text: qsTr("打开网址…")
            marked: picker.current.kind === "open" && picker.current.id.indexOf("://") >= 0
            active: picker.stage === "url"
            onClicked: picker.stageFor("url")
        }
        PickRow {
            width: left.width
            icon: "ph:text-t"
            text: qsTr("输入文字…")
            marked: picker.current.kind === "type"
            active: picker.stage === "type"
            onClicked: picker.stageFor("type")
        }
        PickRow {
            width: left.width
            icon: "ph:prohibit"
            text: qsTr("什么都不做")
            marked: picker.current.kind === "nothing"
            onClicked: picker.choose("nothing", "")
        }
    }

    Hook { // the line between the two columns
        x: picker.pad + picker.leftWidth
        y: picker.pad
        width: Theme.edge(picker.dpr)
        height: parent.height - 2 * picker.pad
        part: Theme.separator
        extra: ({ card: true })
        orientation: Qt.Vertical
        dpr: picker.dpr
    }

    // ---- the right column ----
    readonly property real rightX: picker.pad + picker.leftWidth + 1

    Column { // the chosen category's actions
        x: picker.rightX
        y: picker.pad
        width: picker.rightWidth
        visible: picker.stage === "menu"

        Repeater {
            model: picker.editor.categories[picker.category]?.items ?? []

            delegate: PickRow {
                required property var modelData

                width: picker.rightWidth
                text: modelData.title
                note: modelData.note
                marked: picker.current.kind === "builtin" && picker.current.id === modelData.id
                onClicked: picker.choose("builtin", modelData.id)
            }
        }
    }

    Column { // keys: pressed here, or written
        x: picker.rightX + 12
        y: picker.pad + 6
        width: picker.rightWidth - 24
        spacing: 8
        visible: picker.stage === "keys"

        Text {
            width: parent.width
            text: qsTr("点一下下面的框，再按想用的按键，例如 F5、Ctrl+Shift+T。按 Esc 取消")
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        HotkeyRecorder {
            id: recorder
            width: parent.width
            keyMode: true
            keyMinimumWidth: picker.rightWidth - 24
            hotkey: picker.current.kind === "keys" ? picker.current.id : ""
            prompt: qsTr("请按下按键…")
            placeholder: qsTr("点这里，再按键")
            onRecordingChanged: picker.editor.setRecording(recording) // the plugin's shortcuts let go: they reach here too
            onKeyPressed: (key, modifiers, scanCode) => {
                if (key === Qt.Key_Escape && modifiers === Qt.NoModifier) {
                    picker.open = false
                    return
                }
                const text = picker.editor.keyText(key, modifiers, scanCode)
                if (text.length === 0) {
                    picker.keysProblem = qsTr("这个键不能用，换一个试试")
                    return
                }
                stop()
                picker.choose("keys", text)
            }
        }
        Text {
            width: parent.width
            visible: picker.keysProblem.length > 0
            text: picker.keysProblem
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            topPadding: 6
            text: qsTr("按不出来的组合（例如 Win+D）直接写在这里：")
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Row {
            width: parent.width
            spacing: 8

            InputBox {
                id: keysInput
                width: parent.width - keysOk.width - parent.spacing
                height: 32
                placeholder: "Win+D"
                error: text.length > 0 && !picker.editor.keysValid(text)
                onAccepted: if (picker.editor.keysValid(text)) picker.choose("keys", text.trim())
            }
            FlatButton {
                id: keysOk
                height: 32
                text: qsTr("确定")
                highlighted: true
                enabled: picker.editor.keysValid(keysInput.text)
                onClicked: picker.choose("keys", keysInput.text.trim())
            }
        }
    }

    Column { // a program, a shortcut, a file or a folder
        x: picker.rightX + 12
        y: picker.pad + 6
        width: picker.rightWidth - 24
        spacing: 8
        visible: picker.stage === "open"

        Text {
            width: parent.width
            text: qsTr("写下它的位置，或者点“浏览”选一个")
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        InputBox {
            id: pathInput
            width: parent.width
            height: 32
            placeholder: qsTr("例如 notepad.exe，或完整的位置")
            text: picker.current.kind === "open" && picker.current.id.indexOf("://") < 0 ? picker.current.id : ""
            onAccepted: if (text.trim().length > 0) picker.choose("open", text.trim())
        }
        Row {
            spacing: 8

            FlatButton {
                height: 32
                text: qsTr("浏览…")
                glyph: "ph:folder-open"
                onClicked: {
                    const path = picker.editor.browseFile(qsTr("选择程序或文件"))
                    if (path.length > 0)
                        pathInput.text = path
                    pathInput.takeFocus()
                }
            }
            FlatButton {
                height: 32
                text: qsTr("确定")
                highlighted: true
                enabled: pathInput.text.trim().length > 0
                onClicked: picker.choose("open", pathInput.text.trim())
            }
        }
    }

    Column { // a web address
        x: picker.rightX + 12
        y: picker.pad + 6
        width: picker.rightWidth - 24
        spacing: 8
        visible: picker.stage === "url"

        Text {
            width: parent.width
            text: qsTr("用默认浏览器打开这个网址")
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        InputBox {
            id: urlInput
            width: parent.width
            height: 32
            placeholder: "https://"
            text: picker.current.kind === "open" && picker.current.id.indexOf("://") >= 0 ? picker.current.id : ""
            function address(): string {
                const t = text.trim()
                return t.length === 0 ? "" : t.indexOf("://") >= 0 ? t : "https://" + t
            }
            onAccepted: if (address().length > 0) picker.choose("open", address())
        }
        FlatButton {
            height: 32
            text: qsTr("确定")
            highlighted: true
            enabled: urlInput.text.trim().length > 0
            onClicked: picker.choose("open", urlInput.address())
        }
    }

    Column { // words typed where the caret is
        x: picker.rightX + 12
        y: picker.pad + 6
        width: picker.rightWidth - 24
        spacing: 8
        visible: picker.stage === "type"

        Text {
            width: parent.width
            text: qsTr("这段文字会打到光标所在的地方")
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        SettingTextArea {
            id: typeArea
            width: parent.width
            minLines: 4
            maxLines: 6
            label: qsTr("要输入的文字")
            value: picker.current.kind === "type" ? picker.current.id : ""
            onCommitted: (text) => {
                picker.typed = text
                picker.typedTaken = true
            }
        }
        FlatButton {
            height: 32
            text: qsTr("确定")
            highlighted: true
            onClicked: {
                typeArea.commit()
                const words = picker.typedTaken ? picker.typed : typeArea.value
                if (words.length > 0)
                    picker.choose("type", words)
            }
        }
    }
}
