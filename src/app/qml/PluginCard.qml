pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A plugin in the settings (PluginSettings.list). Folded, three lines: its
// icon, name and tags, what it does, where it stands with its keywords as
// keys to press; its switch, its menu and a chevron on the right. Unfolded
// (a click on the header, a keycap, the settings search, a plugin's row
// asking for its settings): its keywords to change (with who has one it
// cannot use), 给 AI 用, its permissions (what Windows enforces ticked, the
// rest "仅声明"), the settings its manifest declares, each drawn with
// Win顺's own controls (the plugin design, §4.7), and its version and
// author. Built like ShareCard. Whether it is unfolded is the section's
// (PluginsSection.expandedIds): the list is reset when plugins come or go.
Rectangle {
    id: card

    property SettingsEditor editor: null
    required property bool webSearch
    readonly property real dpr: Screen.devicePixelRatio // one Screen attached object for all its items (GitHub #111)
    readonly property Item webOptions: detailsLoader.item ? detailsLoader.item.webOptions : null
    readonly property bool detailsSettled: !expanded || (detailsLoader.item !== null
        && Math.abs(details.height - detailsLoader.item.implicitHeight) < 0.5)
    signal editWebShortcut(int index, Item button)
    required property PluginSettings plugins
    required property string pluginId
    required property string name
    required property string version
    required property string author
    required property string homepage
    required property string description
    required property bool bundled
    required property string icon
    required property bool pluginEnabled // not `enabled`: that is the Item's own
    required property bool usable
    required property string status
    required property string tone
    required property string error
    required property bool resident
    required property var keywords
    required property var permissions
    required property var settings
    required property bool hasReadme
    required property bool settingsWindow // the plugin's own settings window: 设置 beside the switch
    required property bool hasData
    required property bool aiExpose // the manifest offers its commands to the AI: the switch below shows
    required property bool aiEnabled
    required property string aiNote
    // `elevated` while Win顺 runs as administrator (PluginSettings' Card):
    // "elevated" a shield by the name; "unconfirmed" / "changed" a line with
    // 确认; "unavailable" and "confirming" (the copy being made) the line alone.
    required property string elevation
    required property string elevationNote
    property bool marked: false // what the settings search found
    property bool expanded: false
    property bool animated: true // not in the offscreen preview: it draws no frames to run them

    property bool loadedOnce: false // the details, made on the first unfolding and kept
    property string pendingFocus // the command whose keyword box takes the focus once made

    // Anything to unfold: a plugin that cannot be used has none.
    readonly property bool hasDetails: webSearch || keywords.length > 0 || aiExpose || permissions.length > 0 || settings.length > 0
    readonly property color toneColor: tone === "ok" ? Theme.success
                                     : tone === "busy" ? Theme.accent
                                     : tone === "warn" ? Theme.caution
                                     : tone === "error" ? Theme.danger
                                     : Theme.faint

    signal expandRequested(bool on)

    // Unfolds the card and puts the cursor in a command's keyword box.
    function focusKeyword(command) {
        if (!expanded)
            expandRequested(true)
        pendingFocus = ""
        pendingFocus = command
    }

    function onPixel(v) { return Math.round(v * card.dpr) / card.dpr }

    onExpandedChanged: if (expanded) loadedOnce = true
    Component.onCompleted: if (expanded) loadedOnce = true

    implicitHeight: body.implicitHeight + 24
    radius: Theme.controlRadius
    color: "transparent"

    Hook { // the style's card (Theme.cardPart), a plate inside the section's
        anchors.fill: parent
        part: Theme.cardPart
        corner: Theme.controlRadius
        extra: ({ inset: true })
        dpr: card.dpr
    }
    Rectangle { // found by the settings search: the card's face, marked
        visible: card.marked
        anchors.fill: parent
        anchors.margins: Theme.edge(card.dpr)
        radius: Math.max(0, Theme.controlRadius - Theme.edge(card.dpr))
        color: Theme.selection
    }

    Rectangle { // found by the settings search: a mark by the name, as SettingRow has
        visible: card.marked
        x: 3
        y: body.y + header.y + texts.y + (nameText.height - height) / 2
        width: 3
        height: 16
        radius: Theme.smallRadius
        color: Theme.accent
    }

    Column {
        id: body
        x: 14
        y: 12
        width: parent.width - 28
        spacing: 10

        Item { // the header: icon, names, state; the switch, the menu and the chevron on the right
            id: header
            width: parent.width
            height: Math.max(texts.implicitHeight, controls.implicitHeight)

            MouseArea { // a click anywhere but the controls and the keycaps folds or unfolds it
                id: headerArea
                anchors.fill: parent
                enabled: card.hasDetails
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: card.expandRequested(!card.expanded)
            }

            Item {
                id: iconSlot
                width: 32
                height: 32
                y: 2

                Image { // whole device pixels, unsmoothed (see ResultRow)
                    readonly property real dpr: card.dpr
                    readonly property int pixels: Math.round(32 * dpr)

                    x: 1 / 64
                    y: 1 / 64
                    width: pixels / dpr
                    height: pixels / dpr
                    source: card.icon
                    sourceSize.width: pixels / dpr
                    sourceSize.height: pixels / dpr
                    smooth: false
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                }
            }

            Column {
                id: texts
                anchors.left: iconSlot.right
                anchors.leftMargin: 12
                anchors.right: controls.left
                anchors.rightMargin: 12
                spacing: 4

                Row {
                    width: parent.width
                    spacing: 8

                    Text {
                        id: nameText
                        text: card.name
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Theme.fontStrong
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, parent.width - (bundledTag.visible ? bundledTag.width + 8 : 0)
                                                        - (aiTag.visible ? aiTag.width + 8 : 0)
                                                        - (shield.visible ? shield.width + 8 : 0))
                    }
                    Glyph { // it runs as administrator, as Win顺 does
                        id: shield
                        visible: card.elevation === "elevated"
                        anchors.verticalCenter: nameText.verticalCenter
                        width: 16
                        height: 16
                        glyph: String.fromCharCode(0xE7EF) // Admin: Phosphor's shield-check
                        size: 14
                        color: Theme.subtext
                        Accessible.role: Accessible.Graphic
                        Accessible.name: card.elevationNote

                        MouseArea {
                            id: shieldArea
                            anchors.fill: parent
                            hoverEnabled: true
                        }
                        HoverTip {
                            target: shieldArea.containsMouse ? shield : null
                            text: card.elevationNote
                        }
                    }
                    Tag { // 随包: it came with Win顺
                        id: bundledTag
                        visible: card.bundled
                        text: qsTr("随包")
                    }
                    Tag { // its commands are tools of the AI's
                        id: aiTag
                        visible: card.aiExpose && card.aiEnabled
                        text: "AI"
                    }
                }
                Text { // what it does
                    width: parent.width
                    visible: card.description.length > 0
                    text: card.description
                    textFormat: Text.PlainText
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
                Flow { // where it stands, at a glance (the words say it), then its keywords
                    id: stateLine
                    width: parent.width
                    spacing: 6

                    Item {
                        id: stateItem

                        // A line as tall as a keycap; a long message wraps under its dot.
                        readonly property int lineHeight: 24

                        width: Math.min(dot.width + 8 + statusText.implicitWidth + 6, stateLine.width)
                        height: Math.max(lineHeight, statusText.implicitHeight)

                        Rectangle {
                            id: dot
                            y: Math.round(statusText.y + statusText.fontInfo.pixelSize * 0.62 - height / 2)
                            width: 8
                            height: 8
                            radius: Theme.pillRadius(width)
                            color: card.toneColor

                            SequentialAnimation on opacity { // starting
                                running: card.tone === "busy" && SystemTheme.animations && (card.Window.window?.visible ?? false) // the hidden settings window kept: no frames to drive
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
                            anchors.rightMargin: 6
                            y: lineCount > 1 ? 0 : card.onPixel((stateItem.lineHeight - implicitHeight) / 2)
                            text: card.status
                            textFormat: Text.PlainText
                            color: card.tone === "error" ? Theme.danger : card.tone === "warn" ? Theme.caution : Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                            lineHeight: 1.15
                        }
                    }

                    Repeater {
                        model: card.keywords.filter(k => k.keyword.length > 0)

                        delegate: Item { // the keyword, as keys to press; a click goes to its box
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
                                dpr: card.dpr
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
                                onClicked: card.focusKeyword(keycap.modelData.command)
                            }
                        }
                    }
                }
                Item { // `elevated` waits for the user (or cannot be): why, and the one click
                    visible: card.elevation === "unconfirmed" || card.elevation === "changed" || card.elevation === "unavailable"
                             || card.elevation === "confirming"
                    width: parent.width
                    height: visible ? elevationText.implicitHeight + (confirmButton.visible ? 6 + confirmButton.height : 0) : 0

                    Text {
                        id: elevationText
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        text: card.elevationNote
                        textFormat: Text.PlainText
                        color: Theme.caution
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                        lineHeight: 1.15
                    }
                    FlatButton {
                        dpr: card.dpr
                        id: confirmButton
                        visible: card.elevation === "unconfirmed" || card.elevation === "changed"
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        implicitHeight: 28
                        text: qsTr("确认")
                        onClicked: card.plugins.confirmElevation(card.pluginId)
                    }
                }
            }

            Row {
                id: controls
                anchors.right: parent.right
                y: 0
                spacing: 8

                FlatButton { // the plugin's own window, while it runs
                    dpr: card.dpr
                    anchors.verticalCenter: parent.verticalCenter
                    visible: card.settingsWindow && card.usable && card.pluginEnabled
                    implicitHeight: 32
                    text: qsTr("设置")
                    onClicked: card.plugins.showSettings(card.pluginId)
                }
                ToggleSwitch {
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: card.usable
                    checked: card.pluginEnabled
                    onToggled: (on) => card.plugins.setEnabled(card.pluginId, on)
                }
                FlatButton { // the menu: 说明, 打开文件夹, 重新加载, 查看日志, 卸载
                    dpr: card.dpr
                    id: menuButton
                    anchors.verticalCenter: parent.verticalCenter
                    implicitHeight: 32
                    implicitWidth: 32
                    flat: true
                    glyph: "" // More
                    onClicked: menu.open = !menu.open
                }
                Item { // folded or not; the header's click does it
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20
                    height: 32
                    opacity: card.hasDetails ? 1 : 0

                    Glyph {
                        anchors.centerIn: parent
                        glyph: "" // ChevronDown
                        size: 12
                        color: headerArea.containsMouse ? Theme.text : Theme.subtext
                        rotation: card.expanded ? 180 : 0

                        Behavior on rotation {
                            enabled: card.animated && SystemTheme.animations
                            NumberAnimation { duration: 200; easing.type: Easing.BezierSpline; easing.bezierCurve: [0, 0, 0, 1, 1, 1] }
                        }
                    }
                }
            }
        }

        Item { // the details, unfolding
            id: details

            width: parent.width
            height: card.expanded && detailsLoader.item ? detailsLoader.item.implicitHeight : 0
            visible: height > 0
            clip: true

            // As quick as Windows' own; folding starts at once.
            Behavior on height {
                enabled: card.animated && SystemTheme.animations
                NumberAnimation {
                    duration: 200
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: card.expanded ? [0, 0, 0, 1, 1, 1] : [0.3, 0, 1, 1, 1, 1]
                }
            }

            Loader {
                id: detailsLoader
                width: parent.width
                active: card.loadedOnce
                sourceComponent: detailsComponent
            }
        }
    }

    Component {
        id: detailsComponent

        Column {
            property alias webOptions: webOptionsLoader.item
            width: detailsLoader.width
            spacing: 12

            Rectangle {
                width: parent.width
                height: Theme.edge(card.dpr)
                color: Theme.divider
            }

            Loader {
                id: webOptionsLoader
                width: parent.width
                active: card.webSearch && card.expanded && card.pluginEnabled && card.editor !== null
                sourceComponent: WebSearchOptions {
                    editor: card.editor
                    onEditRequested: (index, button) => card.editWebShortcut(index, button)
                }
            }

            Column { // the keywords: two to a line where they fit
                width: parent.width
                spacing: 6
                visible: card.keywords.length > 0

                Text {
                    text: qsTr("关键字")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
                Grid {
                    id: keywordGrid

                    readonly property int cellWidth: columns > 1 ? Math.floor((width - columnSpacing) / 2) : width

                    width: parent.width
                    columns: width >= 540 ? 2 : 1
                    columnSpacing: 24
                    rowSpacing: 6

                    Repeater {
                        model: card.keywords

                        delegate: Column {
                            id: keywordCell

                            required property var modelData

                            function takeFocus() {
                                if (card.pendingFocus.length > 0 && card.pendingFocus === modelData.command) {
                                    card.pendingFocus = ""
                                    keywordBox.takeFocus()
                                }
                            }

                            width: keywordGrid.cellWidth
                            spacing: 4
                            Component.onCompleted: takeFocus()

                            Connections {
                                target: card
                                function onPendingFocusChanged() { keywordCell.takeFocus() }
                            }

                            Item {
                                width: parent.width
                                height: 32

                                Text {
                                    id: keywordLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 84
                                    text: keywordCell.modelData.title
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    font.pixelSize: Theme.fontCaption
                                    elide: Text.ElideRight
                                }
                                InputBox {
                                    id: keywordBox
                                    anchors.left: keywordLabel.right
                                    anchors.leftMargin: 8
                                    anchors.right: resetButton.visible ? resetButton.left : parent.right
                                    anchors.rightMargin: resetButton.visible ? 4 : 0
                                    anchors.verticalCenter: parent.verticalCenter
                                    implicitHeight: 32
                                    text: keywordCell.modelData.keyword
                                    placeholder: qsTr("无")
                                    onEditingFinished: {
                                        if (text !== keywordCell.modelData.keyword)
                                            card.plugins.setKeyword(card.pluginId, keywordCell.modelData.command, text)
                                    }
                                }
                                FlatButton { // the keyword was changed: back to the plugin's
                                    dpr: card.dpr
                                    id: resetButton
                                    visible: keywordCell.modelData.user
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    implicitHeight: 32
                                    flat: true
                                    text: qsTr("默认")
                                    onClicked: card.plugins.resetKeyword(card.pluginId, keywordCell.modelData.command)
                                }
                            }
                            Text { // who has it
                                visible: keywordCell.modelData.conflict.length > 0
                                width: parent.width
                                text: keywordCell.modelData.conflict
                                textFormat: Text.PlainText
                                color: Theme.danger
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }

            Item { // 给 AI 用: its commands as the AI's tools (the AI page's switch gates it)
                width: parent.width
                height: aiColumn.implicitHeight
                visible: card.aiExpose

                Column {
                    id: aiColumn
                    anchors.left: parent.left
                    anchors.right: aiSwitch.left
                    anchors.rightMargin: 12
                    spacing: 2

                    Text {
                        width: parent.width
                        text: qsTr("给 AI 用")
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0
                        text: card.aiNote
                        textFormat: Text.PlainText
                        color: Theme.faint
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
                ToggleSwitch {
                    id: aiSwitch
                    anchors.right: parent.right
                    y: Math.round((aiColumn.implicitHeight - height) / 2)
                    checked: card.aiEnabled
                    onToggled: (on) => card.plugins.setAiEnabled(card.pluginId, on)
                }
            }

            Column { // the permissions
                width: parent.width
                spacing: 2
                visible: card.permissions.length > 0

                Text {
                    height: 24
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("权限")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }

                Repeater {
                    model: card.permissions

                    delegate: Item {
                        id: permissionRow

                        required property var modelData

                        // A line of 24; a long sentence (the sandbox's) wraps, the
                        // tick or tag staying on its first line.
                        width: parent.width
                        height: Math.max(24, permissionText.implicitHeight + 6)

                        Glyph { // enforced by Windows: a tick
                            id: tick
                            x: 16
                            y: Math.round((24 - height) / 2)
                            visible: permissionRow.modelData.enforced
                            glyph: "" // CheckMark
                            size: 12
                            color: Theme.success
                        }
                        Rectangle { // declared only: Win顺 cannot hold the plugin to it
                            id: declaredTag
                            x: 6
                            y: 3
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
                            anchors.leftMargin: declaredTag.visible ? Math.max(64, declaredTag.x + declaredTag.width + 8) : 64
                            anchors.right: parent.right
                            y: 3
                            text: permissionRow.modelData.text
                                  + (permissionRow.modelData.note.length > 0 ? "（" + permissionRow.modelData.note + "）" : "")
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                            lineHeight: 1.15
                        }
                    }
                }
            }

            Column { // the settings the manifest declares
                width: parent.width
                spacing: 6
                visible: card.settings.length > 0

                Rectangle {
                    width: parent.width
                    height: Theme.edge(card.dpr)
                    color: Theme.divider
                }

                Repeater {
                    model: card.settings

                    delegate: Item {
                        id: settingRow

                        required property var modelData
                        readonly property string kind: modelData.type

                        width: parent.width
                        height: Math.max(36, settingControl.implicitHeight) + (settingHint.visible ? settingHint.implicitHeight + 4 : 0)

                        Text {
                            id: settingTitle
                            anchors.left: parent.left
                            anchors.right: settingControl.left
                            anchors.rightMargin: 12
                            y: Math.round((Math.max(36, settingControl.implicitHeight) - height) / 2)
                            text: settingRow.modelData.title
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
                        }
                        Text {
                            id: settingHint
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: settingControl.bottom
                            anchors.topMargin: 4
                            visible: text.length > 0
                            text: settingRow.modelData.hint ?? ""
                            textFormat: Text.PlainText
                            color: Theme.faint
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }

                        Item { // the control for its type
                            id: settingControl

                            readonly property Item control: controlLoader.item as Item

                            anchors.right: parent.right
                            implicitWidth: control ? control.implicitWidth : 0
                            implicitHeight: control ? control.implicitHeight : 0
                            width: implicitWidth
                            height: implicitHeight

                            Loader {
                                id: controlLoader
                                sourceComponent: settingRow.kind === "bool" ? boolControl
                                               : settingRow.kind === "choice" ? choiceControl
                                               : settingRow.kind === "folder" || settingRow.kind === "file" ? pathControl
                                               : settingRow.kind === "hotkey" ? hotkeyControl
                                               : textControl
                            }

                            Component {
                                id: boolControl
                                ToggleSwitch {
                                    checked: settingRow.modelData.value === true
                                    onToggled: (on) => card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key, on)
                                }
                            }
                            Component {
                                id: textControl
                                InputBox { // text, number, password (masked, kept by the Credential Manager)
                                    width: 220
                                    implicitHeight: 32
                                    text: settingRow.kind === "password" ? "" : String(settingRow.modelData.value ?? "")
                                    placeholder: settingRow.kind === "password" ? (settingRow.modelData.set ? qsTr("已保存，输入可替换") : qsTr("未设置")) : ""
                                    echoMode: settingRow.kind === "password" ? TextInput.Password : TextInput.Normal
                                    validator: settingRow.kind === "number" ? numberValidator : null
                                    onEditingFinished: {
                                        if (settingRow.kind === "password") {
                                            if (text.length > 0) {
                                                card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key, text)
                                                clear()
                                            }
                                        } else if (text !== String(settingRow.modelData.value ?? "")) {
                                            card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key, text)
                                        }
                                    }

                                    DoubleValidator { id: numberValidator; notation: DoubleValidator.StandardNotation }
                                }
                            }
                            Component {
                                id: choiceControl
                                // Side by side while they fit beside the title; else
                                // one button with the chosen one's short name (what
                                // comes before its "（…）") and a menu of them all.
                                Item {
                                    id: choice

                                    readonly property var labels: settingRow.modelData.options ?? []
                                    readonly property int index: settingRow.modelData.optionIndex ?? 0
                                    readonly property bool fits: tabs.implicitWidth <= settingRow.width - settingTitle.implicitWidth - 24

                                    function choose(index) {
                                        card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key,
                                                                     settingRow.modelData.optionValues[index])
                                    }
                                    function shortName(label) {
                                        return String(label ?? "").split("（")[0].split(" (")[0].trim()
                                    }

                                    implicitWidth: fits ? tabs.implicitWidth : menuButton.implicitWidth
                                    implicitHeight: fits ? tabs.implicitHeight : menuButton.implicitHeight

                                    ScopeTabs {
                                        id: tabs
                                        visible: choice.fits
                                        labels: choice.labels
                                        current: choice.index
                                        onActivated: (index) => choice.choose(index)
                                    }
                                    FlatButton {
                                        dpr: card.dpr
                                        id: menuButton
                                        visible: !choice.fits
                                        implicitHeight: 32
                                        text: choice.shortName(choice.labels[choice.index])
                                        trailingGlyph: String.fromCharCode(0xE70D) // ChevronDown
                                        onClicked: choiceMenu.open = !choiceMenu.open

                                        DropMenu {
                                            id: choiceMenu
                                            anchor: menuButton
                                            entries: choice.labels.map((label, i) => ({
                                                text: label,
                                                glyph: i === choice.index ? String.fromCharCode(0xE73E) : "" // CheckMark
                                            }))
                                            onActivated: (index) => choice.choose(index)
                                        }
                                    }
                                }
                            }
                            Component {
                                id: pathControl
                                Row {
                                    spacing: 8

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.min(implicitWidth, 260)
                                        text: String(settingRow.modelData.value ?? "").length > 0 ? String(settingRow.modelData.value) : qsTr("未设置")
                                        textFormat: Text.PlainText
                                        color: String(settingRow.modelData.value ?? "").length > 0 ? Theme.text : Theme.faint
                                        font.pixelSize: Theme.fontCaption
                                        elide: Text.ElideMiddle
                                    }
                                    FlatButton {
                                        dpr: card.dpr
                                        implicitHeight: 32
                                        text: qsTr("选择")
                                        onClicked: settingRow.kind === "folder" ? card.plugins.pickFolderSetting(card.pluginId, settingRow.modelData.key)
                                                                                : card.plugins.pickFileSetting(card.pluginId, settingRow.modelData.key)
                                    }
                                }
                            }
                            Component {
                                id: hotkeyControl
                                HotkeyRecorder {
                                    implicitHeight: 32
                                    hotkey: String(settingRow.modelData.value ?? "")
                                    onRecordingChanged: card.plugins.recordingHotkey = recording
                                    onRecorded: (hotkey) => card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key, hotkey)
                                }
                            }
                        }
                    }
                }
            }

            Text { // who made which version of it
                width: parent.width
                visible: text.length > 0
                text: [card.version, card.author].filter(s => s.length > 0).join(" · ")
                textFormat: Text.PlainText
                color: Theme.faint
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Item { // room under the last line, inside the clip
                width: 1
                height: 2
            }
        }
    }

    // The "…" menu, under its button (on top of the whole window: DropMenu).
    DropMenu {
        id: menu
        anchor: menuButton
        menuWidth: 180
        entries: [
            { text: qsTr("说明"), glyph: "", act: () => card.plugins.openReadme(card.pluginId), shown: card.hasReadme, danger: false }, // Document
            { text: qsTr("主页"), glyph: "", act: () => card.plugins.openHomepage(card.pluginId), shown: card.homepage.length > 0, danger: false }, // Link
            { text: qsTr("打开文件夹"), glyph: "", act: () => card.plugins.openFolder(card.pluginId), shown: true, danger: false }, // FolderOpen
            { text: qsTr("重新加载"), glyph: "", act: () => card.plugins.reload(card.pluginId), shown: true, danger: false }, // Refresh
            { text: qsTr("查看日志"), glyph: "", act: () => card.plugins.openLog(), shown: true, danger: false }, // Log
            { text: qsTr("卸载"), glyph: "", act: () => card.plugins.uninstall(card.pluginId), shown: !card.bundled, danger: true } // Delete
        ].filter(e => e.shown)
        onActivated: (index) => menu.entries[index].act()
    }
}
