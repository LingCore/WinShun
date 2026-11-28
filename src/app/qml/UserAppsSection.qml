pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → 搜索范围 › 应用 (#505): the folders of portable programs, listed
// as apps; the programs found there and the other apps given names of the
// user's (UserAppsSettings), each to name, hide or show again.
SettingsSection {
    id: appsSection

    required property SettingsEditor editor
    readonly property UserAppsSettings apps: editor.userApps
    property string editing: "" // the path of the row whose name is being typed

    title: qsTr("便携软件与别名")
    glyph: "ph:app-window"

    SettingRow {
        id: foldersRow

        title: qsTr("便携软件文件夹")
        keywords: qsTr("绿色软件;便携;免安装;解压;portable;green;目录;文件夹").split(";")
        values: appsSection.apps.folders
        description: qsTr("里面的程序（.exe 和快捷方式）像开始菜单里的软件一样搜得到；每个软件一个文件夹时也看下一层，有和文件夹同名的程序就只取它")

        FlatButton {
            dpr: appsSection.dpr
            text: qsTr("添加文件夹")
            glyph: "ph:plus"
            onClicked: appsSection.apps.addFolder()
        }

        body: Rectangle {
            width: parent.width
            height: Math.max(folderList.implicitHeight, 40)
            radius: Theme.controlRadius
            color: "transparent"
            border.width: 1
            border.color: Theme.cardBorder

            Text {
                visible: appsSection.apps.folders.length === 0
                anchors.centerIn: parent
                text: qsTr("还没有添加文件夹")
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
            }

            Column {
                id: folderList
                width: parent.width

                Repeater {
                    model: appsSection.apps.folders

                    delegate: Item {
                        id: folderRow

                        required property int index
                        required property string modelData

                        width: folderList.width
                        height: 42

                        Rectangle {
                            visible: folderRow.index > 0
                            x: 12
                            width: parent.width - 24
                            height: Theme.edge(appsSection.dpr)
                            color: Theme.divider
                        }
                        Glyph {
                            id: folderIcon
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "ph:folder"
                            size: 18
                        }
                        Text {
                            anchors.left: folderIcon.right
                            anchors.leftMargin: 10
                            anchors.right: removeFolder.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: folderRow.modelData
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideMiddle
                        }
                        FlatButton {
                            id: removeFolder
                            dpr: appsSection.dpr
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            implicitHeight: 32
                            flat: true
                            text: qsTr("移除")
                            onClicked: appsSection.apps.removeFolder(folderRow.index)
                        }
                    }
                }
            }
        }
    }

    SettingRow {
        id: namesRow

        title: qsTr("软件和别名")
        keywords: qsTr("别名;改名;名字;中文名;隐藏;不显示;alias;rename;hide").split(";")
        values: appsSection.apps.items.map(item => item.name)
        description: qsTr("给软件起个好记的名字，原来的名字照样搜得到；便携软件里不想看到的可以隐藏。在搜索结果里右键软件选“设置别名”（或按 F2）也一样")

        body: Rectangle {
            width: parent.width
            height: Math.max(appList.implicitHeight, 40)
            radius: Theme.controlRadius
            color: "transparent"
            border.width: 1
            border.color: Theme.cardBorder

            Text {
                visible: appsSection.apps.items.length === 0
                anchors.centerIn: parent
                width: parent.width - 24
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("添加文件夹后，找到的软件列在这里；设置了别名的软件也在这里")
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            // The 隐藏 column takes the wider of its two words (Hide / Show).
            FlatButton {
                id: hideWidth
                visible: false
                dpr: appsSection.dpr
                implicitHeight: 32
                flat: true
                text: qsTr("隐藏")
            }
            FlatButton {
                id: showWidth
                visible: false
                dpr: appsSection.dpr
                implicitHeight: 32
                flat: true
                text: qsTr("显示")
            }

            Column {
                id: appList
                width: parent.width

                Repeater {
                    model: appsSection.apps.items

                    delegate: Item {
                        id: appRow

                        required property int index
                        required property var modelData
                        readonly property bool editing: appsSection.editing === modelData.path
                        readonly property bool dimmed: modelData.hidden || modelData.missing

                        width: appList.width
                        height: 52

                        Rectangle {
                            visible: appRow.index > 0
                            x: 12
                            width: parent.width - 24
                            height: Theme.edge(appsSection.dpr)
                            color: Theme.divider
                        }
                        Item { // as its row shows it in the launcher
                            id: appIcon
                            opacity: appRow.dimmed ? 0.5 : 1
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: 24
                            height: 24

                            Image {
                                readonly property int pixels: Math.round(appIcon.width * appsSection.dpr)

                                // Exactly its device pixels, unsmoothed (as in ResultRow).
                                x: 1 / 64
                                y: 1 / 64
                                width: pixels / appsSection.dpr
                                height: pixels / appsSection.dpr
                                sourceSize.width: pixels / appsSection.dpr
                                sourceSize.height: pixels / appsSection.dpr
                                smooth: false
                                asynchronous: true
                                source: appRow.modelData.icon
                            }
                        }
                        Column {
                            visible: !appRow.editing
                            anchors.left: appIcon.right
                            anchors.leftMargin: 10
                            anchors.right: appButtons.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                width: parent.width
                                text: appRow.modelData.name
                                textFormat: Text.PlainText
                                color: appRow.dimmed ? Theme.subtext : Theme.text
                                font.pixelSize: Theme.fontBody
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: {
                                    const item = appRow.modelData
                                    const parts = []
                                    if (item.missing)
                                        parts.push(qsTr("找不到了"))
                                    else if (item.hidden)
                                        parts.push(qsTr("已隐藏"))
                                    if (item.named && item.ownName.length > 0)
                                        parts.push(qsTr("原名 %1").arg(item.ownName))
                                    parts.push(item.where)
                                    return parts.join(" · ")
                                }
                                textFormat: Text.PlainText
                                color: appRow.modelData.missing ? Theme.danger : Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideMiddle
                            }
                        }
                        InputBox {
                            id: nameBox
                            visible: appRow.editing
                            anchors.left: appIcon.right
                            anchors.leftMargin: 10 - textInset
                            anchors.right: appButtons.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            implicitHeight: 32
                            dpr: appsSection.dpr
                            maximumLength: 40 // kMaxAppNameLength
                            placeholder: qsTr("不填就用原来的名字（%1）").arg(appRow.modelData.ownName)
                            Accessible.name: qsTr("别名")
                            property bool dropped: false // Esc: the name stays as it was
                            onAccepted: appsSection.editing = ""
                            Keys.onEscapePressed: {
                                dropped = true
                                appsSection.editing = ""
                            }
                            onInputFocusedChanged: {
                                if (inputFocused)
                                    return
                                if (!dropped)
                                    appsSection.apps.setName(appRow.modelData.path, text)
                                if (appRow.editing)
                                    appsSection.editing = ""
                            }
                        }
                        // In columns, right to left: 隐藏 (portable programs; its room
                        // kept on the others' rows), 改名, 还原 (named ones).
                        Row {
                            id: appButtons
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            FlatButton {
                                dpr: appsSection.dpr
                                visible: appRow.modelData.named && !appRow.modelData.missing && !appRow.editing
                                implicitHeight: 32
                                flat: true
                                text: qsTr("还原")
                                Accessible.description: qsTr("用回原来的名字")
                                onClicked: appsSection.apps.setName(appRow.modelData.path, "")
                            }
                            FlatButton {
                                dpr: appsSection.dpr
                                visible: !appRow.modelData.missing && !appRow.editing
                                implicitHeight: 32
                                flat: true
                                text: qsTr("改名")
                                onClicked: {
                                    appsSection.editing = appRow.modelData.path
                                    nameBox.dropped = false
                                    nameBox.text = appRow.modelData.named ? appRow.modelData.name : ""
                                    nameBox.takeFocus()
                                }
                            }
                            Item {
                                visible: !appRow.modelData.missing
                                width: Math.max(hideWidth.implicitWidth, showWidth.implicitWidth)
                                height: hideButton.implicitHeight

                                FlatButton {
                                    id: hideButton
                                    anchors.right: parent.right
                                    dpr: appsSection.dpr
                                    visible: appRow.modelData.portable && !appRow.editing
                                    implicitHeight: 32
                                    flat: true
                                    text: appRow.modelData.hidden ? qsTr("显示") : qsTr("隐藏")
                                    onClicked: appsSection.apps.setHidden(appRow.modelData.path, !appRow.modelData.hidden)
                                }
                            }
                            FlatButton {
                                dpr: appsSection.dpr
                                visible: appRow.modelData.missing
                                implicitHeight: 32
                                flat: true
                                text: qsTr("移除")
                                onClicked: appsSection.apps.forget(appRow.modelData.path)
                            }
                        }
                    }
                }
            }
        }
    }
}
