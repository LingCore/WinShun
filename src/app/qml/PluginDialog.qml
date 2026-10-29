pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The questions the 插件 page asks, over the settings window (as
// UpdateDialog does): the permissions a plugin wants before it is turned
// on (the plugin design, §3.4), whether to replace a version already
// installed, and whether a plugin's data goes with it when it is removed.
Item {
    id: dialog

    required property PluginSettings plugins
    property string kind // permissions | install | uninstall
    property var info: ({})
    property bool open: false
    property bool animated: true // false: shown at once (the offscreen preview, which renders no frames between)

    function ask(what, details) {
        kind = what
        info = details
        open = true
        card.forceActiveFocus()
    }
    function close() { open = false }

    function accept(withData) {
        const details = info
        close()
        if (kind === "permissions")
            plugins.confirmEnable(details.id)
        else if (kind === "install")
            plugins.confirmInstall()
        else if (kind === "uninstall")
            plugins.confirmUninstall(details.id, withData === true)
    }
    function reject() {
        if (kind === "permissions")
            plugins.declineEnable(info.id)
        else if (kind === "install")
            plugins.cancelInstall()
        close()
    }

    Connections {
        target: dialog.plugins
        function onConfirmationNeeded(details) { dialog.ask("permissions", details) }
        function onInstallQuestion(details) { dialog.ask("install", details) }
        function onUninstallQuestion(details) { dialog.ask("uninstall", details) }
    }

    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { enabled: dialog.animated; NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

    Rectangle { // dims the window; clicks stay out of it
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, Theme.dark ? 0.5 : 0.3)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Rectangle {
        id: card

        anchors.centerIn: parent
        width: Math.min(560, parent.width - 48)
        height: content.implicitHeight + 48
        radius: Theme.largeCardRadius
        color: Theme.menuBackground
        border.width: 1
        border.color: Theme.windowEdge
        scale: dialog.open ? 1 : 0.96
        Behavior on scale { enabled: dialog.animated; NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        Keys.onEscapePressed: dialog.reject()
        // Enter never deletes the plugin's data: with data to ask about it
        // takes the gentler choice (the plugin alone, data kept).
        Keys.onReturnPressed: enterKey()
        Keys.onEnterPressed: enterKey()
        function enterKey() {
            if (dialog.kind === "uninstall" && dialog.info.hasData === true)
                dialog.accept(false)
            else
                primary.clicked()
        }

        MouseArea { anchors.fill: parent } // a click on the card does not reach the dimmer

        Column {
            id: content
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 14

            Text {
                width: parent.width
                text: dialog.kind === "permissions"
                      ? (dialog.info.more ? qsTr("“%1” 的新版本要更多权限").arg(dialog.info.name) : qsTr("打开“%1”？").arg(dialog.info.name))
                    : dialog.kind === "install"
                      ? (dialog.info.same ? qsTr("重新安装“%1”？").arg(dialog.info.name)
                       : dialog.info.downgrade ? qsTr("把“%1”换成旧版本？").arg(dialog.info.name)
                       : qsTr("升级“%1”？").arg(dialog.info.name))
                    : qsTr("卸载“%1”？").arg(dialog.info.name)
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            Text { // who made it, where it came from
                visible: dialog.kind === "permissions"
                width: parent.width
                text: [dialog.info.version ? qsTr("版本 %1").arg(dialog.info.version) : "",
                       dialog.info.author ? qsTr("作者 %1").arg(dialog.info.author) : "",
                       dialog.info.source ?? ""].filter(s => s.length > 0).join(" · ")
                      + (dialog.info.homepage ? "\n" + dialog.info.homepage : "")
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Text {
                visible: dialog.kind === "install"
                width: parent.width
                text: qsTr("已装的是 %1，要装的是 %2。它的设置和关键字保留；装好后如果它要的权限变多，打开时会再问一次。")
                      .arg(dialog.info.installedVersion ?? "").arg(dialog.info.newVersion ?? "")
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }

            Text {
                visible: dialog.kind === "uninstall"
                width: parent.width
                text: dialog.info.hasData
                      ? qsTr("它的文件夹、设置和保存的密码会删掉。它自己的数据（%1.data）也一起删吗？").arg(dialog.info.id ?? "")
                      : qsTr("它的文件夹、设置和保存的密码会删掉。")
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }

            Rectangle { // the permissions, one per line
                visible: dialog.kind === "permissions"
                width: parent.width
                height: permissionList.implicitHeight + 16
                radius: Theme.cardRadius
                color: Theme.hover
                border.width: 1
                border.color: Theme.cardBorder

                Column {
                    id: permissionList
                    x: 12
                    y: 8
                    width: parent.width - 24
                    spacing: 4

                    Repeater {
                        model: dialog.kind === "permissions" ? (dialog.info.permissions ?? []) : []

                        delegate: Item {
                            id: permissionRow

                            required property var modelData
                            readonly property bool isNew: (dialog.info.newNames ?? []).indexOf(modelData.name) >= 0 && dialog.info.more === true

                            // A line of 26; a long sentence (the sandbox's) wraps, the
                            // tick or tag staying on its first line.
                            width: parent.width
                            height: Math.max(26, permissionText.implicitHeight + 8)

                            Glyph { // enforced by Windows: a tick
                                x: 14
                                y: Math.round((26 - height) / 2)
                                visible: permissionRow.modelData.enforced
                                glyph: "" // CheckMark
                                size: 12
                                color: Theme.success
                            }
                            Rectangle { // declared only: Win顺 cannot hold the plugin to it
                                id: declaredTag
                                x: 2
                                y: 4
                                visible: !permissionRow.modelData.enforced
                                width: declaredText.implicitWidth + 10
                                height: 18
                                radius: Theme.smallControlRadius
                                color: "transparent"
                                border.width: 1
                                border.color: Theme.controlBorder

                                Text {
                                    id: declaredText
                                    anchors.centerIn: parent
                                    text: qsTr("仅声明")
                                    color: Theme.subtext
                                    font.pixelSize: 11
                                }
                            }
                            Text {
                                id: permissionText
                                anchors.left: parent.left
                                // After the tag when it is wider than the usual margin (English).
                                anchors.leftMargin: declaredTag.visible ? Math.max(60, declaredTag.x + declaredTag.width + 8) : 60
                                anchors.right: parent.right
                                y: 4
                                text: permissionRow.modelData.text
                                      + (permissionRow.modelData.note.length > 0 ? "（" + permissionRow.modelData.note + "）" : "")
                                      + (permissionRow.isNew ? qsTr("（新增）") : "")
                                textFormat: Text.PlainText
                                color: permissionRow.isNew ? Theme.accent : Theme.text
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                                lineHeight: 1.15
                            }
                        }
                    }
                }
            }

            Text { // the plugin design, §3.3: what is enforced and what is only said
                visible: dialog.kind === "permissions"
                width: parent.width
                text: dialog.plugins.permissionsNote()
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }

            Item {
                width: parent.width
                height: primary.implicitHeight

                FlatButton { // uninstall: the plugin alone, its data kept
                    visible: dialog.kind === "uninstall" && dialog.info.hasData === true
                    anchors.left: parent.left
                    flat: true
                    text: qsTr("只删插件，留下数据")
                    onClicked: dialog.accept(false)
                }

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    FlatButton {
                        text: qsTr("取消")
                        onClicked: dialog.reject()
                    }
                    FlatButton {
                        id: primary
                        highlighted: true
                        text: dialog.kind === "permissions" ? qsTr("打开")
                            : dialog.kind === "install" ? qsTr("安装")
                            : dialog.info.hasData === true ? qsTr("连数据一起删") : qsTr("卸载")
                        onClicked: dialog.accept(true)
                    }
                }
            }
        }
    }
}
