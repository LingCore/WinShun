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
    property string iconData
    property bool silent: false // 静默执行, 以管理员身份运行 (GitHub #112)
    property bool administrator: false
    property int iconRequest: 0
    readonly property bool iconLoading: iconRequest > 0
    property string previewCommand
    property string previewDirectory
    readonly property bool command: favorite.kind === "command"
    // The drop pie (拖文件时按 ` 键) is on: a folder's form shows how the pie
    // will write its name when it gets shortened.
    property bool dropPie: false
    readonly property string pieName: dropPie && (favorite.kind === "folder" || favorite.kind === "place")
                                      ? (nameField.text.trim().length > 0 ? nameField.text : favorite.ownName).trim()
                                      : ""
    readonly property string pieLabel: pieName.length > 0 ? DropPieText.label(pieName) : ""

    signal finished()
    signal edited(string name, string keyword, string commandLine, string directory, string icon, bool silent, bool administrator)
    signal moveRequested(int to) // 上移 or 下移: the form goes with it

    function save() {
        if (form.iconLoading)
            return
        form.error = form.command
            ? form.favorites.saveCommand(form.index, nameField.text, keywordField.text, commandField.text, directoryField.text, form.iconData, form.silent, form.administrator)
            : form.favorites.save(form.index, nameField.text, keywordField.text)
        if (form.error.length === 0)
            form.finished()
    }

    function changed() {
        error = ""
        if (ready)
            edited(nameField.text, keywordField.text, commandField.text, directoryField.text, form.iconData, form.silent, form.administrator)
    }

    onIconDataChanged: form.changed()
    onSilentChanged: form.changed()
    onAdministratorChanged: form.changed()

    Timer {
        id: previewDelay
        interval: 250
        onTriggered: {
            form.previewCommand = commandField.text
            form.previewDirectory = directoryField.text
        }
    }

    Connections {
        target: form.favorites
        function onCommandIconReady(request, icon, error) {
            if (request !== form.iconRequest)
                return
            form.iconRequest = 0
            if (error.length > 0) {
                form.error = error
            } else {
                form.error = ""
                form.iconData = icon
            }
        }
    }

    implicitHeight: fields.implicitHeight + 28
    Keys.onEscapePressed: form.finished()
    Component.onCompleted: {
        nameField.text = form.draft ? form.draft.name : !form.command && form.favorite.name === form.favorite.ownName ? "" : form.favorite.name
        keywordField.text = form.draft ? form.draft.keyword : form.favorite.keyword
        commandField.text = form.draft ? form.draft.commandLine ?? "" : form.favorite.command ?? ""
        directoryField.text = form.draft ? form.draft.directory ?? "" : form.favorite.workingDirectory ?? ""
        form.iconData = form.draft ? form.draft.icon ?? "" : form.favorite.commandIcon ?? ""
        form.silent = form.draft ? form.draft.silent ?? false : form.favorite.silent ?? false
        form.administrator = form.draft ? form.draft.administrator ?? false : form.favorite.administrator ?? false
        form.previewCommand = commandField.text
        form.previewDirectory = directoryField.text
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

    // A switch with its label and what it does beneath.
    component Option: Row {
        id: option

        property string label
        property string hint
        property alias checked: toggle.checked
        signal toggled(bool on)

        spacing: 12

        ToggleSwitch {
            id: toggle
            word: false
            anchors.verticalCenter: parent.verticalCenter
            Accessible.name: option.label
            onToggled: (on) => option.toggled(on)
        }
        Column {
            width: option.width - toggle.width - option.spacing
            spacing: 2

            Text {
                width: parent.width
                text: option.label
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: option.hint
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }
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

        Text {
            visible: form.pieLabel.length > 0 && form.pieLabel !== form.pieName.replace(/\s+/g, " ")
            width: parent.width
            topPadding: -6 // with the name's hint above it
            text: qsTr("圆盘里显示为：%1。名字太长时可以起个短名").arg(form.pieLabel)
            textFormat: Text.PlainText
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
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
                onTextChanged: { form.changed(); previewDelay.restart() }
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
            // The path outside qsTr: lupdate reads "\\" in QML strings wrongly (docs/pitfalls.md).
            placeholder: qsTr("可选，例如 %1").arg("D:\\Projects")
            hint: qsTr("不填则由 Windows 决定；路径不用加引号")
            onTextChanged: { form.changed(); previewDelay.restart() }
            onAccepted: form.save()
        }

        Option {
            objectName: "favoriteSilent"
            visible: form.command
            width: parent.width
            label: qsTr("静默执行")
            hint: qsTr("控制台程序、.bat、.cmd 和 .ps1 不弹出黑色窗口，输出不显示。自带窗口的程序照常显示")
            checked: form.silent
            onToggled: (on) => form.silent = on
        }
        Option {
            objectName: "favoriteAdministrator"
            visible: form.command
            width: parent.width
            label: qsTr("以管理员身份运行")
            hint: form.silent
                  ? qsTr("每次运行都会先弹出系统的权限确认，确认框里显示的是 Win顺")
                  : qsTr("每次运行都会先弹出系统的权限确认")
            checked: form.administrator
            onToggled: (on) => form.administrator = on
        }

        Column {
            visible: form.command
            width: parent.width
            spacing: 8

            Text {
                text: qsTr("图标")
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
            Row {
                width: parent.width
                spacing: 12

                Rectangle {
                    width: 44
                    height: 44
                    radius: Theme.controlRadius
                    color: Theme.card
                    border.width: 1
                    border.color: Theme.cardBorder

                    Image {
                        id: iconPreview
                        objectName: "favoriteIconPreview"
                        anchors.centerIn: parent
                        width: 32
                        height: 32
                        sourceSize.width: width
                        sourceSize.height: height
                        asynchronous: true
                        smooth: false
                        source: form.favorites.commandIconUrl(form.previewCommand, form.previewDirectory, form.iconData, Theme.dark)
                        Accessible.ignored: true
                    }
                    Glyph {
                        anchors.centerIn: parent
                        visible: iconPreview.status !== Image.Ready
                        glyph: "\uE756"
                        size: 24
                        color: Theme.subtext
                    }
                }
                Column {
                    width: parent.width - 56
                    spacing: 6

                    Flow {
                        width: parent.width
                        spacing: 8

                        FlatButton {
                            objectName: "favoritePickIcon"
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("选择图标…")
                            text: form.iconLoading ? qsTr("正在读取图标…") : qsTr("选择图标…")
                            enabled: !form.iconLoading
                            onClicked: form.iconRequest = form.favorites.pickCommandIcon()
                        }
                        FlatButton {
                            objectName: "favoriteResetIcon"
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("恢复自动")
                            flat: true
                            text: qsTr("恢复自动")
                            enabled: form.iconData.length > 0 && !form.iconLoading
                            onClicked: form.iconData = ""
                        }
                    }
                    Text {
                        width: parent.width
                        text: form.iconData.length > 0
                              ? qsTr("自定义图标已随收藏保存，移动或删除原文件也不受影响")
                              : qsTr("自动使用程序图标；也可选择 PNG、ICO、SVG，或从其他程序、快捷方式提取")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
            }
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
                    enabled: !form.iconLoading
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
                    enabled: form.index > 0 && !form.iconLoading
                    text: qsTr("上移")
                    glyph: "\uE74A" // Up
                    onClicked: form.moveRequested(form.index - 1)
                }
                FlatButton {
                    flat: true
                    enabled: form.index < form.count - 1 && !form.iconLoading
                    text: qsTr("下移")
                    glyph: "\uE74B" // Down
                    onClicked: form.moveRequested(form.index + 1)
                }
            }
        }
    }
}
