pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A program for a rule or the list of those left alone: one of the windows
// open now, or its name typed ("photoshop.exe") or taken from a file.
// ask(button, left, choose): `choose(name)` gets the name as the rules keep
// it; `left` names programs not to offer (the ones already there).
PickerPanel {
    id: picker

    required property GestureEditor editor
    property var running: [] // the programs of the windows open now, not those left out
    property var chooseHandler: null

    readonly property int pad: Theme.menuPadding
    readonly property int shown: Math.min(running.length, 7)

    function ask(button: Item, leftOut: var, choose: var) {
        if (open && anchor === button) {
            open = false
            return
        }
        const out = leftOut ?? []
        running = editor.visiblePrograms().filter(name => out.indexOf(name) < 0)
        chooseHandler = choose
        anchor = button
        nameInput.text = ""
        open = true
        Qt.callLater(() => nameInput.takeFocus())
    }

    function take(text: string) {
        const name = editor.normalizeProgram(text)
        if (name.length === 0)
            return
        open = false
        if (chooseHandler)
            chooseHandler(name)
    }

    boxWidth: 304
    boxHeight: 2 * pad + 30 + Math.max(1, shown) * Theme.menuItemHeight + 9 + 44

    Text {
        x: picker.pad + 10
        y: picker.pad
        height: 30
        verticalAlignment: Text.AlignVCenter
        text: picker.running.length > 0 ? qsTr("现在打开着的程序") : qsTr("没有别的程序开着窗口")
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
    }
    Flickable {
        id: list
        x: picker.pad
        y: picker.pad + 30
        width: picker.boxWidth - 2 * picker.pad
        height: Math.max(1, picker.shown) * Theme.menuItemHeight
        contentHeight: column.height
        boundsBehavior: Flickable.StopAtBounds
        clip: picker.running.length > picker.shown
        interactive: contentHeight > height

        Column {
            id: column
            width: list.width

            Repeater {
                model: picker.running

                delegate: PickRow {
                    required property string modelData

                    width: column.width
                    icon: "ph:app-window"
                    text: modelData
                    onClicked: picker.take(modelData)
                }
            }
        }
    }
    Item {
        id: divider
        x: picker.pad
        y: list.y + list.height
        width: list.width
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
    Row {
        x: picker.pad + 6
        y: divider.y + divider.height + 6
        width: list.width - 12
        spacing: 6

        InputBox {
            id: nameInput
            width: parent.width - browse.width - add.width - 2 * parent.spacing
            height: 32
            placeholder: qsTr("或者写程序名，例如 photoshop.exe")
            onAccepted: picker.take(text)
        }
        FlatButton {
            id: browse
            height: 32
            width: 32
            glyph: "ph:folder-open"
            onClicked: {
                const path = picker.editor.browseFile(qsTr("选择程序"))
                if (path.length > 0)
                    picker.take(path)
            }
        }
        FlatButton {
            id: add
            height: 32
            width: 32
            glyph: "ph:plus"
            highlighted: true
            enabled: picker.editor.normalizeProgram(nameInput.text).length > 0
            onClicked: picker.take(nameInput.text)
        }
    }
}
