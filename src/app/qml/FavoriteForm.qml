import QtQuick
import WinShun

// Changing a favorite in the settings (收藏夹): its name and its keyword,
// and its place in the list for the keyboard (上移, 下移; the mouse drags the
// grip). FavoritesSettings checks and saves it, or says what is wrong. Enter
// saves, Esc puts it away.
Item {
    id: form
    objectName: "favoriteForm"

    required property FavoritesSettings favorites
    required property int index
    required property var favorite // as FavoritesSettings.items has it
    required property int count // of favorites
    // What was typed and not saved yet, {name, keyword}, when the form is
    // made again (moved with 上移, the list redone): null for none.
    property var draft: null
    property string error
    property bool ready: false // the fields filled in
    readonly property bool command: favorite.kind === "command"

    signal finished()
    signal edited(string name, string keyword, string commandLine, string directory)
    signal moveRequested(int to) // 上移 or 下移: the form goes with it

    function save() {
        form.error = form.command
            ? form.favorites.saveCommand(form.index, nameField.text, keywordField.text, commandField.text, directoryField.text)
            : form.favorites.save(form.index, nameField.text, keywordField.text)
        if (form.error.length === 0)
            form.finished()
    }

    function changed() {
        error = ""
        if (ready)
            edited(nameField.text, keywordField.text, commandField.text, directoryField.text)
    }

    implicitHeight: fields.implicitHeight + 28
    Keys.onEscapePressed: form.finished()
    Component.onCompleted: {
        nameField.text = form.draft ? form.draft.name : !form.command && form.favorite.name === form.favorite.ownName ? "" : form.favorite.name
        keywordField.text = form.draft ? form.draft.keyword : form.favorite.keyword
        commandField.text = form.draft ? form.draft.commandLine ?? "" : form.favorite.command ?? ""
        directoryField.text = form.draft ? form.draft.directory ?? "" : form.favorite.workingDirectory ?? ""
        form.ready = true
        nameField.takeFocus()
    }

    // A box with its label above and, if any, a hint below.
    component Field: Column {
        id: field

        property string label
        property string hint
        property alias text: box.text
        property alias placeholder: box.placeholder

        signal accepted()

        function takeFocus() { box.takeFocus() }

        spacing: 6

        Text {
            text: field.label
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }
        InputBox {
            id: box
            width: field.width
            onAccepted: field.accepted()
        }
        Text {
            visible: field.hint.length > 0
            width: field.width
            text: field.hint
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            lineHeight: 1.15
        }
    }

    Rectangle { // sets the form apart from the rows around it
        anchors.fill: parent
        anchors.margins: 1
        radius: Theme.smallControlRadius
        color: Theme.navHover
    }

    Column {
        id: fields
        x: 14
        y: 14
        width: parent.width - 28
        spacing: 14

        Text {
            visible: !form.command
            width: parent.width
            text: form.favorite.shownPath
            textFormat: Text.PlainText
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideMiddle
        }

        Row {
            width: parent.width
            spacing: 12

            Field {
                id: nameField
                objectName: "favoriteName"
                width: parent.width - keywordField.width - parent.spacing
                label: qsTr("名称")
                placeholder: form.favorite.ownName
                hint: form.command ? qsTr("例如：用 VS Code 打开") : qsTr("不填就用它自己的名字")
                onTextChanged: form.changed()
                onAccepted: form.save()
            }
            Field {
                id: keywordField
                objectName: "favoriteKeyword"
                width: 200
                label: qsTr("关键词")
                placeholder: form.command ? qsTr("例如 code") : qsTr("可选，例如 gz")
                hint: form.command ? qsTr("输入它，再按 Enter 运行") : qsTr("只输入它，按 Enter 就打开")
                onTextChanged: form.changed()
                onAccepted: form.save()
            }
        }

        Row {
            visible: form.command
            width: parent.width
            spacing: 8
            Field {
                id: commandField
                objectName: "favoriteCommand"
                width: parent.width - pickButton.width - parent.spacing
                label: qsTr("命令")
                placeholder: '"C:\\Tools\\Code.exe" {query}'
                hint: qsTr("{query} 接收关键词后面的文字，自动作为一个参数；支持 %环境变量%。不会自动通过命令提示符执行。")
                onTextChanged: form.changed()
                onAccepted: form.save()
            }
            FlatButton {
                id: pickButton
                y: 24
                text: qsTr("选择程序…")
                onClicked: {
                    const picked = form.favorites.pickCommand()
                    if (picked.length > 0)
                        commandField.text = picked
                }
            }
        }
        Field {
            id: directoryField
            visible: form.command
            width: parent.width
            label: qsTr("工作文件夹")
            placeholder: qsTr("可选，例如 D:\\Projects")
            hint: qsTr("不填则由 Windows 决定；路径不用加引号")
            onTextChanged: form.changed()
            onAccepted: form.save()
        }

        Text {
            visible: form.error.length > 0
            width: parent.width
            text: form.error
            textFormat: Text.PlainText
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }

        Item {
            width: parent.width
            height: buttons.height

            Row {
                id: buttons
                spacing: 8

                FlatButton {
                    highlighted: true
                    text: qsTr("保存")
                    onClicked: form.save()
                }
                FlatButton {
                    text: qsTr("取消")
                    onClicked: form.finished()
                }
            }
            Row {
                visible: form.index >= 0
                anchors.right: parent.right
                spacing: 2

                FlatButton {
                    flat: true
                    enabled: form.index > 0
                    text: qsTr("上移")
                    glyph: "\uE74A" // Up
                    onClicked: form.moveRequested(form.index - 1)
                }
                FlatButton {
                    flat: true
                    enabled: form.index < form.count - 1
                    text: qsTr("下移")
                    glyph: "\uE74B" // Down
                    onClicked: form.moveRequested(form.index + 1)
                }
            }
        }
    }
}
