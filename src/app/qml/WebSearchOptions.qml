pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The declarative web plugin's settings, loaded only for its expanded card.
Column {
    id: webSection
    property real dpr: Screen.devicePixelRatio // the window's, passed by a host that has it (GitHub #111)
    required property SettingsEditor editor
    readonly property Item addButton: webAddButton
    property string message
    signal editRequested(int index, Item button)
    spacing: 8

    Text {
        width: parent.width
        text: qsTranslate("SettingsWindow", "输入关键词按 Enter 打开网页；网址里有 %s 的，在关键词后加空格和文字就能搜索。")
        wrapMode: Text.Wrap
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
    }
    SettingRow {
        title: qsTranslate("SettingsWindow", "关键词")
        keywords: qsTranslate("SettingsWindow", "网址;网页;搜索引擎;书签;添加;url").split(";")
        values: {
            const words = []
            for (const shortcut of webSection.editor.webShortcuts)
                words.push(shortcut.keyword, shortcut.name, shortcut.shownUrl)
            return words
        }
        description: qsTranslate("SettingsWindow", "输入名称或拼音也能找到")

        FlatButton {
            dpr: webSection.dpr
            flat: true
            text: qsTranslate("SettingsWindow", "导入")
            glyph: "\uE8B5" // Import
            onClicked: {
                const message = webSection.editor.importWebShortcuts()
                if (message.length > 0)
                    webSection.message = message
            }
        }
        FlatButton {
            dpr: webSection.dpr
            flat: true
            text: qsTranslate("SettingsWindow", "导出")
            glyph: "\uEDE1" // Export
            onClicked: {
                const message = webSection.editor.exportWebShortcuts()
                if (message.length > 0)
                    webSection.message = message
            }
        }
        FlatButton {
            dpr: webSection.dpr
            id: webAddButton
        objectName: "webAddButton"
            text: qsTranslate("SettingsWindow", "添加")
            glyph: "\uE710" // Add
            onClicked: webSection.editRequested(-1, webAddButton)
        }

        body: [Text {
            visible: webSection.message.length > 0
            width: parent.width
            bottomPadding: 8
            text: webSection.message
            textFormat: Text.PlainText
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }, Rectangle {
            width: parent.width
            height: Math.max(webList.implicitHeight, 40)
            radius: Theme.controlRadius
            color: "transparent"
            border.width: 1
            border.color: Theme.cardBorder

            Text {
                visible: webSection.editor.webShortcuts.length === 0
                anchors.centerIn: parent
                text: qsTranslate("SettingsWindow", "还没有网页搜索，点“添加”加一个")
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
            }

            Column {
                id: webList
                width: parent.width

                Repeater {
                    model: webSection.editor.webShortcuts

                    delegate: Item {
                        id: webRow

                        required property int index
                        required property var modelData
                        width: webList.width
                        height: 58

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: Theme.smallControlRadius
                            color: webArea.containsMouse ? Theme.navHover : "transparent"
                        }
                        Rectangle {
                            visible: webRow.index > 0
                            x: 12
                            width: parent.width - 24
                            height: Theme.edge(webSection.dpr)
                            color: Theme.divider
                        }
                        MouseArea {
                            id: webArea
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }

                        Item { // its site's icon, as its rows show it in the launcher
                            id: siteIcon
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: 24
                            height: 24

                            Image {
                                readonly property real dpr: Screen.devicePixelRatio
                                readonly property int pixels: Math.round(siteIcon.width * dpr)

                                // Exactly its device pixels, unsmoothed (as in ResultRow).
                                x: 1 / 64
                                y: 1 / 64
                                width: pixels / dpr
                                height: pixels / dpr
                                sourceSize.width: pixels / dpr
                                sourceSize.height: pixels / dpr
                                smooth: false
                                asynchronous: true
                                // Asked again when an icon arrives (siteIconsArrived).
                                source: webSection.editor.siteIconsArrived >= 0
                                        ? webSection.editor.webIconUrl(webRow.modelData.iconRef, Theme.dark) : ""
                            }
                        }
                        Item { // the keyword, as keys to press
                            id: keycap
                            anchors.left: siteIcon.right
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(44, keywordText.implicitWidth + 16)
                            height: 26

                            Hook { // the style's key cap (Theme.keyHint): it only tells
                                id: keycapPart
                                anchors.fill: parent
                                part: Theme.keyHint
                                dpr: webSection.dpr
                            }
                            Text {
                                id: keywordText
                                anchors.centerIn: parent
                                anchors.verticalCenterOffset: keycapPart.out("contentOffsetY", 0)
                                text: webRow.modelData.keyword
                                textFormat: Text.PlainText
                                color: keycapPart.out("foreground", Theme.text)
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                        Column {
                            anchors.left: keycap.right
                            anchors.leftMargin: 12
                            anchors.right: webButtons.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                width: parent.width
                                text: webRow.modelData.name
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: webRow.modelData.searches ? webRow.modelData.shownUrl
                                      : qsTranslate("SettingsWindow", "%1 · 只能打开，不能搜索").arg(webRow.modelData.shownUrl)
                                textFormat: Text.PlainText
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideMiddle
                            }
                        }
                        Row {
                            id: webButtons
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            FlatButton {
                                dpr: webSection.dpr
                                implicitHeight: 32
                                flat: true
                                text: qsTranslate("SettingsWindow", "打开")
                                onClicked: webSection.editor.openWebShortcut(webRow.index)
                            }
                            FlatButton {
                                dpr: webSection.dpr
                                id: webEditButton
                                objectName: "webEditButton"
                                implicitHeight: 32
                                flat: true
                                text: qsTranslate("SettingsWindow", "编辑")
                                onClicked: webSection.editRequested(webRow.index, webEditButton)
                            }
                            FlatButton {
                                dpr: webSection.dpr
                                implicitHeight: 32
                                flat: true
                                text: qsTranslate("SettingsWindow", "移除")
                                onClicked: webSection.editor.removeWebShortcut(webRow.index)
                            }
                        }
                    }
                }
            }
        }]
    }

    SettingRow {
        title: qsTranslate("SettingsWindow", "网页搜索建议")
        keywords: qsTranslate("SettingsWindow", "搜索建议;联想;补全;suggest;suggestion").split(";")
        description: qsTranslate("SettingsWindow", "在网站关键词后输入文字时，列出这个网站的搜索建议。只有这时才会把输入的文字发给那个网站")

        ToggleSwitch {
            checked: webSection.editor.webSuggestions
            onToggled: (on) => webSection.editor.webSuggestions = on
        }
    }
}
