pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The settings window: categories on the left, the chosen one's options on the
// right. Changes are saved and applied as soon as they are made.
Window {
    id: window

    required property SettingsEditor editor
    required property WindowFrame frame
    required property Updater updater

    readonly property var renderers: ["software", "d3d11"]
    readonly property var languages: ["system", "zh", "en"]
    readonly property var transparencies: ["off", "on"]
    readonly property var dialogBarPlaces: ["auto", "below", "left", "right"]
    readonly property var pages: [
        { title: qsTr("打开 Win顺"), glyph: "" }, // Keyboard
        { title: qsTr("外观"), glyph: "\uE771" }, // Personalize
        { title: qsTr("搜索范围"), glyph: "" }, // Folder
        { title: qsTr("文件内容搜索"), glyph: "" }, // Document
        { title: qsTr("网页搜索"), glyph: "\uE774" }, // Globe
        { title: qsTr("剪贴板"), glyph: "\uE77F" }, // Paste
        { title: qsTr("高级"), glyph: "" }, // Settings
        { title: Gleaning.title, gleaning: true } // the author's works, set apart at the bottom of the list
    ]
    readonly property int gleaningPage: 7
    readonly property int webPage: 4
    readonly property int clipboardPage: 5
    readonly property int advancedPage: 6 // where the updates are
    property int currentPage: 0
    // On the 拾穗计划 page the whole window, sidebar included, is a warm scene.
    readonly property bool warm: currentPage === gleaningPage

    width: 920
    height: 720
    minimumWidth: 760
    minimumHeight: 420
    // See-through to Mica; while the window is inactive DWM draws a flat grey
    // there instead, which the page covers. Transparent, not a translucent
    // colour: Qt clears with it unpremultiplied, and a light one comes out
    // white.
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.page
    title: qsTr("设置") // shown as "设置 - Win顺"
    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)

    function clearFocus() { window.contentItem.forceActiveFocus() }

    function showPage(index) {
        currentPage = index
        flick.cancelFlick()
        flick.contentY = 0
    }

    function showUpdateDialog() { updateDialog.show() } // App::showUpdate
    function showClipboardPage() { showPage(clipboardPage) } // App::showClipboardSettings
    function showWebPage() { showPage(webPage) } // App::showWebSettings

    // "刚刚", "5 分钟前": when the last update check was.
    property date now: new Date()
    Timer {
        interval: 60000
        repeat: true
        running: window.visible
        onTriggered: window.now = new Date()
    }
    function sinceText(date) {
        const minutes = Math.floor((window.now - date) / 60000)
        if (minutes < 1)
            return qsTr("刚刚")
        if (minutes < 60)
            return qsTr("%n 分钟前", "", minutes)
        if (minutes < 24 * 60)
            return qsTr("%n 小时前", "", Math.floor(minutes / 60))
        return qsTr("%n 天前", "", Math.floor(minutes / (24 * 60)))
    }

    function navItemAt(index) {
        return index === gleaningPage ? gleaningNav : navItems.itemAt(index)
    }

    // Theme and language change everything at once; the old look fades out
    // over the new one instead of snapping.
    function changeAppearance(change) {
        if (!SystemTheme.animations || !window.visible || fadeOut.running) {
            change()
            return
        }
        window.contentItem.grabToImage(result => {
            snapshot.source = result.url
            change()
            fadeOut.restart()
        })
    }

    onVisibleChanged: if (visible && warm) AuthorClock.play()

    GleaningBackdrop { // under the title bar too: the sky reaches the top of the window
        anchors.fill: parent
        visible: window.warm
        // Painted on the CPU: only while someone is looking at it.
        running: window.active && window.visibility !== Window.Minimized && SystemTheme.animations
        leadingInset: nav.width
    }

    Binding { // suspends the global hotkey and hands the recorder the keyboard (see ShortcutCapture)
        target: window.editor
        property: "recordingHotkey"
        value: hotkeyRecorder.recording || clipHotkeyRecorder.recording
    }

    TitleBar {
        id: titleBar
        z: 1
        width: parent.width
        frame: window.frame
        title: window.title + " - " + Qt.application.displayName
        iconX: navList.x + 16 // over the categories' icons, the title over their names
        foreground: window.warm ? WarmPalette.ink : Theme.text
    }

    Item { // category list
        id: nav
        y: titleBar.height
        width: 248
        height: parent.height - y

        MouseArea { // clicking empty space finishes editing a field
            anchors.fill: parent
            onClicked: window.clearFocus()
        }

        component NavItem: Rectangle {
            id: navItem

            required property int index
            required property var modelData
            readonly property bool selected: index === window.currentPage
            readonly property bool gleaning: modelData.gleaning === true
            readonly property color foreground: window.warm ? (selected ? "white" : WarmPalette.ink)
                                              : navArea.pressed ? Theme.subtext : Theme.text

            function select(to) {
                window.showPage(to)
                window.navItemAt(to).forceActiveFocus()
            }

            width: navList.width
            height: navList.itemHeight
            radius: 4
            color: window.warm ? (navArea.containsMouse && !selected ? WarmPalette.chip : "transparent")
                 : selected ? Theme.navSelected : navArea.containsMouse ? Theme.navHover : "transparent"
            activeFocusOnTab: true

            Keys.onSpacePressed: window.showPage(navItem.index)
            Keys.onReturnPressed: window.showPage(navItem.index)
            Keys.onEnterPressed: window.showPage(navItem.index)
            Keys.onUpPressed: select(Math.max(0, navItem.index - 1))
            Keys.onDownPressed: select(Math.min(window.pages.length - 1, navItem.index + 1))

            Rectangle { // the selected item on the warm scene
                visible: window.warm && navItem.selected
                anchors.fill: parent
                radius: 6
                gradient: Gradient {
                    GradientStop { position: 0; color: WarmPalette.accentLight }
                    GradientStop { position: 1; color: WarmPalette.accent }
                }
            }

            Glyph {
                visible: !navItem.gleaning
                x: 16
                anchors.verticalCenter: parent.verticalCenter
                glyph: navItem.modelData.glyph ?? ""
                size: 16
                color: navItem.foreground
            }
            Loader { // 拾穗计划: the author's mark, which plays when the page opens
                active: navItem.gleaning
                x: 13
                width: 22
                height: 22
                anchors.verticalCenter: parent.verticalCenter
                sourceComponent: Item {
                    AuthorMark {
                        anchors.centerIn: parent
                        width: parent.width * (48 + 2 * margin) / 48
                        height: width
                        time: AuthorClock.elapsed
                        startInk: WarmPalette.ink
                    }
                }
            }
            Text {
                x: 48
                width: parent.width - x - 12
                anchors.verticalCenter: parent.verticalCenter
                text: navItem.modelData.title
                color: navItem.foreground
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }

            Rectangle { // keyboard focus
                visible: navItem.activeFocus
                anchors.fill: parent
                radius: 4
                color: "transparent"
                border.width: 2
                border.color: window.warm ? WarmPalette.ink : Theme.text
            }

            MouseArea {
                id: navArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    window.clearFocus()
                    window.showPage(navItem.index)
                }
            }
        }

        Column {
            id: navList

            readonly property int itemHeight: 40

            x: 12
            y: 8
            width: parent.width - 24
            spacing: 4

            Repeater {
                id: navItems
                model: window.pages.slice(0, window.gleaningPage)
                delegate: NavItem {}
            }

            Item { // sets 拾穗计划 apart from the settings
                width: 1
                height: 12
            }

            NavItem {
                id: gleaningNav
                index: window.gleaningPage
                modelData: window.pages[window.gleaningPage]
            }
        }

        Rectangle { // marks the current category, sliding between items as in Windows 11
            visible: !window.warm // there the item itself is filled
            x: navList.x
            y: navList.y + (window.currentPage === window.gleaningPage ? gleaningNav.y
                            : window.currentPage * (navList.itemHeight + navList.spacing))
               + (navList.itemHeight - height) / 2
            width: 3
            height: 16
            radius: 1.5
            color: Theme.accent

            Behavior on y { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        }

        Rectangle { // the version; with a newer one, says so. A click goes to the updates
            id: versionLink
            x: 16
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14
            width: versionRow.implicitWidth + 16
            height: versionRow.implicitHeight + 8
            radius: 4
            color: versionArea.containsMouse ? (window.warm ? WarmPalette.chip : Theme.navHover) : "transparent"

            Row {
                id: versionRow
                anchors.centerIn: parent
                spacing: 5

                Text {
                    text: qsTr("Win顺 %1").arg(window.updater.currentVersion)
                    color: window.warm ? WarmPalette.inkSoft : Theme.faint
                    font.pixelSize: Theme.fontCaption
                }
                Text {
                    visible: window.updater.available
                    text: qsTr("· 有新版本 %1").arg(window.updater.availableVersion)
                    color: window.warm ? WarmPalette.ink : Theme.accent
                    font.pixelSize: Theme.fontCaption
                }
            }
            MouseArea {
                id: versionArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    window.clearFocus()
                    window.showPage(window.advancedPage)
                }
            }
        }

        Text {
            visible: !window.warm
            x: 24
            width: parent.width - 48
            anchors.bottom: versionLink.top
            anchors.bottomMargin: 8
            text: qsTr("修改会自动保存，立即生效")
            color: Theme.faint
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }

    GleaningPage {
        visible: window.warm
        anchors.left: nav.right
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        editor: window.editor
    }

    Flickable {
        id: flick
        visible: !window.warm
        anchors.left: nav.right
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: page.implicitHeight + 48
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        MouseArea { // clicking empty space finishes editing a field
            width: flick.width
            height: Math.max(flick.height, flick.contentHeight)
            onClicked: window.clearFocus()
        }

        Column { // the current category's options; the other sections are hidden
            id: page
            x: 8
            y: 8
            width: flick.width - 36
            spacing: 20
            // Drawn on whole device pixels wherever it has scrolled to (see
            // Main.qml), plus a fiftieth of one: much of the text inside sits
            // on half pixels (odd logical pixels at 150 %), where the text
            // shader's rounding would go either way with float error from one
            // scroll position to the next. A hair past the half, it always
            // goes the same way.
            transform: Translate {
                y: {
                    const dpr = Screen.devicePixelRatio
                    const deviceY = (flick.y + flick.contentItem.y + page.y) * dpr
                    return (Math.round(deviceY) - deviceY + 0.02) / dpr
                }
            }

            Text {
                text: window.pages[window.currentPage].title
                color: Theme.text
                font.pixelSize: Theme.fontDisplay
                font.weight: Font.DemiBold
            }

            SettingsSection {
                visible: window.currentPage === 0
                width: parent.width

                SettingRow {
                    title: qsTr("双击 Ctrl 打开")
                    description: qsTr("快速连按两下 Ctrl 键，打开或关闭搜索框")

                    ToggleSwitch {
                        checked: window.editor.doubleCtrl
                        onToggled: (on) => window.editor.doubleCtrl = on
                    }
                }

                SettingRow {
                    visible: window.editor.doubleCtrl
                    title: qsTr("玩游戏时不响应双击 Ctrl")
                    description: qsTr("游戏里常连按两下 Ctrl 蹲下。前台程序独占全屏，或者藏起鼠标用来转视角时，双击 Ctrl 不打开搜索框。设置的快捷键照常可用")

                    ToggleSwitch {
                        checked: window.editor.doubleCtrlPauseInGames
                        onToggled: (on) => window.editor.doubleCtrlPauseInGames = on
                    }
                }

                SettingRow {
                    visible: window.editor.doubleCtrl
                    title: qsTr("任何程序全屏时都不响应双击 Ctrl")
                    description: qsTr("看视频、放幻灯片、全屏浏览网页时也不打开搜索框")

                    ToggleSwitch {
                        checked: window.editor.doubleCtrlPauseInFullScreen
                        onToggled: (on) => window.editor.doubleCtrlPauseInFullScreen = on
                    }
                }

                SettingRow {
                    visible: window.editor.doubleCtrl
                    title: qsTr("在这些程序里不响应双击 Ctrl")
                    description: qsTr("填程序的文件名，例如 TheFinals.exe。没被自动认出来的游戏可以加在这里")

                    body: [
                        Flow {
                            width: parent.width
                            spacing: 6

                            Repeater {
                                model: window.editor.doubleCtrlExcludedApps

                                delegate: Chip {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    onRemoveClicked: window.editor.removeDoubleCtrlExcludedApp(index)
                                }
                            }
                        },
                        Row {
                            spacing: 8

                            InputBox {
                                id: doubleCtrlAppInput
                                width: 300
                                placeholder: qsTr("输入程序文件名，例如 TheFinals.exe")
                                onAccepted: if (window.editor.addDoubleCtrlExcludedApp(text)) clear()
                            }
                            FlatButton {
                                text: qsTr("添加")
                                enabled: doubleCtrlAppInput.text.trim().length > 0
                                onClicked: if (window.editor.addDoubleCtrlExcludedApp(doubleCtrlAppInput.text))
                                               doubleCtrlAppInput.clear()
                            }
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("快捷键")
                    description: qsTr("再设一个组合键来打开搜索框，例如 Alt + Space。点击右边的方框，然后按下想用的按键")

                    FlatButton {
                        visible: window.editor.hotkey.length > 0 && !hotkeyRecorder.recording
                        flat: true
                        text: qsTr("清除")
                        onClicked: window.editor.hotkey = ""
                    }
                    HotkeyRecorder {
                        id: hotkeyRecorder
                        hotkey: window.editor.hotkey
                        onRecorded: (hotkey) => window.editor.hotkey = hotkey
                    }

                    body: Text {
                        visible: window.editor.hotkeyError.length > 0
                        width: parent.width
                        text: window.editor.hotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    title: qsTr("Ctrl+G 转到资源管理器的文件夹")
                    description: qsTr("在“打开”“另存为”等对话框里按 Ctrl+G，对话框直接转到最近用过的资源管理器窗口正在显示的文件夹")

                    ToggleSwitch {
                        checked: window.editor.dialogJump
                        onToggled: (on) => window.editor.dialogJump = on
                    }
                }

                SettingRow {
                    title: qsTr("对话框旁的搜索框")
                    description: qsTr("“打开”“另存为”等对话框出现时，在它旁边放一个搜索框：搜文件夹或文件，选中后对话框直接转过去。在对话框里双击 Ctrl 就能开始输入")

                    ToggleSwitch {
                        checked: window.editor.dialogBar
                        onToggled: (on) => window.editor.dialogBar = on
                    }
                }

                SettingRow {
                    visible: window.editor.dialogBar
                    title: qsTr("搜索框的位置")
                    description: qsTr("“自动”时放在对话框下方，下方放不下就放在右边或左边，都放不下就把对话框调矮一点。选定一边时，那边放不下就把对话框调小一点或挪开")

                    ScopeTabs {
                        labels: [qsTr("自动"), qsTr("下方"), qsTr("左边"), qsTr("右边")]
                        current: Math.max(0, window.dialogBarPlaces.indexOf(window.editor.dialogBarPlace))
                        onActivated: (index) => window.editor.dialogBarPlace = window.dialogBarPlaces[index]
                    }
                }

                SettingRow {
                    title: qsTr("对话框自动转到资源管理器的文件夹")
                    description: qsTr("“打开”“另存为”等对话框出现时，自动转到资源管理器正在显示的文件夹；对话框开着时去资源管理器换了文件夹，切回来也跟着转过去，只是看一眼就不动。转过去以后，搜索框上有按钮回到原来的位置")

                    ToggleSwitch {
                        checked: window.editor.dialogAutoJump
                        onToggled: (on) => window.editor.dialogAutoJump = on
                    }
                }

                SettingRow {
                    visible: window.editor.dialogBarExcludedApps.length > 0
                    title: qsTr("不在这些程序的对话框下显示搜索框")
                    description: qsTr("点搜索框最右边的“更多”按钮添加。这些程序里 Ctrl+G 照常可用")

                    body: Flow {
                        width: parent.width
                        spacing: 6

                        Repeater {
                            model: window.editor.dialogBarExcludedApps

                            delegate: Chip {
                                required property int index
                                required property string modelData
                                text: modelData
                                onRemoveClicked: window.editor.removeDialogBarExcludedApp(index)
                            }
                        }
                    }
                }

                SettingRow {
                    title: qsTr("开机时自动启动")
                    description: qsTr("登录 Windows 后在后台运行，随时可以打开")

                    ToggleSwitch {
                        checked: window.editor.autostart
                        onToggled: (on) => window.editor.autostart = on
                    }
                }

                SettingRow {
                    title: qsTr("记住打开过的项目")
                    description: qsTr("什么都不输入时列出最近打开的文件和应用，搜索时它们排在前面。关闭后不再记录，也不再显示已有的记录")

                    ToggleSwitch {
                        checked: window.editor.recordHistory
                        onToggled: (on) => window.editor.recordHistory = on
                    }
                }

                SettingRow {
                    title: qsTr("清除最近使用记录")
                    description: window.editor.historyCount > 0
                                 ? qsTr("共 %1 项。也可以在搜索框里右键某一项，单独移除").arg(window.editor.historyCount)
                                 : qsTr("没有记录")

                    FlatButton {
                        id: clearHistoryButton

                        property bool confirming: false

                        text: confirming ? qsTr("确定清除？再点一次") : qsTr("清除")
                        glyph: confirming ? "" : "\uE74D" // Delete
                        enabled: window.editor.historyCount > 0
                        onClicked: {
                            if (confirming) {
                                window.editor.clearHistory()
                                confirming = false
                            } else {
                                confirming = true
                                clearHistoryTimer.restart()
                            }
                        }

                        Timer {
                            id: clearHistoryTimer
                            interval: 4000
                            onTriggered: clearHistoryButton.confirming = false
                        }
                    }
                }
            }

            SettingsSection {
                visible: window.currentPage === 1
                width: parent.width

                SettingRow {
                    title: qsTr("主题")
                    description: qsTr("“跟随系统”时随 Windows 的浅色、深色模式切换")

                    body: Row {
                        id: themeCards

                        readonly property var modes: ["system", "light", "dark"]
                        readonly property var labels: [qsTr("跟随系统"), qsTr("浅色"), qsTr("深色")]

                        width: parent.width
                        spacing: 14

                        Repeater {
                            id: themeRepeater
                            model: themeCards.modes

                            delegate: ThemeCard {
                                required property int index
                                required property string modelData

                                width: Math.min(196, (themeCards.width - themeCards.spacing * 2) / 3)
                                mode: modelData
                                label: themeCards.labels[index]
                                selected: window.editor.theme === modelData
                                onActivated: if (!selected) window.changeAppearance(() => window.editor.theme = modelData)
                                Keys.onLeftPressed: themeRepeater.itemAt(Math.max(0, index - 1)).forceActiveFocus()
                                Keys.onRightPressed: themeRepeater.itemAt(Math.min(2, index + 1)).forceActiveFocus()
                            }
                        }
                    }
                }

                SettingRow {
                    visible: SystemTheme.backdropSystem // not on Windows 10
                    title: qsTr("透明效果")
                    description: !SystemTheme.backdropAvailable
                                 ? qsTr("需要把“高级”里的界面绘制方式设为“显卡加速”")
                                 : !SystemTheme.materials
                                 ? qsTr("窗口背景透出桌面壁纸的颜色（云母效果）。Windows 设置里的“透明效果”关着，打开后才能看到")
                                 : qsTr("窗口背景透出桌面壁纸的颜色（云母效果）")

                    ScopeTabs {
                        enabled: SystemTheme.backdropAvailable
                        opacity: enabled ? 1 : 0.4
                        labels: [qsTr("关"), qsTr("开")]
                        current: Math.max(0, window.transparencies.indexOf(window.editor.transparency))
                        onActivated: (index) => window.editor.transparency = window.transparencies[index]
                    }
                }

                SettingRow {
                    title: qsTr("语言")
                    description: qsTr("“跟随系统”时，中文版 Windows 显示中文，其他语言的 Windows 显示英文")

                    ScopeTabs { // each language in its own words
                        labels: [qsTr("跟随系统"), "简体中文", "English"]
                        current: Math.max(0, window.languages.indexOf(window.editor.language))
                        onActivated: (index) => {
                            if (index !== current)
                                window.changeAppearance(() => window.editor.language = window.languages[index])
                        }
                    }
                }
            }

            SettingsSection {
                visible: window.currentPage === 2
                width: parent.width
                note: qsTr("修改后会在后台重新整理文件列表，期间可以照常搜索。")

                SettingRow {
                    title: qsTr("不搜索的文件夹")
                    description: qsTr("这些文件夹以及里面的所有内容都不会出现在搜索结果里")

                    FlatButton {
                        text: qsTr("添加文件夹…")
                        glyph: "" // Add
                        onClicked: window.editor.addExcludedFolder()
                    }

                    body: Rectangle {
                        width: parent.width
                        height: Math.max(pathList.implicitHeight, 40)
                        radius: 4
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.cardBorder

                        Text {
                            visible: window.editor.excludedPaths.length === 0
                            anchors.centerIn: parent
                            text: qsTr("没有排除任何文件夹")
                            color: Theme.faint
                            font.pixelSize: Theme.fontCaption
                        }

                        Column {
                            id: pathList
                            width: parent.width

                            Repeater {
                                model: window.editor.excludedPaths

                                delegate: Item {
                                    id: pathRow

                                    required property int index
                                    required property string modelData

                                    width: pathList.width
                                    height: 42

                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        radius: 3
                                        color: rowArea.containsMouse ? Theme.navHover : "transparent"
                                    }
                                    Rectangle {
                                        visible: pathRow.index > 0
                                        x: 12
                                        width: parent.width - 24
                                        height: 1
                                        color: Theme.divider
                                    }
                                    MouseArea {
                                        id: rowArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        acceptedButtons: Qt.NoButton
                                    }
                                    Glyph {
                                        id: folderIcon
                                        x: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        glyph: "" // Folder
                                        size: 16
                                    }
                                    Text {
                                        anchors.left: folderIcon.right
                                        anchors.leftMargin: 10
                                        anchors.right: removePath.left
                                        anchors.rightMargin: 8
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: pathRow.modelData
                                        textFormat: Text.PlainText
                                        color: Theme.text
                                        font.pixelSize: Theme.fontBody
                                        elide: Text.ElideMiddle
                                    }
                                    FlatButton {
                                        id: removePath
                                        anchors.right: parent.right
                                        anchors.rightMargin: 6
                                        anchors.verticalCenter: parent.verticalCenter
                                        implicitHeight: 32
                                        flat: true
                                        text: qsTr("移除")
                                        onClicked: window.editor.removeExcludedPath(pathRow.index)
                                    }
                                }
                            }
                        }
                    }
                }

                SettingRow {
                    title: qsTr("跳过的文件夹名称")
                    description: qsTr("在任何位置遇到这些名字的文件夹都会跳过，适合 node_modules 这类到处都有的文件夹")

                    body: [
                        Flow {
                            width: parent.width
                            spacing: 6

                            Repeater {
                                model: window.editor.excludedNames

                                delegate: Chip {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    onRemoveClicked: window.editor.removeExcludedName(index)
                                }
                            }
                        },
                        Row {
                            spacing: 8

                            InputBox {
                                id: nameInput
                                width: 240
                                placeholder: qsTr("输入文件夹名称，例如 build")
                                onAccepted: if (window.editor.addExcludedName(text)) clear()
                            }
                            FlatButton {
                                text: qsTr("添加")
                                enabled: nameInput.text.trim().length > 0
                                onClicked: if (window.editor.addExcludedName(nameInput.text)) nameInput.clear()
                            }
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("包括 U 盘和移动硬盘")
                    description: qsTr("默认只搜索电脑自带的硬盘")

                    ToggleSwitch {
                        checked: window.editor.includeRemovableDrives
                        onToggled: (on) => window.editor.includeRemovableDrives = on
                    }
                }

                SettingRow {
                    title: qsTr("启动时检查文件变化")
                    description: qsTr("U 盘等非 NTFS 磁盘在 Win顺没有运行期间的改动，启动后于后台补上（NTFS 磁盘总会自动补上）")

                    ToggleSwitch {
                        checked: window.editor.rescanOnStartup
                        onToggled: (on) => window.editor.rescanOnStartup = on
                    }
                }
            }

            SettingsSection {
                visible: window.currentPage === 3
                width: parent.width
                note: qsTr("在搜索框按 Tab 切换到“内容”，可以查找文件里的文字。")

                SettingRow {
                    title: qsTr("搜索文档")
                    description: qsTr("Word、Excel、PowerPoint、PDF 和 WPS 文件，新旧格式都可以。由一个权限受限的单独进程读取；扫描件和图片里的文字读不到")

                    ToggleSwitch {
                        checked: window.editor.contentDocuments
                        onToggled: (on) => window.editor.contentDocuments = on
                    }
                }

                SettingRow {
                    title: qsTr("搜索这些类型的文本文件")
                    description: qsTr("纯文本类型的文件，按扩展名列出。Word、Excel、PDF 等文档由上面的“搜索文档”负责")

                    body: [
                        Flow {
                            width: parent.width
                            spacing: 6

                            Repeater {
                                model: window.editor.contentExtensions

                                delegate: Chip {
                                    required property int index
                                    required property string modelData
                                    text: "." + modelData
                                    onRemoveClicked: window.editor.removeContentExtension(index)
                                }
                            }
                        },
                        Text {
                            visible: window.editor.contentExtensions.length === 0
                            text: qsTr("还没有选择任何类型，内容搜索将找不到结果")
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                        },
                        Row {
                            spacing: 8

                            InputBox {
                                id: extensionInput
                                width: 240
                                placeholder: qsTr("例如 md, log")
                                onAccepted: if (window.editor.addContentExtensions(text)) clear()
                            }
                            FlatButton {
                                text: qsTr("添加")
                                enabled: extensionInput.text.trim().length > 0
                                onClicked: if (window.editor.addContentExtensions(extensionInput.text)) extensionInput.clear()
                            }
                        }
                    ]
                }

                // A size limit for each kind of file (ContentSizeLimits::Kind:
                // text, code, data, documents).
                Repeater {
                    model: [
                        { title: qsTr("文本和日志的大小上限"),
                          note: qsTr("更大的文件不查找内容") },
                        { title: qsTr("源代码的大小上限"),
                          note: qsTr("手写的代码很少有几 MB，更大的多是打包压缩后的脚本，搜什么都可能要整个读一遍") },
                        { title: qsTr("数据和网页的大小上限"),
                          note: qsTr("几十 MB 的多是导出的数据，搜什么都可能要整个读一遍，所以默认小一些") },
                        { title: qsTr("文档的大小上限"),
                          files: qsTr("Word、Excel、PowerPoint、PDF 和 WPS 文件"),
                          note: qsTr("文档常有几十上百 MB，大多是图片，读出的文字不多，所以默认大得多；每个最多读出 16 MB 文字") }
                    ]

                    delegate: SettingRow {
                        id: sizeRow

                        required property int index
                        required property var modelData
                        readonly property int mb: window.editor.contentMaxSizeMB[index]
                        readonly property string extensions: window.editor.contentKindExtensions[index]

                        visible: modelData.files === undefined || window.editor.contentDocuments
                        title: modelData.title
                        description: (modelData.files ?? (extensions.length > 0 ? extensions : qsTr("列表里没有这类文件")))
                                     + "\n" + modelData.note

                        FlatButton {
                            glyph: "" // Remove (minus)
                            enabled: sizeRow.mb > 1
                            onClicked: window.editor.setContentMaxSizeMB(sizeRow.index, sizeRow.mb > 16 ? sizeRow.mb - 16 : sizeRow.mb - 1)
                        }
                        InputBox {
                            width: 80
                            horizontalAlignment: TextInput.AlignHCenter
                            text: sizeRow.mb
                            validator: IntValidator { bottom: 1; top: 4096 }
                            onEditingFinished: {
                                const mb = parseInt(text)
                                if (!isNaN(mb))
                                    window.editor.setContentMaxSizeMB(sizeRow.index, mb)
                                text = Qt.binding(() => sizeRow.mb)
                            }
                        }
                        FlatButton {
                            glyph: "" // Add
                            enabled: sizeRow.mb < 4096
                            onClicked: window.editor.setContentMaxSizeMB(sizeRow.index, sizeRow.mb >= 16 ? sizeRow.mb + 16 : sizeRow.mb + 1)
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "MB"
                            color: Theme.subtext
                            font.pixelSize: Theme.fontBody
                        }
                    }
                }

                SettingRow {
                    title: qsTr("也搜索系统和程序文件夹")
                    description: qsTr("包括 Windows、Program Files、AppData、node_modules 等文件夹。这些地方文件很多，打开后内容搜索会慢不少")

                    ToggleSwitch {
                        checked: window.editor.contentInLowPriority
                        onToggled: (on) => window.editor.contentInLowPriority = on
                    }
                }

                SettingRow {
                    title: qsTr("建立内容索引")
                    description: qsTr("在后台记下每个文件里有哪些中日韩文字，和三个字符一段的英文、数字、空格和标点，搜索时只打开可能含有它的文件，快得多；搜索中读过的文件也会记下。文档读出的文字也存在这里，搜索时不用再读一遍文档。只用于 NTFS 磁盘，首次建立需要一段时间，几十万个文件约占一两百 MB 磁盘空间")
                                 + (window.editor.contentIndexStatus.length > 0 ? "\n" + window.editor.contentIndexStatus : "")

                    ToggleSwitch {
                        checked: window.editor.contentIndex
                        onToggled: (on) => window.editor.contentIndex = on
                    }
                }
            }

            SettingsSection {
                id: webSection

                // The shortcut being changed in its row; -1: a new one, at the end of the list; -2: none.
                property int editing: -2

                visible: window.currentPage === window.webPage
                width: parent.width
                note: qsTr("给常去的网页起个关键词：在搜索框的“全部”里输入它，按 Enter 就打开。网址里带 %s 的还能搜索：关键词后面加空格和要搜的文字，按 Enter 就在那个网站上搜。例如关键词 gh、网址 https://github.com/search?q=%s，输入 gh WinShun 就在 GitHub 上搜索 WinShun。")

                onVisibleChanged: if (!visible) editing = -2

                SettingRow {
                    title: qsTr("关键词")
                    description: qsTr("不输关键词，输入名称也能找到它；中文名称也可以打拼音")

                    FlatButton {
                        text: qsTr("添加")
                        glyph: "\uE710" // Add
                        enabled: webSection.editing !== -1
                        onClicked: webSection.editing = -1
                    }

                    body: Rectangle {
                        width: parent.width
                        height: Math.max(webList.implicitHeight, 40)
                        radius: 4
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.cardBorder

                        Text {
                            visible: window.editor.webShortcuts.length === 0 && webSection.editing !== -1
                            anchors.centerIn: parent
                            text: qsTr("还没有网页搜索，点“添加”加一个")
                            color: Theme.faint
                            font.pixelSize: Theme.fontCaption
                        }

                        Column {
                            id: webList
                            width: parent.width

                            Repeater {
                                model: window.editor.webShortcuts

                                delegate: Item {
                                    id: webRow

                                    required property int index
                                    required property var modelData
                                    readonly property bool editing: webSection.editing === index

                                    width: webList.width
                                    height: editing ? webForm.implicitHeight : 58

                                    Rectangle {
                                        visible: !webRow.editing
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        radius: 3
                                        color: webArea.containsMouse ? Theme.navHover : "transparent"
                                    }
                                    Rectangle {
                                        visible: webRow.index > 0
                                        x: 12
                                        width: parent.width - 24
                                        height: 1
                                        color: Theme.divider
                                    }
                                    MouseArea {
                                        id: webArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        acceptedButtons: Qt.NoButton
                                    }

                                    Rectangle { // the keyword, as keys to press
                                        id: keycap
                                        visible: !webRow.editing
                                        x: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.max(44, keywordText.implicitWidth + 16)
                                        height: 26
                                        radius: 4
                                        color: Theme.keycap
                                        border.width: 1
                                        border.color: Theme.keycapBorder

                                        Text {
                                            id: keywordText
                                            anchors.centerIn: parent
                                            text: webRow.modelData.keyword
                                            textFormat: Text.PlainText
                                            color: Theme.text
                                            font.pixelSize: Theme.fontCaption
                                        }
                                    }
                                    Column {
                                        visible: !webRow.editing
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
                                                  : qsTr("%1 · 只能打开，不能搜索").arg(webRow.modelData.shownUrl)
                                            textFormat: Text.PlainText
                                            color: Theme.subtext
                                            font.pixelSize: Theme.fontCaption
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                    Row {
                                        id: webButtons
                                        visible: !webRow.editing
                                        anchors.right: parent.right
                                        anchors.rightMargin: 6
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 2

                                        FlatButton {
                                            implicitHeight: 32
                                            flat: true
                                            text: qsTr("打开")
                                            onClicked: window.editor.openWebShortcut(webRow.index)
                                        }
                                        FlatButton {
                                            implicitHeight: 32
                                            flat: true
                                            text: qsTr("编辑")
                                            onClicked: webSection.editing = webRow.index
                                        }
                                        FlatButton {
                                            implicitHeight: 32
                                            flat: true
                                            text: qsTr("移除")
                                            onClicked: {
                                                const removed = webRow.index
                                                if (webSection.editing === removed)
                                                    webSection.editing = -2
                                                else if (webSection.editing > removed)
                                                    webSection.editing -= 1 // the one being changed moves up
                                                window.editor.removeWebShortcut(removed)
                                            }
                                        }
                                    }

                                    Loader {
                                        id: webForm
                                        active: webRow.editing
                                        width: parent.width
                                        sourceComponent: WebShortcutForm {
                                            editor: window.editor
                                            index: webRow.index
                                            shortcut: webRow.modelData
                                            onFinished: webSection.editing = -2
                                        }
                                    }
                                }
                            }

                            Loader { // adding one
                                active: webSection.editing === -1
                                width: parent.width
                                sourceComponent: WebShortcutForm {
                                    editor: window.editor
                                    onFinished: webSection.editing = -2
                                }
                            }
                        }
                    }
                }
            }

            SettingsSection {
                visible: window.currentPage === window.clipboardPage
                width: parent.width
                note: qsTr("复制过的文字、图片和文件记在这台电脑上，不会上传。在剪贴板里点一条或按 Enter，就粘贴到打开之前所在的窗口，Shift+Enter 粘贴为纯文本；按住 Ctrl 或 Shift 点击可以选多条，按选的顺序合在一起粘贴。")

                SettingRow {
                    title: qsTr("记录剪贴板历史")
                    description: qsTr("密码管理器复制的密码不会被记录")

                    ToggleSwitch {
                        checked: window.editor.clipboard
                        onToggled: (on) => window.editor.clipboard = on
                    }
                }

                SettingRow {
                    title: qsTr("用 Win+V 打开，代替 Windows 自带的剪贴板")
                    description: qsTr("打开后按 Win+V 出现的是 Win顺的剪贴板；Windows 面板里的表情可以改用 Win+. 打开。关掉这项，Win+V 就回到 Windows 自带的")

                    ToggleSwitch {
                        checked: window.editor.clipboardWinV
                        onToggled: (on) => window.editor.clipboardWinV = on
                    }

                    body: Column {
                        readonly property string winV: window.editor.winVState
                        readonly property bool restartable: (winV === "waiting" || winV === "releasing")
                                                            && window.editor.canRestartExplorer

                        visible: statusText.text.length > 0
                        width: parent.width
                        spacing: 10

                        Text {
                            id: statusText
                            width: parent.width
                            text: {
                                switch (window.editor.winVState) {
                                case "on": return qsTr("已生效：Win+V 打开 Win顺的剪贴板")
                                case "waiting": return qsTr("资源管理器重启后生效，下次登录 Windows 时也会自动生效")
                                case "releasing": return qsTr("资源管理器重启后，Win+V 回到 Windows 自带的剪贴板")
                                case "failed": return qsTr("没能修改 Windows 的设置，Win+V 仍是 Windows 自带的剪贴板")
                                default: return ""
                                }
                            }
                            color: window.editor.winVState === "failed" ? Theme.danger
                                 : window.editor.winVState === "on" ? Theme.subtext : Theme.text
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                        Row {
                            visible: parent.restartable
                            spacing: 12

                            FlatButton {
                                text: qsTr("现在重启资源管理器")
                                glyph: "" // Sync
                                onClicked: window.editor.restartExplorer()
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("任务栏会闪一下，打开的文件夹窗口会关闭")
                                color: Theme.faint
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                }

                SettingRow {
                    title: qsTr("另设快捷键")
                    description: qsTr("不想换掉 Win+V 时，可以另设一个组合键打开剪贴板，例如 Win + Alt + V")

                    FlatButton {
                        visible: window.editor.clipboardHotkey.length > 0 && !clipHotkeyRecorder.recording
                        flat: true
                        text: qsTr("清除")
                        onClicked: window.editor.clipboardHotkey = ""
                    }
                    HotkeyRecorder {
                        id: clipHotkeyRecorder
                        hotkey: window.editor.clipboardHotkey
                        onRecorded: (hotkey) => window.editor.clipboardHotkey = hotkey
                    }

                    body: Text {
                        visible: window.editor.clipboardHotkeyError.length > 0
                        width: parent.width
                        text: window.editor.clipboardHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    title: qsTr("记录图片")
                    description: qsTr("截图和复制的图片也记下来，每张图片占一些磁盘空间")

                    ToggleSwitch {
                        checked: window.editor.clipboardImages
                        onToggled: (on) => window.editor.clipboardImages = on
                    }
                }

                SettingRow {
                    title: qsTr("最多保留")
                    description: qsTr("放进“固定”和其他分组的不算在内，一直保留")

                    ScopeTabs {
                        labels: [qsTr("%1 条").arg(50), qsTr("%1 条").arg(100), qsTr("%1 条").arg(200),
                                 qsTr("%1 条").arg(300)]
                        current: [50, 100, 200, 300].indexOf(window.editor.clipboardMaxItems)
                        onActivated: (index) => window.editor.clipboardMaxItems = [50, 100, 200, 300][index]
                    }
                }

                SettingRow {
                    title: qsTr("保留时间")
                    description: qsTr("这么久没再复制或粘贴过的记录会自动删除；分组里的不会")

                    ScopeTabs {
                        labels: [qsTr("%n 天", "", 7), qsTr("%n 天", "", 30), qsTr("%n 天", "", 90), qsTr("一直保留")]
                        current: [7, 30, 90, 0].indexOf(window.editor.clipboardMaxDays)
                        onActivated: (index) => window.editor.clipboardMaxDays = [7, 30, 90, 0][index]
                    }
                }

                SettingRow {
                    title: qsTr("不记录这些程序复制的内容")
                    description: qsTr("填程序的文件名，例如 KeePass.exe。密码管理器一般会自己声明“不要记录”，这里再多一层保险")

                    body: [
                        Flow {
                            width: parent.width
                            spacing: 6

                            Repeater {
                                model: window.editor.clipboardExcludedApps

                                delegate: Chip {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    onRemoveClicked: window.editor.removeClipboardExcludedApp(index)
                                }
                            }
                        },
                        Row {
                            spacing: 8

                            InputBox {
                                id: appInput
                                width: 240
                                placeholder: qsTr("输入程序文件名，例如 KeePass.exe")
                                onAccepted: if (window.editor.addClipboardExcludedApp(text)) clear()
                            }
                            FlatButton {
                                text: qsTr("添加")
                                enabled: appInput.text.trim().length > 0
                                onClicked: if (window.editor.addClipboardExcludedApp(appInput.text)) appInput.clear()
                            }
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("清除剪贴板历史")
                    description: window.editor.clipboardCount > 0
                                 ? qsTr("共 %1 条。“固定”和其他分组里的会保留").arg(window.editor.clipboardCount)
                                 : qsTr("没有记录")

                    FlatButton {
                        id: clearClipboardButton

                        property bool confirming: false

                        text: confirming ? qsTr("确定清除？再点一次") : qsTr("清除")
                        glyph: confirming ? "" : "" // Delete
                        enabled: window.editor.clipboardCount > 0
                        onClicked: {
                            if (confirming) {
                                window.editor.clearClipboard()
                                confirming = false
                            } else {
                                confirming = true
                                clearClipboardTimer.restart()
                            }
                        }

                        Timer {
                            id: clearClipboardTimer
                            interval: 4000
                            onTriggered: clearClipboardButton.confirming = false
                        }
                    }
                }
            }

            SettingsSection {
                visible: window.currentPage === window.advancedPage
                width: parent.width
                title: qsTr("更新")
                note: qsTr("新版本发布在 GitHub 上。检查更新时只访问 GitHub，不发送任何个人信息；有新版本时，点“去下载”会在浏览器里打开下载页。")

                SettingRow {
                    title: qsTr("版本 %1").arg(window.updater.currentVersion)
                    description: window.updater.checking ? qsTr("正在检查更新…")
                               : window.updater.available ? qsTr("有新版本 %1").arg(window.updater.availableVersion)
                               : window.updater.problem.length > 0 ? window.updater.problem
                               : !isNaN(window.updater.lastChecked.getTime())
                                 ? qsTr("已是最新版本 · %1检查").arg(window.sinceText(window.updater.lastChecked))
                               : ""

                    FlatButton {
                        visible: !window.updater.available
                        enabled: !window.updater.checking
                        text: qsTr("检查更新")
                        glyph: "\uE895" // Sync
                        onClicked: window.updater.check(true)
                    }
                    FlatButton {
                        visible: window.updater.available
                        highlighted: true
                        text: qsTr("查看 %1").arg(window.updater.availableVersion)
                        onClicked: window.showUpdateDialog()
                    }
                }

                SettingRow {
                    title: qsTr("自动检查更新")
                    description: qsTr("每次启动时看一次，之后每隔 12 小时看一次。有新版本时在托盘弹出提示")

                    ToggleSwitch {
                        checked: window.editor.autoUpdate
                        onToggled: (on) => window.editor.autoUpdate = on
                    }
                }
            }

            SettingsSection {
                visible: window.currentPage === window.advancedPage
                width: parent.width

                SettingRow {
                    title: qsTr("界面绘制方式")
                    description: window.editor.restartRequired
                                 ? qsTr("重启 Win顺后生效")
                                 : qsTr("“显卡加速”文字最清晰；“省内存”少占约 50 MB 内存，但文字偏模糊")

                    FlatButton { // sized like one of the tabs next to it
                        anchors.verticalCenter: parent.verticalCenter
                        implicitHeight: 32
                        radius: 6
                        visible: window.editor.restartRequired
                        text: qsTr("立即重启")
                        highlighted: true
                        onClicked: window.editor.restart()
                    }
                    ScopeTabs {
                        labels: [qsTr("省内存"), qsTr("显卡加速")]
                        current: Math.max(0, window.renderers.indexOf(window.editor.renderer))
                        onActivated: (index) => window.editor.renderer = window.renderers[index]
                    }
                }

                SettingRow {
                    id: indexFolderRow

                    readonly property bool moving: window.editor.indexMoveProgress >= 0

                    title: qsTr("索引位置")
                    description: (window.editor.indexSize.length > 0
                                  ? qsTr("文件索引和内容索引放在这里，共 %1。C 盘空间紧张时，可以移到其他内置硬盘上（U 盘、移动硬盘不行）").arg(window.editor.indexSize)
                                  : qsTr("文件索引和内容索引放在这里。C 盘空间紧张时，可以移到其他内置硬盘上（U 盘、移动硬盘不行）"))
                                 + "\n" + window.editor.indexFolder
                    bodyShown: moving || window.editor.indexFolderProblem.length > 0 || window.editor.indexFolderNote.length > 0

                    FlatButton {
                        text: qsTr("打开")
                        glyph: "" // FolderOpen
                        enabled: !indexFolderRow.moving
                        onClicked: window.editor.openIndexFolder()
                    }
                    FlatButton {
                        text: qsTr("更改…")
                        glyph: "" // MoveToFolder
                        enabled: !indexFolderRow.moving
                        onClicked: window.editor.chooseIndexFolder()
                    }
                    FlatButton {
                        visible: !window.editor.indexFolderIsDefault
                        text: qsTr("恢复默认")
                        enabled: !indexFolderRow.moving
                        onClicked: window.editor.resetIndexFolder()
                    }

                    body: [
                        Column {
                            visible: indexFolderRow.moving
                            width: parent.width
                            spacing: 6

                            Text {
                                text: qsTr("正在移动… %1%，期间可以照常搜索").arg(Math.max(0, window.editor.indexMoveProgress))
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                            }
                            Rectangle { // how far
                                width: parent.width
                                height: 4
                                radius: 2
                                color: Theme.track

                                Rectangle {
                                    width: parent.width * Math.max(0, window.editor.indexMoveProgress) / 100
                                    height: parent.height
                                    radius: 2
                                    color: Theme.accent

                                    Behavior on width { NumberAnimation { duration: 150 } }
                                }
                            }
                        },
                        Text {
                            visible: !indexFolderRow.moving && window.editor.indexFolderProblem.length > 0
                            width: parent.width
                            text: window.editor.indexFolderProblem
                            textFormat: Text.PlainText
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        },
                        Text { // a hint, not a warning
                            visible: !indexFolderRow.moving && window.editor.indexFolderNote.length > 0
                            width: parent.width
                            text: window.editor.indexFolderNote
                            textFormat: Text.PlainText
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("数据文件夹")
                    description: qsTr("搜索记录、剪贴板历史和日志都存放在这里，索引默认也放在这里") + "\n" + window.editor.dataFolder

                    FlatButton {
                        text: qsTr("打开")
                        glyph: "" // FolderOpen
                        onClicked: window.editor.openDataFolder()
                    }
                }

                SettingRow {
                    title: qsTr("恢复默认设置")
                    description: qsTr("所有分类里的设置都会回到刚安装时的样子")

                    FlatButton {
                        id: resetButton

                        property bool confirming: false

                        text: confirming ? qsTr("确定要恢复吗？再点一次") : qsTr("恢复默认")
                        enabled: !window.editor.isDefault
                        onClicked: {
                            if (confirming) {
                                window.editor.restoreDefaults()
                                confirming = false
                            } else {
                                confirming = true
                                resetTimer.restart()
                            }
                        }

                        Timer {
                            id: resetTimer
                            interval: 4000
                            onTriggered: resetButton.confirming = false
                        }
                    }
                }
            }
        }
    }

    Rectangle { // scroll position
        visible: flick.visible && flick.contentHeight > flick.height
        x: parent.width - 7
        y: flick.visibleArea.yPosition * flick.height
        width: 3
        height: Math.max(24, flick.visibleArea.heightRatio * flick.height)
        radius: 1.5
        color: Theme.faint
        opacity: 0.6
    }

    UpdateDialog {
        id: updateDialog
        z: 1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom // the title bar keeps working
        anchors.bottom: parent.bottom
        updater: window.updater
        icon: window.frame.icon
    }

    Image { // the window as it looked before a theme or language change, fading out (see changeAppearance)
        id: snapshot
        z: 2
        anchors.fill: parent
        visible: fadeOut.running
        cache: false
        smooth: true

        NumberAnimation {
            id: fadeOut
            target: snapshot
            property: "opacity"
            from: 1
            to: 0
            duration: 260
            easing.type: Easing.InOutQuad
            onFinished: snapshot.source = "" // the picture's memory
        }
    }

    WindowEdge {}
}
