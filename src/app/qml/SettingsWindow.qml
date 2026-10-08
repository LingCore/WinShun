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

    readonly property var renderers: ["software", "d3d11", "auto"]
    readonly property var languages: ["system", "zh", "en"]
    readonly property var pages: [
        { title: qsTr("打开 Win顺"), glyph: "" }, // Keyboard
        { title: qsTr("外观"), glyph: "\uE771" }, // Personalize
        { title: qsTr("搜索范围"), glyph: "" }, // Folder
        { title: qsTr("文件内容搜索"), glyph: "" }, // Document
        { title: qsTr("高级"), glyph: "" }, // Settings
        { title: Gleaning.title, gleaning: true } // the author's works, set apart at the bottom of the list
    ]
    readonly property int gleaningPage: 5
    readonly property int advancedPage: 4 // where the updates are
    property int currentPage: 0
    // On the 拾穗计划 page the whole window, sidebar included, is a warm scene.
    readonly property bool warm: currentPage === gleaningPage

    width: 920
    height: 720
    minimumWidth: 760
    minimumHeight: 420
    color: Theme.page
    title: qsTr("设置") // shown as "设置 - Win顺"
    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)

    function clearFocus() { window.contentItem.forceActiveFocus() }

    function showPage(index) {
        currentPage = index
        flick.cancelFlick()
        flick.contentY = 0
    }

    function showUpdateDialog() { updateDialog.show() } // App::showUpdate

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

    Binding { // the global hotkey would fire instead of reaching the recorder
        target: window.editor
        property: "recordingHotkey"
        value: hotkeyRecorder.recording
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
                    title: qsTr("开机时自动启动")
                    description: qsTr("登录 Windows 后在后台运行，随时可以打开")

                    ToggleSwitch {
                        checked: window.editor.autostart
                        onToggled: (on) => window.editor.autostart = on
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
                                        color: rowArea.containsMouse ? Theme.hover : "transparent"
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
                    title: qsTr("搜索这些类型的文件")
                    description: qsTr("只会在这些类型的文件里查找文字。只支持纯文本文件，Word、Excel、PDF 加进来也搜不到")

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

                SettingRow {
                    title: qsTr("文件大小上限")
                    description: qsTr("大于这个大小的文件不查找内容，避免在超大文件上花太多时间")

                    FlatButton {
                        glyph: "" // Remove (minus)
                        enabled: window.editor.maxContentFileSizeMB > 1
                        onClicked: window.editor.maxContentFileSizeMB = window.editor.maxContentFileSizeMB > 16
                                   ? window.editor.maxContentFileSizeMB - 16 : window.editor.maxContentFileSizeMB - 1
                    }
                    InputBox {
                        width: 80
                        horizontalAlignment: TextInput.AlignHCenter
                        text: window.editor.maxContentFileSizeMB
                        validator: IntValidator { bottom: 1; top: 4096 }
                        onEditingFinished: {
                            const mb = parseInt(text)
                            window.editor.maxContentFileSizeMB = isNaN(mb) ? window.editor.maxContentFileSizeMB : mb
                            text = Qt.binding(() => window.editor.maxContentFileSizeMB)
                        }
                    }
                    FlatButton {
                        glyph: "" // Add
                        enabled: window.editor.maxContentFileSizeMB < 4096
                        onClicked: window.editor.maxContentFileSizeMB = window.editor.maxContentFileSizeMB >= 16
                                   ? window.editor.maxContentFileSizeMB + 16 : window.editor.maxContentFileSizeMB + 1
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "MB"
                        color: Theme.subtext
                        font.pixelSize: Theme.fontBody
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
                    description: qsTr("在后台记下每个文件里有哪些中日韩文字和英文单词片段，搜索时只打开可能含有它的文件，快得多。只用于 NTFS 磁盘，首次建立需要一段时间，几十万个文件约占 200 MB 磁盘空间")
                                 + (window.editor.contentIndexStatus.length > 0 ? "\n" + window.editor.contentIndexStatus : "")

                    ToggleSwitch {
                        checked: window.editor.contentIndex
                        onToggled: (on) => window.editor.contentIndex = on
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
                                 : qsTr("“显卡加速”文字最清晰；“省内存”少占约 50 MB 内存，但文字偏模糊；“自动”在内存不超过 16 GB 时省内存")

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
                        labels: [qsTr("省内存"), qsTr("显卡加速"), qsTr("自动")]
                        current: Math.max(0, window.renderers.indexOf(window.editor.renderer))
                        onActivated: (index) => window.editor.renderer = window.renderers[index]
                    }
                }

                SettingRow {
                    title: qsTr("数据文件夹")
                    description: qsTr("文件索引、内容索引、搜索记录和日志都存放在这里") + "\n" + window.editor.dataFolder

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
}
