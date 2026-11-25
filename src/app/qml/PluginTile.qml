pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// An installed plugin (PluginSettings.list) as a tile of 已安装的插件's grid
// (TileFlow, PluginsSection): its icon, name, version and tags; what it
// does, three lines at most; its first keywords as keys to press; where it
// stands when that needs a look (starting, waiting for 确认, broken, a newer
// version downloaded), with the one click it may need; along the bottom 详情
// and its own 设置 window, its switch on the right. A click anywhere else, 详情
// or a key opens it: the whole card (PluginCard) in place of the grid, with
// its keywords, permissions and settings. The grid sizes it (its row as
// tall as the tallest there); its own content sets `implicitHeight`.
Rectangle {
    id: tile

    required property PluginSettings plugins
    required property string pluginId
    required property string name
    required property string version
    required property string author
    required property string description
    required property bool bundled
    required property string icon
    required property bool pluginEnabled // not `enabled`: that is the Item's own
    required property bool usable
    required property string status
    required property string tone
    required property bool resident
    required property var keywords
    required property var permissions
    required property bool settingsWindow // the plugin's own settings window: 设置 by the switch
    required property bool aiExpose
    required property bool aiEnabled
    required property string elevation
    required property string elevationNote
    required property string staged // a newer version downloaded, waiting for the next start
    required property string report // what its running process says of its work (winshun/status)
    required property string reportTone
    required property real progress
    required property var reportActions
    required property string elevationProblem // why the last 确认 did not work
    property bool marked: false // just opened for (PluginsSection.reveal)

    readonly property real dpr: Screen.devicePixelRatio // one Screen attached object for all its items (GitHub #111)
    readonly property real pad: 14
    // Starting, waiting for a yes, or broken: said on the tile. Running or ready is only in the tip.
    readonly property bool noteworthy: tone === "busy" || tone === "warn" || tone === "error"
    readonly property bool elevationShown: elevation === "unconfirmed" || elevation === "changed" || elevation === "unavailable"
                                           || elevation === "confirming" || elevation === "host"
    readonly property var shownKeywords: keywords.filter(k => k.keyword.length > 0).slice(0, 3)
    readonly property string infoText: {
        const lines = []
        if (descriptionText.truncated)
            lines.push(description, "")
        if (author.length > 0)
            lines.push(qsTr("作者：%1").arg(author))
        if (status.length > 0 && !noteworthy)
            lines.push(qsTr("状态：%1").arg(status))
        if (permissions.length > 0) {
            lines.push(qsTr("权限："))
            for (const p of permissions)
                lines.push("• " + p.text + (p.note.length > 0 ? "（" + p.note + "）" : "") + (p.enforced ? "" : qsTr("（仅声明）")))
        }
        return lines.join("\n")
    }
    readonly property color toneColor: tone === "busy" ? Theme.accent
                                     : tone === "warn" ? Theme.caution
                                     : tone === "error" ? Theme.danger
                                     : Theme.faint

    signal openRequested()
    signal keywordRequested(string command) // a key pressed: opened at its keyword's box

    implicitHeight: Theme.snapUp(top.y + top.implicitHeight + 12 + footer.height + 12, dpr)
    // One element for UI Automation; one filtered away is skipped whole (see SettingsSection).
    Accessible.role: Accessible.Grouping
    Accessible.name: name
    radius: Theme.controlRadius
    color: "transparent"

    Hook { // the style's card (Theme.cardPart), a plate inside the section's
        anchors.fill: parent
        part: Theme.cardPart
        corner: Theme.controlRadius
        extra: ({ inset: true })
        dpr: tile.dpr
    }
    Rectangle { // under the pointer, or just opened for: it opens on a click
        visible: tileArea.containsMouse || tile.marked
        anchors.fill: parent
        anchors.margins: Theme.edge(tile.dpr)
        radius: Math.max(0, Theme.controlRadius - Theme.edge(tile.dpr))
        color: tile.marked ? Theme.selection : Theme.hover
    }
    MouseArea { // under the controls: a click anywhere else opens it
        id: tileArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: tile.openRequested()
    }

    Column {
        id: top
        x: tile.pad
        y: Theme.snap(14, tile.dpr)
        width: parent.width - 2 * x
        spacing: Theme.snap(10, tile.dpr)

        Item { // the icon; the name and the "i"; the version and its tags
            width: parent.width
            height: Theme.snapUp(Math.max(36, names.implicitHeight), tile.dpr)

            Image { // whole device pixels, unsmoothed (see ResultRow)
                readonly property int pixels: Math.round(36 * tile.dpr)

                x: 1 / 64
                y: 1 / 64
                width: pixels / tile.dpr
                height: pixels / tile.dpr
                source: tile.icon
                sourceSize.width: pixels / tile.dpr
                sourceSize.height: pixels / tile.dpr
                smooth: false
                asynchronous: true
                fillMode: Image.PreserveAspectFit
            }

            Column {
                id: names
                x: 36 + 12
                width: parent.width - x - infoDot.width - 8
                spacing: 2

                Text {
                    id: nameText
                    width: parent.width
                    height: Theme.snapUp(implicitHeight, tile.dpr)
                    text: tile.name
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Theme.fontStrong
                    elide: Text.ElideRight
                }
                Row { // the version; it came with Win顺; the AI uses it; it runs as administrator
                    width: parent.width
                    height: Theme.snapUp(Math.max(versionText.implicitHeight, bundledTag.visible || aiTag.visible ? bundledTag.implicitHeight : 0), tile.dpr)
                    spacing: 6

                    Text {
                        id: versionText
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width - (bundledTag.visible ? bundledTag.width + 6 : 0)
                                                        - (aiTag.visible ? aiTag.width + 6 : 0) - (shield.visible ? shield.width + 6 : 0))
                        text: tile.version
                        textFormat: Text.PlainText
                        color: Theme.faint
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                    Tag {
                        id: bundledTag
                        anchors.verticalCenter: parent.verticalCenter
                        visible: tile.bundled
                        dpr: tile.dpr
                        text: qsTr("随包")
                    }
                    Tag {
                        id: aiTag
                        anchors.verticalCenter: parent.verticalCenter
                        visible: tile.aiExpose && tile.aiEnabled
                        dpr: tile.dpr
                        text: "AI"
                    }
                    Glyph {
                        id: shield
                        anchors.verticalCenter: parent.verticalCenter
                        visible: tile.elevation === "elevated"
                        glyph: String.fromCharCode(0xE7EF) // Admin: Phosphor's shield-check
                        size: 16
                        color: Theme.subtext
                        Accessible.role: Accessible.Graphic
                        Accessible.name: tile.elevationNote
                    }
                }
            }
            InfoDot { // who made it, how it stands, what it may do; the whole description when it is cut
                id: infoDot
                objectName: "pluginInfo"
                anchors.right: parent.right
                y: Theme.snap((nameText.height - height) / 2, tile.dpr)
                dpr: tile.dpr
                text: tile.infoText
            }
        }

        Text { // what it does
            id: descriptionText
            width: parent.width
            height: Theme.snapUp(implicitHeight, tile.dpr)
            visible: tile.description.length > 0
            text: tile.description
            textFormat: Text.PlainText
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            lineHeight: 1.15
        }

        Flow { // its first keywords, as keys to press: a key opens it at its box
            visible: tile.shownKeywords.length > 0
            width: parent.width
            spacing: 6

            Repeater {
                model: tile.shownKeywords

                delegate: Item {
                    id: keycap

                    required property var modelData

                    width: Math.max(28, keycapText.implicitWidth + 14)
                    height: 24

                    Hook { // the style's key (Theme.keySurface): it can be pressed
                        id: keycapPart
                        anchors.fill: parent
                        part: Theme.keySurface
                        hovered: keycapArea.containsMouse
                        pressed: keycapArea.pressed
                        role: "toolbar"
                        dpr: tile.dpr
                    }
                    Text {
                        id: keycapText
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: keycapPart.out("contentOffsetY", 0)
                        text: keycap.modelData.keyword
                        textFormat: Text.PlainText
                        color: keycap.modelData.conflict.length > 0 ? Theme.danger : keycapPart.out("foreground", Theme.text)
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: keycapArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: tile.keywordRequested(keycap.modelData.command)
                    }
                }
            }
        }

        Item { // where it stands, only when that needs a look (the tip says it otherwise)
            visible: tile.noteworthy
            width: parent.width
            height: Theme.snapUp(Math.max(16, statusText.implicitHeight), tile.dpr)

            Rectangle {
                id: dot
                y: Math.round(statusText.y + statusText.fontInfo.pixelSize * 0.62 - height / 2)
                width: 8
                height: 8
                radius: Theme.pillRadius(width)
                color: tile.toneColor

                SequentialAnimation on opacity { // starting
                    running: tile.tone === "busy" && SystemTheme.animations && (tile.Window.window?.visible ?? false)
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation { to: 0.3; duration: 600; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1; duration: 600; easing.type: Easing.InOutSine }
                }
            }
            Text {
                id: statusText
                anchors.left: dot.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                text: tile.status
                textFormat: Text.PlainText
                color: tile.tone === "error" ? Theme.danger : tile.tone === "warn" ? Theme.caution : Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
        }

        PluginReport { // an index being built: how far (its buttons are on the card)
            objectName: "pluginReport"
            width: parent.width
            dpr: tile.dpr
            buttons: false
            maximumLineCount: 2
            text: tile.report
            tone: tile.reportTone
            progress: tile.progress
        }

        Column { // `elevated` waits for the user (or cannot be): why, what the last 确认 came to, and the one click
            visible: tile.elevationShown
            width: parent.width
            spacing: 6

            Text {
                width: parent.width
                text: tile.elevationNote
                textFormat: Text.PlainText
                color: tile.elevation === "host" ? Theme.subtext : Theme.caution // nothing wrong: how it is
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            Text {
                objectName: "elevationProblem"
                visible: tile.elevationProblem.length > 0
                width: parent.width
                text: tile.elevationProblem
                textFormat: Text.PlainText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            FlatButton {
                dpr: tile.dpr
                objectName: "elevationConfirm"
                visible: tile.elevation === "unconfirmed" || tile.elevation === "changed"
                implicitHeight: 28
                text: tile.elevationProblem.length > 0 ? qsTr("再试一次") : qsTr("确认")
                onClicked: tile.plugins.confirmElevation(tile.pluginId)
            }
            FlatButton {
                dpr: tile.dpr
                objectName: "elevationRestart"
                visible: tile.elevation === "host"
                implicitHeight: 28
                text: qsTr("以管理员身份重新启动")
                onClicked: tile.plugins.restartAsAdministrator()
            }
        }

        Row { // a newer version is downloaded: it waited for the plugin's program to stop
            visible: tile.staged.length > 0
            width: parent.width
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - stagedButton.width - parent.spacing)
                text: qsTr("新版本 %1 已下载").arg(tile.staged)
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
            FlatButton {
                id: stagedButton
                dpr: tile.dpr
                objectName: "applyStagedUpdate"
                implicitHeight: 28
                highlighted: true
                text: qsTr("现在更新")
                onClicked: tile.plugins.gallery.applyNow(tile.pluginId)

                HoverHandler { id: stagedHover }
                HoverTip {
                    target: stagedHover.hovered ? stagedButton : null
                    dpr: tile.dpr
                    maxWidth: 320
                    text: qsTr("新版本已经下载好。现在更新会先停一下这个插件；不点的话，下次启动 Win顺 时自动换上")
                }
            }
        }
    }

    // Along the bottom, whatever the words above: 详情 and its own 设置 on
    // the left, its switch on the right.
    Item {
        id: footer
        x: tile.pad
        y: Theme.snap(tile.height - 12 - height, tile.dpr)
        width: parent.width - 2 * x
        height: 32

        Row {
            x: -12 // 详情's words under the words above: a flat button's padding left out
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            FlatButton {
                objectName: "pluginDetails-" + tile.pluginId
                dpr: tile.dpr
                implicitHeight: 32
                flat: true
                text: qsTr("详情")
                trailingGlyph: String.fromCharCode(0xE76C) // ChevronRight
                Accessible.name: qsTr("%1详情").arg(tile.name)
                onClicked: tile.openRequested()
            }
            FlatButton { // the plugin's own settings window
                objectName: "pluginSettings-" + tile.pluginId
                dpr: tile.dpr
                visible: tile.settingsWindow && tile.usable && tile.pluginEnabled
                implicitHeight: 32
                flat: true
                text: qsTr("设置")
                onClicked: tile.plugins.showSettings(tile.pluginId)
            }
        }
        ToggleSwitch {
            dpr: tile.dpr
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            word: false
            enabled: tile.usable
            checked: tile.pluginEnabled
            Accessible.name: tile.name
            onToggled: (on) => tile.plugins.setEnabled(tile.pluginId, on)
        }
    }
}
