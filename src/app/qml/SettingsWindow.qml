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
    required property Backup backup

    readonly property var renderers: ["software", "d3d11"]
    readonly property var languages: ["system", "zh", "en"]
    readonly property var transparencies: ["off", "on"]
    readonly property var dialogBarPlaces: ["auto", "below", "left", "right"]
    readonly property var pages: [
        { title: qsTr("打开 Win顺"), glyph: "" }, // Keyboard
        { title: qsTr("外观"), glyph: "\uE771" }, // Personalize
        { title: qsTr("搜索范围"), glyph: "" }, // Folder
        { title: qsTr("结果列表"), glyph: "\uE8FD" }, // BulletedList
        { title: qsTr("文件内容搜索"), glyph: "" }, // Document
        { title: qsTr("网页搜索"), glyph: "\uE774" }, // Globe
        { title: qsTr("收藏夹"), glyph: "\uE734" }, // FavoriteStar
        { title: qsTr("剪贴板"), glyph: "\uE77F" }, // Paste
        { title: qsTr("高级"), glyph: "" }, // Settings
        { title: qsTr("AI 助手"), glyph: "" }, // Robot
        { title: qsTr("插件"), glyph: "" }, // Puzzle
        { title: Gleaning.title, gleaning: true } // the author's works, set apart at the bottom of the list
    ]
    // Other names of each page, for the search; ";" between them. A word
    // here brings up the whole page, so never one of its options' words.
    readonly property var pageKeywords: [
        qsTr("唤出;呼出;打开方式"),
        qsTr("皮肤;颜色;界面;appearance"),
        qsTr("搜索位置;磁盘;scope"),
        qsTr("搜索结果;结果;列表;results"),
        qsTr("全文搜索;全文;文字;content"),
        qsTr("搜索引擎;网址;书签;百度;谷歌;必应;web"),
        qsTr("快捷访问;常去;quick access"),
        qsTr("剪切板;粘贴板;复制;粘贴;clipboard"),
        qsTr("其他;更多;advanced"),
        qsTr("MCP;人工智能;大模型;Claude;Cursor;ai"),
        qsTr("扩展;脚本;PowerShell;Python;内置功能;模块;plugin;extension;module"),
        ""
    ]
    // The tabs of the pages too long for one screen, each a few sections
    // (SettingsSection.tab); [] for a page without tabs.
    readonly property var pageTabs: [
        [qsTr("快捷键", "settings tab"), qsTr("任务栏"), qsTr("对话框"), qsTr("启动与历史")],
        [],
        [qsTr("文件夹"), qsTr("名称和类型"), qsTr("磁盘"), qsTr("网络与 WSL")],
        [qsTr("排序"), qsTr("显示与预览"), qsTr("操作习惯"), qsTr("文件类型")],
        window.editor.contentSearch ? [qsTr("常规"), qsTr("大小上限")] : [], // 大小上限: nothing while off
        [],
        [],
        [qsTr("记录"), qsTr("快捷键", "settings tab"), qsTr("保留")],
        [],
        window.editor.mcp ? [qsTr("权限"), qsTr("连接客户端"), qsTr("隐私与记录")] : [], // the others: nothing while off
        window.editor.plugins.developerMode ? [qsTr("已安装"), qsTr("插件库"), qsTr("开发者")] : [qsTr("已安装"), qsTr("插件库")], // 开发者: in developer mode only
        []
    ]
    readonly property int gleaningPage: 11
    readonly property int webPage: 5
    readonly property int favoritesPage: 6 // 收藏夹 (FavoritesSection)
    readonly property int clipboardPage: 7
    readonly property int advancedPage: 8 // where the updates are
    readonly property int aiPage: 9 // AI 助手: the MCP server (AiAccessSection, AiClientsSection, AiPrivacySection)
    readonly property int pluginsPage: 10 // 插件: 已安装 (PluginsSection), 插件库 (PluginGallerySection), 开发者 (PluginDevSection)
    readonly property string tapKey: editor.doubleTapKey === "alt" ? "Alt" : editor.doubleTapKey === "shift" ? "Shift" : "Ctrl" // the key tapped twice to open Win顺
    property int currentPage: 0
    property int currentTab: 0
    property var lastTabs: [] // by page: the tab it was left on, to come back to
    // A tab gone from the page on show (its switch turned off): the last one left.
    onPageTabsChanged: currentTab = Math.min(currentTab, Math.max(0, (pageTabs[currentPage]?.length ?? 0) - 1))
    // On the 拾穗计划 page the whole window, sidebar included, is a warm scene.
    // Not while searching: the results are on the usual page.
    readonly property bool warm: currentPage === gleaningPage && !searching
    readonly property bool searching: settingsSearch.active
    property var beforeSearch: null // {page, y}: where the search started, to go back to when the box is cleared
    property bool leavingSearch: false // for another page: no going back

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

    // Forcing it onto contentItem would hand it straight back to the item
    // inside that has it (a hotkey recorder would go on recording).
    function clearFocus() {
        if (window.activeFocusItem && window.activeFocusItem !== window.contentItem)
            window.activeFocusItem.focus = false
    }

    // The taskbar button is pinned, Windows' search hidden, a file manager
    // installed, outside Win顺: looked at again when the user comes back.
    onActiveChanged: {
        if (active) {
            editor.refreshTaskbarState()
            editor.refreshFileManagers()
            editor.refreshNumberKeys()
            editor.refreshDoubleCtrlClash()
            editor.refreshOpusFolderMenu()
        }
    }

    // Settings::fileManager's names, as the user knows them.
    function fileManagerName(name) {
        switch (name) {
        case "totalcmd": return "Total Commander"
        case "dopus": return "Directory Opus"
        case "xyplorer": return "XYplorer"
        case "pulse": return "Pulse"
        case "onecommander": return "OneCommander"
        case "custom": return qsTr("其他（自定义）")
        default: return qsTr("资源管理器")
        }
    }

    // To a page, on `tab` if given; else on the tab it was left on.
    function showPage(index, tab) {
        if (searching) {
            leavingSearch = true
            searchBox.clear()
            leavingSearch = false
        }
        lastTabs[currentPage] = currentTab
        currentPage = index
        // A tab that has gone since (大小上限 with content search off): the first.
        currentTab = Math.min(tab ?? lastTabs[index] ?? 0, Math.max(0, (pageTabs[index]?.length ?? 0) - 1))
        flick.stopScrolling()
        flick.contentY = 0
    }

    function showTab(tab) {
        if (tab === currentTab)
            return
        currentTab = tab
        flick.stopScrolling()
        flick.contentY = 0
    }

    // Ctrl+Tab and Ctrl+Shift+Tab: the next or previous tab, round.
    function cycleTab(delta) {
        const count = pageTabs[currentPage]?.length ?? 0
        if (count > 1)
            showTab((currentTab + delta + count) % count)
    }

    // The settings, searched: the box above the categories, the options on
    // the right narrowed down to those that fit (SettingRow, SettingsSection).
    SettingsSearch {
        id: settingsSearch
        query: searchBox.query
        highlightColor: Theme.accent
        onQueryChanged: {
            if (active && !window.beforeSearch)
                window.beforeSearch = { page: window.currentPage, tab: window.currentTab, y: flick.contentY }
        }
        onQueryApplied: (wasActive) => window.searchChanged(wasActive)
    }

    function searchChanged(wasActive) {
        if (searching) {
            flick.stopScrolling()
            flick.contentY = 0
            pickBest()
            return
        }
        settingsSearch.current = null
        const before = beforeSearch
        beforeSearch = null
        if (!wasActive || leavingSearch || !before)
            return
        // Cleared: back where it started.
        flick.stopScrolling()
        currentPage = before.page
        currentTab = before.tab
        layoutNow()
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, before.y))
    }

    // The rows on show, in the order they stand, with their scores.
    function results() {
        const list = []
        for (const result of settingsSearch.results())
            list.push({ row: result.row, score: result.score, key: result.row.orderKey() })
        list.sort((a, b) => a.key - b.key)
        return list
    }

    // The row that fits best is the one Enter goes to.
    function pickBest() {
        let best = null
        for (const result of results()) {
            if (!best || result.score > best.score)
                best = result
        }
        settingsSearch.current = best ? best.row : null
    }

    // ↑ and ↓ in the search box: through the results; with nothing typed,
    // ↓ goes to the categories.
    function moveCurrent(delta) {
        if (!searching) {
            if (delta > 0)
                navItemAt(currentPage).forceActiveFocus()
            return
        }
        const list = results()
        if (list.length === 0)
            return
        const at = list.findIndex(result => result.row === settingsSearch.current)
        const to = at < 0 ? 0 : Math.max(0, Math.min(list.length - 1, at + delta))
        settingsSearch.current = list[to].row
        revealRow(list[to].row)
    }

    function goToCurrent() {
        if (searching && settingsSearch.current)
            goTo(settingsSearch.current)
    }

    // Leaves the search for the row's page, the row in view and its switch,
    // if it has one, given the keyboard.
    function goTo(row) {
        row.revealRequested()
        showPage(row.section.page, row.section.tab)
        layoutNow()
        // From the top of the page while the row shows there whole; else
        // with the row near the top.
        const top = row.mapToItem(flick.contentItem, 0, 0).y
        flick.contentY = top + row.height + 24 <= flick.height
                         ? 0 : Math.max(0, Math.min(flick.contentHeight - flick.height, top - 24))
        row.focusControl() // a switch; else the box keeps the keyboard, for the next search
        row.flash()
    }

    // Scrolls the page just enough for the row to be in view.
    function revealRow(row) {
        flick.stopScrolling()
        const top = row.mapToItem(flick.contentItem, 0, 0).y
        const bottom = top + row.height
        if (top - 48 < flick.contentY)
            flick.contentY = Math.max(0, top - 48) // with its page's name above it
        else if (bottom + 24 > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.contentHeight - flick.height, bottom + 24 - flick.height)
    }

    // Positions the sections and rows now, rather than before the next frame.
    function layoutNow() {
        for (let i = 0; i < page.children.length; ++i) {
            const child = page.children[i]
            if (child.layoutNow)
                child.layoutNow()
        }
        page.forceLayout()
    }

    // Scrolls the categories, when they do not all fit, to show `item`.
    function revealNavItem(item) {
        if (!navFlick.interactive)
            return
        const top = item.mapToItem(navContent, 0, 0).y
        const bottom = top + item.height
        if (top - 8 < navFlick.contentY)
            navFlick.contentY = Math.max(0, top - 8)
        else if (bottom + 8 > navFlick.contentY + navFlick.height)
            navFlick.contentY = Math.min(navFlick.contentHeight - navFlick.height, bottom + 8 - navFlick.height)
    }

    Shortcut { // Ctrl+F: to the search box
        sequences: [StandardKey.Find]
        enabled: !window.editor.recordingHotkey && !updateDialog.open && !pluginDialog.open && !webDialog.active
        onActivated: searchBox.focusAll()
    }
    Shortcut { // Ctrl+Tab, Ctrl+Shift+Tab: through the page's tabs
        sequences: [StandardKey.NextChild]
        enabled: !window.searching && !window.warm && !window.editor.recordingHotkey && !updateDialog.open && !webDialog.active
        onActivated: window.cycleTab(1)
    }
    Shortcut {
        // Not StandardKey.PreviousChild: its Ctrl+Shift+Backtab never matches
        // on Windows (Qt 6.12), the written-out keys do.
        sequences: ["Ctrl+Shift+Tab"]
        enabled: !window.searching && !window.warm && !window.editor.recordingHotkey && !updateDialog.open && !webDialog.active
        onActivated: window.cycleTab(-1)
    }

    function showUpdateDialog() { updateDialog.show() } // App::showUpdate
    function showClipboardPage() { showPage(clipboardPage) } // App::showClipboardSettings
    function showWebPage() { showPage(webPage) } // App::showWebSettings
    function showFileTypesPage() { goTo(fileTypesRow) } // App::showFileTypeSettings: the launcher strip's +
    // Win顺 restarted from here (App::restart, --settings-at): back where it
    // was, "page,tab"; after 索引方式 changed the rights it runs with, that row.
    function place() { return currentPage + "," + currentTab }
    function showPlace(where) {
        if (where === "indexAccess") {
            goTo(indexAccessRow)
            return
        }
        const at = String(where).split(",").map(Number)
        if (at.length === 2 && at.every(Number.isInteger) && at[0] >= 0 && at[0] < pageTabs.length)
            showPage(at[0], at[1])
    }
    // open_settings (App::openAiSettings): that page and tab, then the row
    // whose title is given, found with the search box and gone to (its page,
    // the row in view and flashed). No row by that title: the page it is.
    function showSetting(page, tab, title) {
        showPage(page, tab >= 0 ? tab : undefined)
        if (!title)
            return
        searchBox.setText(title)
        Qt.callLater(() => {
            if (settingsSearch.current)
                goTo(settingsSearch.current)
            else
                searchBox.clear()
        })
    }
    function showFavoritesPage(path) { // App::showFavoriteSettings: that favorite's form open
        showPage(favoritesPage)
        favoritesSection.edit(path)
    }
    // The 插件 page, scrolled to the plugin's card, which is marked for a
    // moment (a plugin's row opening "winshun:settings": Launcher.pluginSettingsRequested).
    function showPluginPage(pluginId) {
        showPage(pluginsPage, 0) // the cards' tab, whichever was left on
        const card = pluginsSection.cardOf(pluginId)
        if (!card)
            return
        pluginsSection.expand(pluginId) // its settings are what was asked for
        layoutNow()
        revealRow(card)
        pluginsSection.reveal(pluginId)
        // A window opened just now has not laid out the cards' column yet (layoutNow
        // places only the sections): every card is still at y 0. Once more after a frame.
        revealAfterFrame = pluginId
    }
    property string revealAfterFrame
    Connections {
        target: window
        enabled: window.revealAfterFrame.length > 0
        function onFrameSwapped() {
            const card = pluginsSection.cardOf(window.revealAfterFrame)
            window.revealAfterFrame = ""
            if (card)
                window.revealRow(card)
        }
    }

    // Files dropped from File Explorer (which a DropArea never hears of: see
    // FileDropTarget): on 收藏夹 added, on 插件 installed, on 搜索范围 › 文件夹
    // indexed (GitHub #52; the first problem stays shown).
    FileDropTarget {
        window: window
        onDropped: (urls) => {
            if (window.searching)
                return
            if (window.currentPage === window.favoritesPage) {
                favoritesSection.favorites.addPaths(urls)
            } else if (window.currentPage === window.pluginsPage) {
                for (const url of urls)
                    pluginsSection.plugins.install(String(url))
            } else if (window.currentPage === 2 && window.currentTab === 0) {
                let problem = ""
                let networkPath = ""
                for (const url of urls) {
                    if (!foldersRow.add(String(url)) && problem.length === 0) {
                        problem = foldersRow.problem
                        networkPath = foldersRow.networkPath
                    }
                }
                foldersRow.problem = problem
                foldersRow.networkPath = networkPath
            }
        }
    }

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
        value: hotkeyRecorder.recording || clipHotkeyRecorder.recording || folderMenuHotkeyRecorder.recording
               || pluginsSection.recordingHotkey
               || window.editor.plugins.recordingHotkey
    }
    Binding { // the plugin cards read their processes' memory only while on show
        target: window.editor.plugins
        property: "active"
        value: window.visible && !window.searching && window.currentPage === window.pluginsPage && window.currentTab === 0
    }
    Binding { // the 插件库 fetches its list while its tab is on show (never for the settings search)
        target: window.editor.plugins.gallery
        property: "active"
        value: window.visible && !window.searching && window.currentPage === window.pluginsPage && window.currentTab === 1
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
            readonly property bool selected: index === window.currentPage && !window.searching
            // While searching: its options on show.
            readonly property int found: window.searching ? settingsSearch.counts[index] ?? 0 : -1
            readonly property bool gleaning: modelData.gleaning === true
            readonly property color foreground: window.warm ? (selected ? "white" : WarmPalette.ink)
                                              : navArea.pressed ? Theme.subtext : Theme.text

            function select(to) {
                window.showPage(to)
                window.navItemAt(to).forceActiveFocus()
            }

            width: navList.width
            height: navList.itemHeight
            radius: Theme.controlRadius
            opacity: found === 0 ? 0.45 : 1
            color: window.warm ? (navArea.containsMouse && !selected ? WarmPalette.chip : "transparent")
                 : selected ? Theme.navSelected : navArea.containsMouse ? Theme.navHover : "transparent"
            activeFocusOnTab: true

            Keys.onSpacePressed: window.showPage(navItem.index)
            Keys.onReturnPressed: window.showPage(navItem.index)
            Keys.onEnterPressed: window.showPage(navItem.index)
            Keys.onUpPressed: {
                if (navItem.index === 0)
                    searchBox.focusAll()
                else
                    select(navItem.index - 1)
            }
            Keys.onDownPressed: select(Math.min(window.pages.length - 1, navItem.index + 1))
            onActiveFocusChanged: if (activeFocus) window.revealNavItem(navItem)

            Rectangle { // the selected item on the warm scene
                visible: window.warm && navItem.selected
                anchors.fill: parent
                radius: Theme.largeControlRadius
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
                width: parent.width - x - 12 - (count.visible ? count.width + 8 : 0)
                anchors.verticalCenter: parent.verticalCenter
                text: navItem.modelData.title
                color: navItem.foreground
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Text { // how many of its options fit the words typed
                id: count
                visible: navItem.found > 0
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: navItem.found
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }

            Rectangle { // keyboard focus
                visible: navItem.activeFocus
                anchors.fill: parent
                radius: Theme.controlRadius
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

        SettingsSearchBox {
            id: searchBox
            x: 12
            y: 8
            width: parent.width - 24
            warm: window.warm
            onMoved: (delta) => window.moveCurrent(delta)
            onActivated: window.goToCurrent()
            onEscaped: window.clearFocus()
        }

        Flickable { // scrolls when the window is too low for all the categories
            id: navFlick
            y: searchBox.y + searchBox.height + 4
            width: parent.width
            height: (navNote.visible ? navNote.y : versionLink.y) - 8 - y
            contentWidth: width
            contentHeight: navContent.height
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            pixelAligned: true
            clip: interactive

            Item {
                id: navContent
                width: navFlick.width
                height: navList.y + navList.height + 8

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
                    visible: !window.warm && !window.searching // on the warm scene the item itself is filled
                    x: navList.x
                    y: navList.y + (window.currentPage === window.gleaningPage ? gleaningNav.y
                                    : window.currentPage * (navList.itemHeight + navList.spacing))
                       + (navList.itemHeight - height) / 2
                    width: 3
                    height: 16
                    radius: Theme.smallRadius
                    color: Theme.accent

                    Behavior on y { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                }
            }
        }

        Rectangle { // the version; with a newer one, says so. A click goes to the updates
            id: versionLink
            x: 16
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14
            width: versionRow.implicitWidth + 16
            height: versionRow.implicitHeight + 8
            radius: Theme.controlRadius
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
            id: navNote
            // Only where there is room for it below the categories.
            visible: !window.warm && navFlick.y + navContent.height + 8 <= y
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
        function stopScrolling() {
            wheelScroll.cancel()
            cancelFlick()
        }
        ListWheelScroll {
            id: wheelScroll
            view: flick
            animate: SystemTheme.animations
        }

        MouseArea { // clicking empty space finishes editing a field
            width: flick.width
            height: Math.max(flick.height, flick.contentHeight)
            onClicked: window.clearFocus()
        }

        Column { // the current category's options; the other sections are hidden
            id: page

            // What the sections and their rows read (SettingsSection.qml).
            readonly property SettingsSearch search: settingsSearch
            readonly property int currentPage: window.currentPage
            readonly property int currentTab: window.currentTab
            readonly property var pages: window.pages
            readonly property var pageKeywords: window.pageKeywords
            function openPage(index, tab) { window.showPage(index, tab) }
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
            transform: ScrollPixelAlignment {
                view: flick
                viewY: flick.y
                itemY: page.y
                pixelBias: 0.02
            }

            Column {
                width: parent.width
                spacing: 6

                Row {
                    spacing: 12

                    Text {
                        id: pageTitle
                        text: window.searching ? qsTr("搜索结果") : window.pages[window.currentPage].title
                        color: Theme.text
                        font.pixelSize: Theme.fontDisplay
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: window.searching && settingsSearch.total > 0
                        anchors.baseline: pageTitle.baseline
                        text: qsTr("%n 项", "", settingsSearch.total)
                        color: Theme.subtext
                        font.pixelSize: Theme.fontBody
                    }
                }

                PageTabs { // while searching, the rows that fit are shown from all of them
                    visible: !window.searching && labels.length > 0
                    labels: window.pageTabs[window.currentPage] ?? []
                    current: window.currentTab
                    onActivated: (index) => {
                        window.clearFocus()
                        window.showTab(index)
                    }
                }
            }

            Column { // nothing fits the words typed
                id: noResults

                // What it may be instead: a setting of Windows (as the
                // launcher finds them), such as 蓝牙 or 显示器.
                readonly property var windowsSettings: visible ? window.editor.findWindowsSettings(searchBox.query, 3) : []

                visible: window.searching && settingsSearch.total === 0
                width: parent.width
                spacing: 8

                Text {
                    width: parent.width
                    text: qsTr("没有找到“%1”相关的设置").arg(searchBox.query.replace(/\s+/g, " ").trim())
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: noResults.windowsSettings.length > 0
                          ? qsTr("要找的可能是 Windows 的设置：")
                          : qsTr("换个说法、少输几个字，或者用拼音首字母试试，例如 jtb 找“剪贴板”")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
                Rectangle {
                    visible: noResults.windowsSettings.length > 0
                    width: parent.width
                    height: windowsList.implicitHeight
                    radius: Theme.cardRadius
                    color: Theme.card
                    border.width: 1
                    border.color: Theme.cardBorder

                    Column {
                        id: windowsList
                        width: parent.width

                        Repeater {
                            model: noResults.windowsSettings

                            delegate: Item { // opens it, as the launcher does
                                id: windowsRow

                                required property int index
                                required property var modelData

                                width: windowsList.width
                                height: 52

                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    radius: Theme.insetCardRadius
                                    color: windowsArea.pressed ? Theme.selection : windowsArea.containsMouse ? Theme.navHover : "transparent"
                                }
                                Rectangle {
                                    visible: windowsRow.index > 0
                                    x: Theme.edge(Screen.devicePixelRatio) // inside the card's border
                                    width: parent.width - 2 * x
                                    height: Theme.edge(Screen.devicePixelRatio)
                                    color: Theme.cardBorder
                                }
                                Item {
                                    id: windowsIcon
                                    x: 18
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 24
                                    height: 24

                                    Image { // whole device pixels, unsmoothed (see ResultRow)
                                        readonly property real dpr: Screen.devicePixelRatio
                                        readonly property int pixels: Math.round(windowsIcon.width * dpr)

                                        x: 1 / 64
                                        y: 1 / 64
                                        width: pixels / dpr
                                        height: pixels / dpr
                                        source: windowsRow.modelData.icon
                                        sourceSize.width: pixels / dpr
                                        sourceSize.height: pixels / dpr
                                        smooth: false
                                        asynchronous: true
                                        fillMode: Image.PreserveAspectFit
                                    }
                                }
                                Text {
                                    anchors.left: windowsIcon.right
                                    anchors.leftMargin: 12
                                    anchors.right: windowsChevron.left
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: windowsRow.modelData.name
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                    elide: Text.ElideRight
                                }
                                Glyph {
                                    id: windowsChevron
                                    anchors.right: parent.right
                                    anchors.rightMargin: 18
                                    anchors.verticalCenter: parent.verticalCenter
                                    glyph: "\uE76C" // ChevronRight
                                    size: 12
                                }
                                MouseArea {
                                    id: windowsArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: window.editor.openWindowsSetting(windowsRow.modelData.command)
                                }
                            }
                        }
                    }
                }
            }

            SettingsSection {
                page: 0
                width: parent.width

                SettingRow {
                    title: qsTr("双击 Ctrl、Alt 或 Shift 打开")
                    keywords: qsTr("双击;连按;呼出;唤出;double ctrl;double alt;double shift").split(";")
                    unlocks: [doubleCtrlKeyRow, gamesRow, fullScreenRow, doubleCtrlAppsRow]
                    description: window.tapKey === "Shift"
                                 ? qsTr("连按两下 %1，打开或关闭搜索框。输入法用 %1 切换中英文时，双击会切换两次，最后回到原来的状态。管理员或部分输入法窗口中需要 Win顺 服务支持").arg(window.tapKey)
                                 : qsTr("连按两下 %1，打开或关闭搜索框。管理员或部分输入法窗口中需要 Win顺 服务支持")
                                 .arg(window.tapKey)

                    ScopeTabs {
                        labels: ["Ctrl", "Alt", "Shift", qsTr("关闭")]
                        current: !window.editor.doubleCtrl ? 3 : Math.max(0, ["ctrl", "alt", "shift"].indexOf(window.editor.doubleTapKey))
                        onActivated: (index) => {
                            if (index < 3)
                                window.editor.doubleTapKey = ["ctrl", "alt", "shift"][index]
                            window.editor.doubleCtrl = index < 3
                        }
                    }
                }

                SettingRow {
                    id: doubleCtrlKeyRow
                    shown: window.editor.doubleCtrl
                    title: qsTr("用哪个 %1").arg(window.tapKey)
                    keywords: qsTr("左 Ctrl;右 Ctrl;左 Alt;右 Alt;左 Shift;右 Shift;左边;右边;冲突;PowerToys;查找鼠标;left ctrl;right ctrl;left alt;right alt;left shift;right shift").split(";")
                    description: qsTr("同一个 %1 连按两下才算。和别的软件撞了时，可以只用一边").arg(window.tapKey)
                    bodyShown: window.editor.doubleCtrlClash.length > 0

                    ScopeTabs {
                        labels: [qsTr("左右都行"), qsTr("左 %1").arg(window.tapKey), qsTr("右 %1").arg(window.tapKey)]
                        current: Math.max(0, ["both", "left", "right"].indexOf(window.editor.doubleCtrlKey))
                        onActivated: (index) => window.editor.doubleCtrlKey = ["both", "left", "right"][index]
                    }

                    // PowerToys' Find My Mouse comes on the same double tap: say so, one click away from the other side.
                    body: Row {
                        readonly property string other: window.editor.doubleCtrlClash === "left" ? "right" : "left"
                        visible: window.editor.doubleCtrlClash.length > 0
                        width: parent.width
                        spacing: 8

                        Text {
                            width: Math.min(implicitWidth, parent.width - clashButton.width - parent.spacing)
                            anchors.verticalCenter: parent.verticalCenter
                            text: window.editor.doubleCtrlClash === "left"
                                  ? qsTr("PowerToys 的“查找鼠标”也是双击左 Ctrl，两边会一起出来。")
                                  : qsTr("PowerToys 的“查找鼠标”也是双击右 Ctrl，两边会一起出来。")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                        FlatButton {
                            id: clashButton
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.other === "right" ? qsTr("Win顺 改用右 Ctrl") : qsTr("Win顺 改用左 Ctrl")
                            onClicked: window.editor.doubleCtrlKey = parent.other
                        }
                    }
                }

                SettingRow {
                    id: gamesRow
                    shown: window.editor.doubleCtrl
                    title: qsTr("玩游戏时不响应双击 %1").arg(window.tapKey)
                    keywords: qsTr("游戏;蹲下;误触;game").split(";")
                    description: window.tapKey !== "Ctrl" ? qsTr("游戏全屏或藏起鼠标时不打开，免得误触")
                                                         : qsTr("游戏全屏或藏起鼠标时不打开，免得蹲下时误触")

                    ToggleSwitch {
                        checked: window.editor.doubleCtrlPauseInGames
                        onToggled: (on) => window.editor.doubleCtrlPauseInGames = on
                    }
                }

                SettingRow {
                    id: fullScreenRow
                    shown: window.editor.doubleCtrl
                    title: qsTr("任何程序全屏时都不响应双击 %1").arg(window.tapKey)
                    keywords: qsTr("全屏;视频;幻灯片;误触;fullscreen").split(";")
                    description: qsTr("看视频、放幻灯片时也不打开")

                    ToggleSwitch {
                        checked: window.editor.doubleCtrlPauseInFullScreen
                        onToggled: (on) => window.editor.doubleCtrlPauseInFullScreen = on
                    }
                }

                SettingRow {
                    id: doubleCtrlAppsRow
                    shown: window.editor.doubleCtrl
                    title: qsTr("在这些程序里不响应双击 %1").arg(window.tapKey)
                    keywords: qsTr("排除;程序;游戏;exe").split(";")
                    values: window.editor.doubleCtrlExcludedApps
                    description: qsTr("没被认出来的游戏，可以加在这里")

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
                                    marked: doubleCtrlAppsRow.valueFound(modelData)
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
                    keywords: qsTr("热键;组合键;hotkey;shortcut").split(";")
                    values: [window.editor.hotkey]
                    description: qsTr("另设一个组合键打开搜索框，例如 Alt+Space")

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
                    title: qsTr("再次按下快捷键时")
                    keywords: qsTr("再次;两次;再按;搜索面板;展开;Ctrl+E;面板;关闭;panel;again;expand").split(";")
                    description: qsTr("搜索框已打开时再按一次快捷键（自己设的组合键，或双击 Ctrl、Alt、Shift）：关闭它，或展开成搜索面板，保留已输入的字；面板里再按一次才关闭")

                    ScopeTabs {
                        labels: [qsTr("关闭 Win顺"), qsTr("展开为搜索面板")]
                        current: window.editor.shortcutAgain === "panel" ? 1 : 0
                        onActivated: (index) => window.editor.shortcutAgain = index === 1 ? "panel" : "close"
                    }
                }
            }

            SettingsSection {
                page: 0
                tab: 1
                width: parent.width
                title: qsTr("在任务栏使用 Win顺")
                note: qsTr("任务栏搜索框可单独使用，也可与 Windows 搜索并存。下面的选项按需设置。")

                SettingRow {
                    id: taskbarButtonRow

                    property bool missing: false // no WinShunSearch.exe next to Win顺
                    // The tab last clicked here, while Windows does not show it
                    // yet: the button's pinning asked for (1), or off with the
                    // button still pinned (2). Win顺 can ask Windows to pin its
                    // button; unpinning is left to the user.
                    property int chosen: -1
                    readonly property bool boxes: window.editor.searchBoxSupported // Windows 11; else the button alone
                    readonly property bool boxOn: window.editor.taskbarSearchBox
                    readonly property bool pinned: window.editor.taskbarButtonPinned
                    // Windows 11: 0 the box, 1 the pinned button, 2 neither.
                    readonly property int way: boxOn ? 0 : chosen === 2 ? 2 : pinned || chosen === 1 ? 1 : 2
                    // The box can't be put on this taskbar: the pinned button instead.
                    readonly property bool unreadable: boxes && boxOn && window.editor.searchBoxHidden === "unreadable"
                    // Pinned, but the box or nothing chosen: only the user can take it off.
                    readonly property bool unpinNeeded: boxes && pinned && !unreadable && way !== 1

                    // Clicked again, 固定图标 asks again (its prompt turned down or missed).
                    function choose(index) {
                        chosen = index
                        window.editor.taskbarSearchBox = index === 0
                        if (index === 1 && !pinned)
                            missing = !window.editor.pinTaskbarButton()
                    }

                    title: boxes ? qsTr("任务栏上的 Win顺") : qsTr("把 Win顺 按钮固定到任务栏")
                    keywords: qsTr("固定;任务栏;按钮;图标;搜索框;取消固定;pin;taskbar").split(";")
                    description: {
                        if (!boxes || way === 1)
                            return pinned ? qsTr("已固定，点它就能打开 Win顺")
                                 : boxes ? qsTr("在 Windows 的提示里选“是”；没看到提示，再点一次“固定图标”")
                                 : qsTr("点“固定”后选“是”；打开的若是文件夹，右键其中的图标选“固定到任务栏”")
                        if (way === 2)
                            return qsTr("任务栏上不放 Win顺，用快捷键打开")
                        switch (window.editor.searchBoxHidden) {
                        case "":
                            return qsTr("图标居中时在任务栏最左边，靠左时在托盘图标左边；点它直接打字")
                        case "noroom":
                            return qsTr("任务栏太挤，放不下：取消固定几个图标，或改用“固定图标”")
                        case "unreadable":
                            return pinned ? qsTr("这个任务栏放不了搜索框；Win顺 的图标已固定，可改选“固定图标”")
                                          : qsTr("这个任务栏读不到图标，放不了搜索框；可以改用“固定图标”（原因在 WinShun.log）")
                        case "notaskbar":
                            return qsTr("没找到任务栏；重启资源管理器后会再放")
                        case "offscreen":
                            return qsTr("任务栏收起来了，显示出来时就会放上")
                        case "vertical":
                            return qsTr("任务栏在屏幕侧边，放不下搜索框")
                        default:
                            return qsTr("正在放到任务栏上…")
                        }
                    }
                    bodyShown: unpinNeeded || (missing && !pinned && (!boxes || way === 1))
                    ScopeTabs {
                        visible: taskbarButtonRow.boxes
                        labels: [qsTr("搜索框"), qsTr("固定图标"), qsTr("关闭")]
                        current: taskbarButtonRow.way
                        onActivated: (index) => taskbarButtonRow.choose(index)
                    }
                    FlatButton {
                        visible: !taskbarButtonRow.boxes && !taskbarButtonRow.pinned
                        text: qsTr("固定")
                        glyph: "" // Pin
                        onClicked: taskbarButtonRow.missing = !window.editor.pinTaskbarButton()
                    }

                    body: Text {
                        width: parent.width
                        text: taskbarButtonRow.unpinNeeded
                              ? qsTr("Win顺 的图标还固定在任务栏上，Win顺 取消不了：右键那个图标，选“从任务栏取消固定”")
                              : qsTr("没找到 WinShunSearch.exe，重新安装 Win顺 就有了")
                        color: taskbarButtonRow.unpinNeeded ? Theme.subtext : Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    // Under 搜索框 chosen above: how wide it may be, as
                    // Windows' own search has it (Settings › Taskbar ›
                    // Search). Narrower ones still where that does not fit.
                    shown: taskbarButtonRow.boxes && taskbarButtonRow.boxOn
                    title: qsTr("搜索框样式")
                    keywords: qsTr("样式;仅图标;只显示图标;图标和标签;标签;放大镜;按钮;搜索框;icon;label").split(";")
                    description: window.editor.taskbarSearchBoxStyle === "icon"
                                 ? qsTr("一个搜索图标，像任务栏上的按钮")
                                 : window.editor.taskbarSearchBoxStyle === "label"
                                   ? qsTr("图标加“搜索”两个字，点它打开搜索")
                                   : qsTr("点它直接打字；任务栏挤时自动变窄")

                    ScopeTabs {
                        readonly property var styles: ["box", "label", "icon"]
                        labels: [qsTr("搜索框"), qsTr("图标和标签"), qsTr("仅图标")]
                        current: Math.max(0, styles.indexOf(window.editor.taskbarSearchBoxStyle))
                        onActivated: (index) => window.editor.taskbarSearchBoxStyle = styles[index]
                    }
                }

                SettingRow {
                    title: qsTr("隐藏任务栏上的 Windows 搜索")
                    keywords: qsTr("隐藏;任务栏;搜索框;taskbar").split(";")
                    description: window.editor.windowsSearchShown
                                 ? qsTr("可在任务栏设置里把“搜索”选成“隐藏”")
                                 : qsTr("已隐藏；可在任务栏设置中恢复")

                    FlatButton {
                        text: qsTr("打开任务栏设置")
                        onClicked: window.editor.openUrl("ms-settings:taskbar")
                    }
                }

                SettingRow {
                    title: qsTr("用 Win+S 打开 Win顺")
                    keywords: qsTr("Win+S;Windows 搜索;截图;Win+Shift+S").split(";")
                    description: qsTr("切换时资源管理器会重启，打开的文件夹窗口会关闭")

                    ToggleSwitch {
                        checked: window.editor.taskbarWinS
                        onToggled: (on) => {
                            // Explorer takes the change when it starts again: at once.
                            window.editor.taskbarWinS = on
                            if (window.editor.canRestartExplorer)
                                window.editor.restartExplorer()
                        }
                    }

                    body: Column {
                        readonly property string winS: window.editor.winSState
                        readonly property bool restartable: (winS === "waiting" || winS === "releasing")
                                                            && window.editor.canRestartExplorer

                        visible: winSStatus.text.length > 0
                        width: parent.width
                        spacing: 10

                        Text {
                            id: winSStatus
                            width: parent.width
                            text: {
                                switch (window.editor.winSState) {
                                case "waiting": return qsTr("资源管理器重启后生效，下次登录 Windows 时也会自动生效")
                                case "releasing": return qsTr("资源管理器重启后，Win+S 和 Win+Shift+S 回到 Windows 自带的")
                                case "failed": return window.editor.taskbarWinS
                                                      ? qsTr("没能修改 Windows 的设置，Win+S 仍是 Windows 搜索")
                                                      : qsTr("没能修改 Windows 的设置，Win+S 和 Win+Shift+S 仍被禁用")
                                case "blocked": return qsTr("Windows 的 Win+S 和 Win+Shift+S 仍被禁用（可能是旧版 Win顺 或其他软件设置的）")
                                default: return ""
                                }
                            }
                            color: window.editor.winSState === "failed" ? Theme.danger : Theme.text
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                        FlatButton {
                            visible: window.editor.winSState === "blocked"
                                     || (window.editor.winSState === "failed" && !window.editor.taskbarWinS)
                            text: qsTr("还给 Windows")
                            onClicked: window.editor.giveBackToWindows("S")
                        }
                        FlatButton {
                            visible: parent.restartable
                            text: qsTr("现在重启资源管理器")
                            glyph: "" // Sync
                            onClicked: window.editor.restartExplorer()
                        }
                    }
                }

                SettingRow {
                    title: qsTr("在开始菜单里打字，也用 Win顺 搜索")
                    keywords: qsTr("开始菜单;打字;Win;start menu").split(";")
                    description: qsTr("想用 Windows 自带的搜索时，先按一下左 Alt 再打字")

                    ToggleSwitch {
                        checked: window.editor.taskbarStartTyping
                        onToggled: (on) => window.editor.taskbarStartTyping = on
                    }
                }
            }

            SettingsSection {
                page: 0
                tab: 2
                width: parent.width

                SettingRow {
                    shown: window.editor.fileManagers.length > 1
                    title: qsTr("用哪个文件管理器打开文件夹")
                    keywords: qsTr("文件管理器;资源管理器;打开文件夹;打开所在位置;Total Commander;Directory Opus;TC;Opus;XYplorer;Pulse;OneCommander;自定义;其他;Tessoa;custom").split(";")
                    description: qsTr("打开文件夹、打开文件所在位置时用它；没装的不列")

                    body: Flow { // keep full names inside the card, even with every manager installed
                        width: parent.width
                        spacing: 6

                        Repeater {
                            model: window.editor.fileManagers

                            delegate: ScopeTabs {
                                required property string modelData
                                labels: [window.fileManagerName(modelData)]
                                current: window.editor.fileManager === modelData ? 0 : -1
                                onActivated: window.editor.fileManager = modelData
                            }
                        }
                    }
                }

                SettingRow {
                    id: customManagerRow
                    shown: window.editor.fileManager === "custom"
                    title: qsTr("其他文件管理器的命令")
                    keywords: qsTr("文件管理器;自定义;命令;命令行;其他;Tessoa;custom;command").split(";")
                    description: qsTr("命令里的 {path} 会换成文件夹或文件的完整路径，Win顺 自动加引号。这样的文件管理器 Win顺 读不到它正在显示的文件夹：对话框里的 Ctrl+G 和搜索框的建议只认资源管理器、Total Commander、Directory Opus、XYplorer、Pulse 和 OneCommander")
                    values: [window.editor.fileManagerOpenCommand, window.editor.fileManagerRevealCommand]

                    body: [
                        Text {
                            text: qsTr("打开文件夹")
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                        },
                        Row {
                            width: parent.width
                            spacing: 8

                            InputBox {
                                id: openCommandBox
                                width: parent.width - browseOpen.width - parent.spacing
                                text: window.editor.fileManagerOpenCommand
                                // The path outside qsTr(): lupdate reads backslashes in QML strings wrongly.
                                placeholder: qsTr("例如 %1").arg("\"C:\\Program Files\\Tessoa\\tessoa.exe\" {path}")
                                onEditingFinished: window.editor.fileManagerOpenCommand = text
                            }
                            FlatButton {
                                id: browseOpen
                                text: qsTr("浏览…")
                                onClicked: {
                                    const command = window.editor.pickFileManagerProgram()
                                    if (command.length > 0) {
                                        openCommandBox.text = command
                                        window.editor.fileManagerOpenCommand = command
                                    }
                                }
                            }
                        },
                        Text {
                            text: qsTr("打开所在位置（选中文件）")
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                        },
                        Row {
                            width: parent.width
                            spacing: 8

                            InputBox {
                                id: revealCommandBox
                                width: parent.width - browseReveal.width - parent.spacing
                                text: window.editor.fileManagerRevealCommand
                                placeholder: qsTr("可不填：就打开文件所在的文件夹")
                                onEditingFinished: window.editor.fileManagerRevealCommand = text
                            }
                            FlatButton {
                                id: browseReveal
                                text: qsTr("浏览…")
                                onClicked: {
                                    const command = window.editor.pickFileManagerProgram()
                                    if (command.length > 0) {
                                        revealCommandBox.text = command
                                        window.editor.fileManagerRevealCommand = command
                                    }
                                }
                            }
                        },
                        Text {
                            visible: window.editor.fileManagerProblem.length > 0
                            width: parent.width
                            text: window.editor.fileManagerProblem
                            textFormat: Text.PlainText
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("资源管理器右键“用 Win顺 搜索此处”")
                    keywords: qsTr("右键;右键菜单;上下文菜单;搜索此文件夹;在此搜索;资源管理器;context menu").split(";")
                    description: qsTr("右键文件夹、文件夹空白处或磁盘，只在那里面搜；Windows 11 在“显示更多选项”里")

                    ToggleSwitch {
                        checked: window.editor.explorerMenu
                        onToggled: (on) => window.editor.explorerMenu = on
                    }
                }

                SettingRow {
                    id: explorerTypingRow
                    title: qsTr("在资源管理器里直接打字搜索")
                    keywords: qsTr("打字;输入;直接搜索;当前文件夹;资源管理器;Listary;type to search;explorer").split(";")
                    description: qsTr("文件列表有焦点时直接打字就用 Win顺 搜，和 Listary 一样；代替了资源管理器自己的“按首字母跳到文件”")
                    unlocks: [folderTypingRow]

                    ToggleSwitch {
                        checked: window.editor.explorerTyping
                        onToggled: (on) => window.editor.explorerTyping = on
                    }
                }

                SettingRow {
                    id: opusTypingRow
                    title: qsTr("在 Directory Opus 里直接打字搜索")
                    keywords: qsTr("Opus;DOpus;直接打字;输入;当前文件夹;双栏;type to search").split(";")
                    description: qsTr("文件列表有焦点时，从字母或数字开始输入，就用 Win顺 搜索当前标签的文件夹。符号键保留给 Opus，改名和地址栏不受影响。")
                    unlocks: [folderTypingRow]

                    ToggleSwitch {
                        objectName: "opusTypingToggle"
                        checked: window.editor.opusTyping
                        Accessible.name: qsTr("在 Directory Opus 里直接打字搜索")
                        onToggled: (on) => window.editor.opusTyping = on
                    }
                }

                SettingRow { // GitHub #110
                    id: folderTypingRow
                    shown: window.editor.explorerTyping || window.editor.opusTyping
                    title: qsTr("在文件夹里打字时")
                    keywords: qsTr("打字;当前文件夹;优先;只搜;其他位置;子文件夹;Listary;Ctrl+D;current folder;type to search").split(";")
                    description: qsTr("当前文件夹优先：先列这个文件夹和子文件夹里的，再列其他位置的，和 Listary 一样。搜索时按 Ctrl+D 临时切换")

                    ScopeTabs {
                        labels: [qsTr("当前文件夹优先"), qsTr("只搜当前文件夹")]
                        current: window.editor.folderTyping === "only" ? 1 : 0
                        onActivated: (index) => window.editor.folderTyping = index === 1 ? "only" : "first"
                    }
                }

                SettingRow {
                    id: folderMenuRow
                    title: qsTr("双击空白处弹出文件夹菜单")
                    keywords: qsTr("双击;空白;空白处;收藏;收藏夹;最近;文件夹菜单;资源管理器;桌面;Listary;double-click;folder menu").split(";")
                    description: qsTr("在资源管理器或桌面的空白处双击，列出收藏的、最近的和别的窗口里的文件夹，选一个就在这个窗口里打开；Listary 在运行时由它来弹")

                    ToggleSwitch {
                        checked: window.editor.folderMenu
                        onToggled: (on) => window.editor.folderMenu = on
                    }
                }

                SettingRow {
                    shown: window.editor.folderMenu
                    title: qsTr("在资源管理器的空白处双击")
                    keywords: qsTr("双击;空白;文件夹菜单;资源管理器").split(";")

                    ToggleSwitch {
                        checked: window.editor.folderMenuExplorer
                        onToggled: (on) => window.editor.folderMenuExplorer = on
                    }
                }

                SettingRow {
                    shown: window.editor.folderMenu
                    title: qsTr("在桌面的空白处双击")
                    keywords: qsTr("双击;空白;文件夹菜单;桌面").split(";")

                    ToggleSwitch {
                        checked: window.editor.folderMenuDesktop
                        onToggled: (on) => window.editor.folderMenuDesktop = on
                    }
                }

                // At least one group stays on (GitHub #51): the editor keeps
                // the last one, and the switch lights up to say so.
                SettingRow {
                    shown: window.editor.folderMenu
                    title: qsTr("菜单里列出收藏的文件夹")
                    keywords: qsTr("双击;文件夹菜单;收藏;收藏夹").split(";")

                    ToggleSwitch {
                        checked: window.editor.folderMenuFavorites
                        onToggled: (on) => {
                            window.editor.folderMenuFavorites = on
                            if (checked !== on)
                                pulse()
                        }
                    }
                }

                SettingRow {
                    shown: window.editor.folderMenu
                    title: qsTr("菜单里列出最近的文件夹")
                    keywords: qsTr("双击;文件夹菜单;最近;最近打开").split(";")

                    ToggleSwitch {
                        checked: window.editor.folderMenuRecent
                        onToggled: (on) => {
                            window.editor.folderMenuRecent = on
                            if (checked !== on)
                                pulse()
                        }
                    }
                }

                SettingRow {
                    shown: window.editor.folderMenu
                    title: qsTr("菜单里列出别的窗口里的文件夹")
                    keywords: qsTr("双击;文件夹菜单;其他窗口;别的窗口;已打开").split(";")

                    ToggleSwitch {
                        checked: window.editor.folderMenuWindows
                        onToggled: (on) => {
                            window.editor.folderMenuWindows = on
                            if (checked !== on)
                                pulse()
                        }
                    }
                }

                SettingRow {
                    id: opusMenuRow
                    // on | stale | off | taken | manual | busy (SettingsEditor::opusFolderMenu)
                    readonly property string setup: window.editor.opusFolderMenu
                    property bool copied: false
                    property bool confirming: false // Opus runs: it restarts after a second click
                    shown: window.editor.folderMenu && window.editor.fileManagers.indexOf("dopus") >= 0
                    title: qsTr("在 Directory Opus 的空白处双击")
                    keywords: qsTr("双击;空白;文件夹菜单;Directory Opus;Opus;dopus").split(";")
                    description: {
                        let text
                        switch (setup) {
                        case "on":
                            text = qsTr("已在 Opus 的设置里加好")
                            break
                        case "stale":
                            text = qsTr("Opus 里设的是别处的 Win顺，更新一下就好")
                            break
                        case "off":
                            text = qsTr("要在 Opus 的设置里加一条命令，Win顺 可以替你加")
                            break
                        case "taken":
                            text = qsTr("Opus 里双击空白处已设成“%1”；换成 Win顺 的菜单后，点“取消设置”能换回来")
                                .arg(window.editor.opusFolderMenuTaken)
                            break
                        case "busy":
                            text = qsTr("正在设置 Opus…")
                            break
                        default:
                            text = qsTr("Opus 还没有设置文件（运行一次 Opus 就有了），或 Win顺 认不出它：照下面的步骤在 Opus 里设一次")
                        }
                        const problem = window.editor.opusFolderMenuProblem
                        return problem ? text + "\n" + problem : text
                    }
                    // By hand: when Win顺 cannot do it, or could not this time.
                    readonly property bool byHand: setup === "manual"
                        || (setup !== "busy" && window.editor.opusFolderMenuProblem !== "")
                    bodyShown: byHand

                    FlatButton {
                        visible: opusMenuRow.setup !== "manual"
                        enabled: opusMenuRow.setup !== "busy"
                        flat: opusMenuRow.setup === "on" && !opusMenuRow.confirming
                        text: opusMenuRow.confirming ? qsTr("会重启 Opus，再点一次")
                            : opusMenuRow.setup === "on" ? qsTr("取消设置")
                            : opusMenuRow.setup === "stale" ? qsTr("更新", "动词：把 Opus 里的命令换成这一份 Win顺 的")
                            : opusMenuRow.setup === "taken" ? qsTr("换成 Win顺 的菜单")
                            : opusMenuRow.setup === "busy" ? qsTr("正在设置…")
                            : qsTr("自动设置")
                        onClicked: {
                            window.editor.refreshOpusFolderMenu()
                            if (window.editor.opusRunning && !opusMenuRow.confirming) {
                                opusMenuRow.confirming = true
                                opusConfirmReset.restart()
                                return
                            }
                            opusMenuRow.confirming = false
                            window.editor.setOpusFolderMenu(opusMenuRow.setup !== "on")
                        }
                    }
                    FlatButton { // one button on the right: after a failed try it goes under the steps
                        visible: opusMenuRow.setup === "manual"
                        flat: true
                        text: qsTr("打开 Opus 设置")
                        onClicked: window.editor.openOpusPreferences()
                    }

                    Timer {
                        id: opusConfirmReset
                        interval: 4000
                        onTriggered: opusMenuRow.confirming = false
                    }

                    body: Column {
                        width: parent.width
                        spacing: 8

                        Text {
                            width: parent.width
                            text: qsTr("1. 打开 Opus 的“首选项 › 文件显示栏 › 鼠标 › 背景事件”（英文版：Preferences › File Displays › Mouse › Background Events），勾上“左键双击”下的“默认”\n2. 把它右边的命令（预填的 Go UP BACK 是回上一级）换成下面这条，点“确定”")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            lineHeight: 1.3
                            wrapMode: Text.Wrap
                        }
                        Row {
                            width: parent.width
                            spacing: 8

                            InputBox {
                                id: opusCommand
                                width: parent.width - copyOpusCommand.width - parent.spacing
                                    - (openOpusPrefs.visible ? openOpusPrefs.width + parent.spacing : 0)
                                text: window.editor.opusFolderMenuCommand()
                                readOnly: true
                            }
                            FlatButton {
                                id: copyOpusCommand
                                text: opusMenuRow.copied ? qsTr("已复制") : qsTr("复制")
                                onClicked: {
                                    window.editor.copyText(opusCommand.text)
                                    opusMenuRow.copied = true
                                    copiedReset.restart()
                                }
                            }
                            FlatButton {
                                id: openOpusPrefs
                                visible: opusMenuRow.setup !== "manual" // there it is on the right
                                text: qsTr("打开 Opus 设置")
                                onClicked: window.editor.openOpusPreferences()
                            }
                        }
                    }

                    Timer {
                        id: copiedReset
                        interval: 2000
                        onTriggered: opusMenuRow.copied = false
                    }
                }

                SettingRow {
                    // Shown with the double click off too: the hotkey alone
                    // brings the menu (GitHub #25).
                    title: qsTr("用快捷键弹出文件夹菜单")
                    keywords: qsTr("快捷键;热键;组合键;文件夹菜单;收藏;最近;对话框;Directory Opus;Total Commander;XYplorer;Pulse;OneCommander;hotkey;folder menu").split(";")
                    values: [window.editor.folderMenuHotkey]
                    description: qsTr("双击被别的程序占用时用：在资源管理器、桌面、Directory Opus、Total Commander、XYplorer、OneCommander 或“打开”“另存为”对话框里按它，选中的文件夹在那个窗口里打开，在 Pulse 里按就交给 Pulse，按它自己的设置开；在别的窗口里按，用文件管理器打开。再按一次关闭")

                    FlatButton {
                        visible: window.editor.folderMenuHotkey.length > 0 && !folderMenuHotkeyRecorder.recording
                        flat: true
                        text: qsTr("清除")
                        onClicked: window.editor.folderMenuHotkey = ""
                    }
                    HotkeyRecorder {
                        id: folderMenuHotkeyRecorder
                        hotkey: window.editor.folderMenuHotkey
                        onRecorded: (hotkey) => window.editor.folderMenuHotkey = hotkey
                    }

                    body: Text {
                        visible: window.editor.folderMenuHotkeyError.length > 0
                        width: parent.width
                        text: window.editor.folderMenuHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    title: qsTr("Ctrl+G 转到文件管理器的文件夹")
                    keywords: qsTr("跳转;转到;对话框;另存为;打开文件;资源管理器;Total Commander;Directory Opus;jump").split(";")
                    description: qsTr("在“打开”“另存为”里按 Ctrl+G，转到文件管理器当前的文件夹")

                    ToggleSwitch {
                        checked: window.editor.dialogJump
                        onToggled: (on) => window.editor.dialogJump = on
                    }
                }

                SettingRow {
                    id: dialogBarRow
                    title: qsTr("对话框旁的搜索框")
                    keywords: qsTr("对话框;另存为;打开文件;保存").split(";")
                    unlocks: [dialogBarPlaceRow]
                    description: qsTr("在“打开”“另存为”旁搜文件夹和文件，选中后直接转过去")

                    ToggleSwitch {
                        checked: window.editor.dialogBar
                        onToggled: (on) => window.editor.dialogBar = on
                    }
                }

                SettingRow {
                    id: dialogBarPlaceRow
                    shown: window.editor.dialogBar
                    title: qsTr("搜索框的位置")
                    keywords: qsTr("位置;下方;左边;右边;对话框").split(";")
                    description: qsTr("“自动”时放在下方，放不下就换到旁边")

                    ScopeTabs {
                        labels: [qsTr("自动"), qsTr("下方"), qsTr("左边"), qsTr("右边")]
                        current: Math.max(0, window.dialogBarPlaces.indexOf(window.editor.dialogBarPlace))
                        onActivated: (index) => window.editor.dialogBarPlace = window.dialogBarPlaces[index]
                    }
                }

                SettingRow {
                    title: qsTr("对话框自动转到文件管理器的文件夹")
                    keywords: qsTr("自动跳转;跟随;对话框;另存为;资源管理器").split(";")
                    unlocks: [dialogAutoJumpWhenRow]
                    description: qsTr("不用按 Ctrl+G，对话框自己转到文件管理器当前的文件夹")

                    ToggleSwitch {
                        checked: window.editor.dialogAutoJump
                        onToggled: (on) => window.editor.dialogAutoJump = on
                    }
                }

                SettingRow {
                    id: dialogAutoJumpWhenRow
                    shown: window.editor.dialogAutoJump
                    title: qsTr("什么时候转")
                    keywords: qsTr("自动跳转;切回来;上次保存的位置;对话框").split(";")
                    description: window.editor.dialogAutoJumpWhen === "back"
                                 ? qsTr("对话框先停在 Windows 记住的上次位置，从文件管理器切回来时再转过去")
                                 : qsTr("对话框一出现就转过去")

                    ScopeTabs {
                        labels: [qsTr("打开时"), qsTr("切回来时")]
                        current: window.editor.dialogAutoJumpWhen === "back" ? 1 : 0
                        onActivated: (index) => window.editor.dialogAutoJumpWhen = index === 1 ? "back" : "open"
                    }
                }

                SettingRow {
                    id: dialogBarAppsRow
                    shown: window.editor.dialogBarExcludedApps.length > 0
                    title: qsTr("不在这些程序的对话框下显示搜索框")
                    keywords: qsTr("排除;程序;对话框;exe").split(";")
                    values: window.editor.dialogBarExcludedApps
                    description: qsTr("在搜索框的“更多”菜单里添加")

                    body: Flow {
                        width: parent.width
                        spacing: 6

                        Repeater {
                            model: window.editor.dialogBarExcludedApps

                            delegate: Chip {
                                required property int index
                                required property string modelData
                                text: modelData
                                marked: dialogBarAppsRow.valueFound(modelData)
                                onRemoveClicked: window.editor.removeDialogBarExcludedApp(index)
                            }
                        }
                    }
                }
            }

            SettingsSection {
                page: 0
                tab: 3
                width: parent.width

                SettingRow {
                    title: qsTr("开机时自动启动")
                    keywords: qsTr("自启;开机启动;启动;autostart;startup").split(";")
                    description: qsTr("登录 Windows 后在后台运行")

                    ToggleSwitch {
                        checked: window.editor.autostart
                        onToggled: (on) => window.editor.autostart = on
                    }
                }

                SettingRow {
                    title: qsTr("记住打开过的项目")
                    keywords: qsTr("历史;最近;记录;history;recent").split(";")
                    description: qsTr("没输入时列出最近打开的项目；搜索时，你输入同样的字常打开的排在前面")

                    ToggleSwitch {
                        checked: window.editor.recordHistory
                        onToggled: (on) => window.editor.recordHistory = on
                    }
                }

                SettingRow {
                    title: qsTr("清除最近使用记录")
                    keywords: qsTr("历史;最近;删除;清空;history").split(";")
                    description: window.editor.historyCount > 0
                                 ? qsTr("共 %1 项；在搜索框里右键一项可以单独移除").arg(window.editor.historyCount)
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
                page: 1
                width: parent.width

                SettingRow {
                    title: qsTr("主题")
                    keywords: qsTr("跟随系统;浅色;深色;暗色;暗黑;夜间;黑色;白色;dark;light").split(";")
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
                    shown: SystemTheme.backdropSystem // not on Windows 10
                    title: qsTr("透明效果")
                    keywords: qsTr("云母;Mica;毛玻璃;亚克力;半透明;壁纸;transparency").split(";")
                    description: !SystemTheme.backdropAvailable
                                 ? qsTr("需要把“高级”里的界面绘制方式设为“显卡加速”")
                                 : !SystemTheme.materials
                                 ? qsTr("要先打开 Windows 设置里的“透明效果”")
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
                    title: qsTr("圆角", "appearance setting")
                    keywords: qsTr("圆角;直角;方角;尖角;Win10;Windows 10;win10风格;经典;corner;rounded;square").split(";")
                    description: qsTr("直角时窗口、菜单、按钮和输入框都不带圆角，和 Windows 10 一样")

                    ScopeTabs {
                        readonly property var corners: ["round", "square"]
                        labels: [qsTr("圆角"), qsTr("直角")]
                        current: Math.max(0, corners.indexOf(window.editor.corners))
                        onActivated: (index) => window.editor.corners = corners[index]
                    }
                }

                SettingRow {
                    title: qsTr("界面大小")
                    keywords: qsTr("缩放;大小;字号;字体大小;行高;紧凑;宽松;scale;size;zoom;font").split(";")
                    description: qsTr("搜索框、剪贴板和对话框旁搜索框的文字和行高；设置窗口不变")

                    ScopeTabs {
                        readonly property var sizes: ["compact", "standard", "roomy"]
                        labels: [qsTr("紧凑"), qsTr("标准"), qsTr("宽松")]
                        current: Math.max(0, sizes.indexOf(window.editor.uiSize))
                        onActivated: (index) => window.editor.uiSize = sizes[index]
                    }

                    body: AppearancePreview { width: parent.width }
                }

                SettingRow {
                    title: qsTr("搜索框宽度")
                    keywords: qsTr("宽度;加宽;拉宽;调宽;变宽;窄;文件名太长;显示不全;width;wide").split(";")
                    description: qsTr("拖动搜索框的左边或右边调宽，名字长也看得全；双击边缘恢复默认。名字仍放不下时，鼠标停在那一行上会显示完整的名字和位置")

                    FlatButton {
                        text: qsTr("恢复默认宽度")
                        enabled: window.editor.launcherWidthSet
                        onClicked: window.editor.resetLauncherWidth()
                    }
                }

                SettingRow {
                    title: qsTr("任务栏图标")
                    keywords: qsTr("托盘;通知区域;单色;黑白;彩色;图标;tray;icon").split(";")
                    description: qsTr("通知区域和任务栏搜索框里的图标。单色随任务栏的浅色、深色变成黑或白，和系统图标一致")

                    ScopeTabs {
                        labels: [qsTr("彩色"), qsTr("单色")]
                        current: window.editor.taskbarIcon === "mono" ? 1 : 0
                        onActivated: (index) => window.editor.taskbarIcon = index === 1 ? "mono" : "color"
                    }
                }

                SettingRow {
                    title: qsTr("搜索框显示 Logo")
                    keywords: qsTr("Logo;图标;标志;隐藏;简洁;搜索框上面的图标;logo").split(";")
                    description: qsTr("搜索框、剪贴板和对话框旁搜索框的边上探出来的 Logo。关掉后更简洁")

                    ToggleSwitch {
                        checked: window.editor.searchLogo
                        onToggled: (on) => window.editor.searchLogo = on
                    }
                }

                SettingRow {
                    title: qsTr("语言")
                    keywords: qsTr("中文;英文;简体;language").split(";")
                    description: qsTr("“跟随系统”时按 Windows 的语言显示中文或英文")

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
                page: 2
                width: parent.width
                note: qsTr("修改后在后台重新整理，不影响搜索。")

                SettingRow {
                    id: indexScopeRow

                    // 索引哪些地方 (GitHub #52): the chosen folders only while the
                    // disks are walked; with the index service or 管理员直读 they
                    // are read whole, and the third choice waits.
                    readonly property var scopes: ["auto", "drives", "folders"]

                    title: qsTr("索引哪些地方")
                    stackControl: width - 52 - indexScopeTabs.implicitWidth < 240
                    keywords: qsTr("索引范围;范围;整个磁盘;选中的文件夹;工作文件夹;普通用户;没有管理员权限;scope").split(";")
                    description: {
                        let what
                        switch (window.editor.indexScope) {
                        case "drives":
                            what = qsTr("索引所有固定磁盘上的文件")
                            break
                        case "folders":
                            what = qsTr("只索引下面选中的文件夹，建索引和核对都快得多")
                            break
                        default:
                            what = qsTr("没有索引服务、也没有以管理员身份运行时只索引选中的文件夹，否则索引整个磁盘")
                        }
                        return what + "\n" + window.editor.indexScopeStatus
                    }

                    ScopeTabs {
                        id: indexScopeTabs
                        labels: [qsTr("自动"), qsTr("整个磁盘"), qsTr("只索引选中的文件夹")]
                        current: Math.max(0, indexScopeRow.scopes.indexOf(window.editor.indexScope))
                        disabled: window.editor.foldersPossible ? [] : [2]
                        onActivated: (index) => window.editor.indexScope = indexScopeRow.scopes[index]
                    }
                }

                SettingRow {
                    id: foldersRow

                    // Why the last one could not be added, and the network place
                    // it was (offered for 网络共享 instead).
                    property string problem
                    property string networkPath

                    function add(text) {
                        const path = window.editor.localPath(text)
                        const problem = window.editor.addFolder(path)
                        foldersRow.problem = problem
                        foldersRow.networkPath = problem.length > 0 && window.editor.isNetworkFolder(path) ? path : ""
                        return problem.length === 0
                    }
                    function addShare(path) {
                        const problem = window.editor.addShare(path)
                        foldersRow.problem = problem
                        foldersRow.networkPath = ""
                        if (problem.length === 0)
                            window.showPage(2, 3) // 网络共享, where it is now
                    }

                    title: qsTr("索引的文件夹")
                    keywords: qsTr("文件夹;添加文件夹;工作文件夹;桌面;文档;下载;立即核对;folders").split(";")
                    values: window.editor.folders.map(folder => folder.path)
                    description: window.editor.foldersInEffect
                                 ? qsTr("只有这些文件夹里的文件能搜到；也可以从资源管理器把文件夹拖到这里")
                                 : qsTr("普通文件夹已包含在整盘索引中；mklink 创建的链接文件夹可在这里额外添加，管理员和索引服务模式也能用。")

                    FlatButton {
                        text: qsTr("添加文件夹…")
                        glyph: "" // Add
                        onClicked: {
                            const path = window.editor.pickFolder(qsTr("选择要索引的文件夹"))
                            if (path.length > 0)
                                foldersRow.add(path)
                        }
                    }

                    body: [
                        Text {
                            visible: window.editor.foldersInEffect && window.editor.folders.length === 0
                            width: parent.width
                            text: qsTr("还没有选文件夹：本机磁盘上的文件都搜不到")
                            color: Theme.caution
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        },
                        Row {
                            visible: foldersRow.problem.length > 0
                            width: parent.width
                            spacing: 8

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.min(implicitWidth, parent.width - (toShares.visible ? toShares.width + 8 : 0))
                                text: foldersRow.problem
                                textFormat: Text.PlainText
                                color: Theme.danger
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                            FlatButton {
                                id: toShares
                                visible: foldersRow.networkPath.length > 0
                                text: qsTr("加到网络共享")
                                onClicked: foldersRow.addShare(foldersRow.networkPath)
                            }
                        },
                        Column { // the known folders not chosen yet, a click away
                            visible: window.editor.foldersInEffect && window.editor.commonFolders.length > 0
                            width: parent.width
                            spacing: 6

                            Text {
                                text: qsTr("常用位置，点一下添加：")
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                            }
                            Flow {
                                width: parent.width
                                spacing: 6

                                Repeater {
                                    model: window.editor.commonFolders

                                    delegate: FlatButton {
                                        required property var modelData
                                        glyph: "" // Add
                                        // Redirected to a server, as companies do: 网络共享's.
                                        text: modelData.network ? qsTr("%1（在服务器上：加到网络共享）").arg(modelData.name) : modelData.name
                                        onClicked: modelData.network ? foldersRow.addShare(modelData.path) : foldersRow.add(modelData.path)
                                    }
                                }
                            }
                        },
                        Column {
                            width: parent.width
                            spacing: 8

                            Repeater {
                                // By count: a card stays as its words change, while it is walked.
                                model: window.editor.folders.length

                                delegate: ShareCard {
                                    required property int index
                                    width: parent.width
                                    kind: "folder"
                                    share: window.editor.folders[index] ?? blank // gone, before the card goes
                                    marked: foldersRow.valueFound(share.path)
                                    onVerifyClicked: window.editor.verifyFolder(index)
                                    onRemoveClicked: {
                                        foldersRow.problem = ""
                                        window.editor.removeFolder(index)
                                    }
                                }
                            }
                        }
                    ]
                }

                SettingRow {
                    id: folderVerifyRow

                    readonly property var hours: [1, 4, 24, 0]

                    shown: window.editor.foldersInEffect || window.editor.folders.some(folder => folder.linked)
                    title: qsTr("定时核对")
                    keywords: qsTr("核对;重新扫描;每天;每小时;定时;verify;rescan").split(";")
                    description: qsTr("把选中的文件夹重新走一遍，补上没跟上的改动；到时间后等你离开电脑一会儿再核对")
                    bodyShown: folderVerifyRow.hours.indexOf(window.editor.folderVerifyHours) < 0

                    ScopeTabs {
                        labels: [qsTr("每小时"), qsTr("每 4 小时"), qsTr("每天"), qsTr("只在启动时")]
                        current: folderVerifyRow.hours.indexOf(window.editor.folderVerifyHours)
                        onActivated: (index) => window.editor.folderVerifyHours = folderVerifyRow.hours[index]
                    }

                    body: Text { // another number, written in the file (or by AI)
                        width: parent.width
                        text: qsTr("现在是每 %n 小时一次", "", window.editor.folderVerifyHours)
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                    }
                }

                SettingRow {
                    title: qsTr("不搜索的文件夹")
                    keywords: qsTr("排除;忽略;隐藏;黑名单;exclude").split(";")
                    values: window.editor.excludedPaths
                    description: qsTr("这些文件夹里的内容都不出现在结果里")

                    FlatButton {
                        text: qsTr("添加文件夹")
                        glyph: "" // Add
                        onClicked: window.editor.addExcludedFolder()
                    }

                    body: Rectangle {
                        width: parent.width
                        height: Math.max(pathList.implicitHeight, 40)
                        radius: Theme.controlRadius
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
                                        radius: Theme.smallControlRadius
                                        color: rowArea.containsMouse ? Theme.navHover : "transparent"
                                    }
                                    Rectangle {
                                        visible: pathRow.index > 0
                                        x: 12
                                        width: parent.width - 24
                                        height: Theme.edge(Screen.devicePixelRatio)
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
            }

            SettingsSection {
                page: 2
                tab: 1
                width: parent.width
                note: qsTr("修改后在后台重新整理，不影响搜索。")

                SettingRow {
                    id: excludedNamesRow
                    title: qsTr("跳过的文件夹名称")
                    keywords: qsTr("排除;忽略;文件夹名;exclude").split(";")
                    values: window.editor.excludedNames
                    description: qsTr("任何位置的同名文件夹都跳过，例如 node_modules")

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
                                    marked: excludedNamesRow.valueFound(modelData)
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
            }

            SettingsSection {
                page: 2
                tab: 1
                width: parent.width
                note: qsTr("立即生效，不用重新整理。")

                SettingRow {
                    id: excludedTypesRow
                    title: qsTr("跳过的文件类型")
                    keywords: qsTr("排除;忽略;扩展名;后缀;格式;exclude").split(";")
                    values: window.editor.excludedTypes
                    description: qsTr("这些扩展名的文件不出现在搜索结果里，例如 wbk、bak")

                    body: [
                        Flow {
                            width: parent.width
                            spacing: 6

                            Repeater {
                                model: window.editor.excludedTypes

                                delegate: Chip {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    marked: excludedTypesRow.valueFound(modelData)
                                    onRemoveClicked: window.editor.removeExcludedType(index)
                                }
                            }
                        },
                        Row {
                            spacing: 8

                            InputBox {
                                id: typeInput
                                width: 240
                                placeholder: qsTr("例如 wbk, bak")
                                onAccepted: if (window.editor.addExcludedTypes(text)) clear()
                            }
                            FlatButton {
                                text: qsTr("添加")
                                enabled: typeInput.text.trim().length > 0
                                onClicked: if (window.editor.addExcludedTypes(typeInput.text)) typeInput.clear()
                            }
                        }
                    ]
                }

                SettingRow {
                    id: excludedFileNamesRow
                    title: qsTr("跳过的文件名")
                    keywords: qsTr("排除;忽略;文件名;通配符;临时文件;~$;exclude").split(";")
                    values: window.editor.excludedFileNames
                    description: qsTr("这些名字的文件不出现在结果里，* 代表任意几个字，例如 ~$*（Office、WPS 临时文件）")

                    body: [
                        Flow {
                            width: parent.width
                            spacing: 6

                            Repeater {
                                model: window.editor.excludedFileNames

                                delegate: Chip {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    marked: excludedFileNamesRow.valueFound(modelData)
                                    onRemoveClicked: window.editor.removeExcludedFileName(index)
                                }
                            }
                        },
                        Row {
                            spacing: 8

                            InputBox {
                                id: fileNameInput
                                width: 240
                                placeholder: qsTr("例如 ~$*, Thumbs.db")
                                onAccepted: if (window.editor.addExcludedFileNames(text)) clear()
                            }
                            FlatButton {
                                text: qsTr("添加")
                                enabled: fileNameInput.text.trim().length > 0
                                onClicked: if (window.editor.addExcludedFileNames(fileNameInput.text)) fileNameInput.clear()
                            }
                        }
                    ]
                }
            }

            SettingsSection {
                page: 2
                tab: 2
                width: parent.width
                note: qsTr("修改后在后台重新整理，不影响搜索。")

                SettingRow {
                    id: drivesRow
                    title: qsTr("索引哪些磁盘")
                    stackControl: width - 52 - driveScopeTabs.implicitWidth < 240
                    keywords: qsTr("磁盘;硬盘;盘符;选择磁盘;索引范围;drive;disk;volume").split(";")
                    description: window.editor.allDrives
                                 ? qsTr("自动索引所有符合条件的本机磁盘")
                                 : qsTr("只索引勾选的磁盘；取消勾选后，该盘的文件会从结果中移除。新接入的磁盘需要手动勾选。")
                    bodyShown: !window.editor.allDrives
                    ScopeTabs {
                        id: driveScopeTabs
                        labels: [qsTr("全部磁盘"), qsTr("自己选择")]
                        current: window.editor.allDrives ? 0 : 1
                        onActivated: index => window.editor.allDrives = index === 0
                    }
                    body: [
                        Column {
                            width: parent.width
                            spacing: 6
                            Repeater {
                                model: window.editor.drives
                                delegate: Row {
                                    id: driveRow
                                    required property var modelData
                                    width: parent.width
                                    spacing: 12
                                    ToggleSwitch {
                                        id: driveToggle
                                        word: false
                                        checked: driveRow.modelData.selected
                                        enabled: driveRow.modelData.enabled
                                        Accessible.name: qsTr("索引 %1 盘").arg(driveRow.modelData.root)
                                        onToggled: on => window.editor.setDriveSelected(driveRow.modelData.root, on)
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.max(0, driveRow.width - driveToggle.width - driveRow.spacing)
                                        wrapMode: Text.Wrap
                                        text: driveRow.modelData.root + (driveRow.modelData.status.length > 0 ? "  ·  " + driveRow.modelData.status : "")
                                        font.pixelSize: Theme.fontBody
                                        color: driveRow.modelData.enabled ? Theme.text : Theme.subtext
                                    }
                                }
                            }
                            Text {
                                visible: !window.editor.drives.some(drive => drive.selected && drive.enabled)
                                width: parent.width
                                text: qsTr("没有选择本机磁盘，只搜索你添加的网络和 WSL 位置")
                                wrapMode: Text.Wrap
                                color: Theme.caution
                                font.pixelSize: Theme.fontCaption
                            }
                        },
                        FlatButton {
                            text: qsTr("刷新磁盘列表")
                            onClicked: window.editor.refreshDrives()
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("包括 U 盘和移动硬盘")
                    keywords: qsTr("U盘;移动硬盘;外接;USB;removable").split(";")
                    description: qsTr("默认只搜索电脑自带的硬盘")

                    ToggleSwitch {
                        checked: window.editor.includeRemovableDrives
                        onToggled: (on) => window.editor.includeRemovableDrives = on
                    }
                }

                SettingRow {
                    title: qsTr("启动时检查文件变化")
                    keywords: qsTr("重新扫描;扫描;rescan").split(";")
                    description: qsTr("补上 U 盘等非 NTFS 磁盘在 Win顺 关闭期间的改动")

                    ToggleSwitch {
                        checked: window.editor.rescanOnStartup
                        onToggled: (on) => window.editor.rescanOnStartup = on
                    }
                }

                SettingRow {
                    id: indexAccessRow

                    // How NTFS disks are read (GitHub #21): the choices, 管理员直读
                    // only on an administrator's account (it means nothing elsewhere).
                    readonly property var modes: window.editor.isAdministrator
                                                 ? ["auto", "service", "direct", "walk"]
                                                 : ["auto", "service", "walk"]
                    readonly property var modeLabels: ({
                        "auto": qsTr("自动（推荐）"),
                        "service": qsTr("索引服务"),
                        "direct": qsTr("管理员直读"),
                        "walk": qsTr("遍历")
                    })
                    readonly property string chosen: window.editor.indexAccess

                    title: qsTr("索引方式")
                    keywords: qsTr("索引服务;管理员;权限;UAC;MFT;主文件表;遍历;服务;index service;administrator").split(";")
                    description: {
                        let what
                        switch (chosen) {
                        case "service":
                            what = qsTr("通过索引服务读取磁盘的主文件表和变更日志，Win顺 以普通权限运行")
                            break
                        case "direct":
                            what = qsTr("Win顺 以管理员身份运行，自己读取主文件表和变更日志；开机自启也以管理员身份")
                            break
                        case "walk":
                            what = qsTr("不需要任何权限，逐个文件夹遍历，建索引慢一些")
                            break
                        default:
                            what = qsTr("以管理员身份运行时直接读取，否则通过索引服务；没有服务就遍历文件夹")
                        }
                        return what + "\n" + window.editor.indexAccessStatus
                    }

                    FlatButton {
                        // 管理员直读 asked for while running with normal rights, or a
                        // portable copy wanting the MFT read this once: UAC asks once.
                        visible: window.editor.isAdministrator && !window.editor.isElevated
                                 && (window.editor.isPortable || indexAccessRow.chosen === "direct")
                        text: qsTr("以管理员身份重新启动")
                        onClicked: window.editor.restartAsAdministrator()
                    }

                    body: [
                        ScopeTabs {
                            labels: indexAccessRow.modes.map(mode => indexAccessRow.modeLabels[mode])
                            current: Math.max(0, indexAccessRow.modes.indexOf(indexAccessRow.chosen))
                            onActivated: (index) => window.editor.indexAccess = indexAccessRow.modes[index]
                        },
                        Text { // worth reading, nothing wrong
                            visible: window.editor.indexAccessNote.length > 0
                            width: parent.width
                            text: window.editor.indexAccessNote
                            textFormat: Text.PlainText
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    ]
                }

                SettingRow {
                    id: indexServiceRow

                    // A portable copy has no installer to register the service:
                    // done from here, as administrator (UAC asks once).
                    // An account that cannot install it (not an administrator's,
                    // GitHub #52) gets the reason instead of the buttons.
                    shown: window.editor.isPortable
                    title: qsTr("便携版的索引服务")
                    keywords: qsTr("便携;安装服务;卸载服务;索引服务;没有管理员权限;装不了;portable;service").split(";")
                    description: window.editor.indexServiceInstalled && window.editor.indexServiceUnsafeLocation
                                 ? (window.editor.indexServiceInstallable
                                    ? qsTr("索引服务装在一个别的程序能改动的位置，有安全风险。点“修复”把它移到受保护的位置（需要管理员权限一次）")
                                    : qsTr("索引服务装在一个别的程序能改动的位置，有安全风险。请让管理员打开 Win顺 的设置，点“修复”。"))
                                 : !window.editor.indexServiceInstallable
                                 ? (window.editor.indexServiceInstalled
                                    ? qsTr("已安装 Windows 服务“Win顺 索引服务”，它替普通权限的 Win顺 读取主文件表。这个账户不是管理员，不能卸载它")
                                    : window.editor.foldersInEffect
                                    ? qsTr("你在这台电脑上没有管理员权限，装不了索引服务。Win顺 会索引你选的文件夹，并定时核对")
                                    : qsTr("你在这台电脑上没有管理员权限，装不了索引服务。Win顺 按文件夹遍历整个磁盘"))
                                 : window.editor.indexServiceInstalled
                                 ? qsTr("已安装 Windows 服务“Win顺 索引服务”，它替普通权限的 Win顺 读取主文件表")
                                 : qsTr("安装一个 Windows 服务，替普通权限的 Win顺 读取主文件表，建索引快得多；需要管理员权限一次")
                    bodyShown: window.editor.indexServiceMessage.length > 0

                    FlatButton {
                        // Install, or, when the service is in a place a non-administrator
                        // could change, repair it (reinstall into a protected folder).
                        visible: window.editor.indexServiceInstallable
                                 && (!window.editor.indexServiceInstalled || window.editor.indexServiceUnsafeLocation)
                        text: window.editor.indexServiceInstalled && window.editor.indexServiceUnsafeLocation
                              ? qsTr("修复") : qsTr("安装索引服务…")
                        enabled: !window.editor.indexServiceBusy
                        onClicked: window.editor.installIndexService()
                    }
                    FlatButton {
                        visible: window.editor.indexServiceInstallable && window.editor.indexServiceInstalled
                                 && !window.editor.indexServiceUnsafeLocation
                        text: qsTr("卸载索引服务")
                        enabled: !window.editor.indexServiceBusy
                        onClicked: window.editor.uninstallIndexService()
                    }

                    body: Text {
                        width: parent.width
                        text: window.editor.indexServiceMessage
                        textFormat: Text.PlainText
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
            }

            SettingsSection {
                page: 2
                tab: 3
                width: parent.width
                note: qsTr("默认只收录文件名和文件夹名；打开“搜索文件内容”的，会在电脑空闲时读取文件内容。服务器离线或 WSL 停止时，收录过的照样能搜到；恢复后自动更新。")

                SettingRow {
                    id: sharesRow
                    title: qsTr("网络与 WSL 文件夹")
                    keywords: qsTr("网络;共享;局域网;内网;NAS;服务器;SMB;映射;网络驱动器;WSL;Linux;Ubuntu;子系统;WebDAV;内容;全文;搜索文件内容;network;share;content").split(";")
                    values: window.editor.shares.map(share => share.path + " " + share.name)
                    description: qsTr("添加 NAS、共享文件夹或 WSL 中的项目、用户目录，和本机文件一起搜索")

                    // The shares of the server typed ("\\192.168.1.10"), once asked for.
                    property string listedServer
                    property var listedShares: []
                    property string listProblem
                    property bool listSignIn: false
                    property bool listing: false
                    readonly property string typedServer: window.editor.serverOf(shareInput.text)
                    readonly property bool typedWsl: window.editor.isWslPath(shareInput.text)

                    function add(text) {
                        const problem = window.editor.addShare(text)
                        shareProblem.text = problem
                        if (problem.length === 0 && text === shareInput.text)
                            shareInput.clear()
                        return problem.length === 0
                    }
                    function listShares(server) {
                        listedServer = server
                        listedShares = []
                        listProblem = ""
                        listSignIn = false
                        listing = true
                        window.editor.listServerShares(server)
                    }

                    Connections {
                        target: window.editor
                        function onServerSharesListed(server, shares, problem, signIn) {
                            if (server.toLowerCase() !== sharesRow.listedServer.toLowerCase())
                                return
                            sharesRow.listing = false
                            sharesRow.listedShares = shares
                            sharesRow.listProblem = problem
                            sharesRow.listSignIn = signIn
                        }
                        function onSignedIn(path, problem) {
                            if (problem.length > 0) {
                                shareProblem.text = problem
                                return
                            }
                            shareProblem.text = ""
                            if (sharesRow.listSignIn && path.toLowerCase() === "\\\\" + sharesRow.listedServer.toLowerCase())
                                sharesRow.listShares(sharesRow.listedServer) // signed in: now its shares show
                        }
                    }

                    body: [
                        Flow {
                            width: parent.width
                            spacing: 8

                            InputBox {
                                id: shareInput
                                width: Math.min(360, parent.width) // room for the placeholder
                                placeholder: qsTr("粘贴网络路径、WebDAV 网址或 WSL 文件夹")
                                onAccepted: {
                                    if (sharesRow.typedServer.length > 0)
                                        sharesRow.listShares(sharesRow.typedServer) // a server alone: what it shares
                                    else
                                        sharesRow.add(text)
                                }
                                onTextChanged: shareProblem.text = ""
                            }
                            FlatButton {
                                visible: sharesRow.typedServer.length === 0
                                text: qsTr("添加")
                                enabled: shareInput.text.trim().length > 0
                                onClicked: sharesRow.add(shareInput.text)
                            }
                            FlatButton {
                                visible: sharesRow.typedServer.length > 0
                                text: sharesRow.typedWsl ? qsTr("查看 WSL 发行版") : qsTr("查看共享的文件夹")
                                onClicked: sharesRow.listShares(sharesRow.typedServer)
                            }
                            FlatButton {
                                text: qsTr("选择文件夹…")
                                onClicked: {
                                    const path = window.editor.pickFolder(qsTr("选择要搜索的网络或 WSL 文件夹"))
                                    if (path.length > 0)
                                        sharesRow.add(path)
                                }
                            }
                        },
                        Text {
                            id: shareProblem
                            visible: text.length > 0
                            width: parent.width
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        },
                        Column { // what the server listed shares, to add with a click
                            visible: sharesRow.listedServer.length > 0
                            width: parent.width
                            spacing: 6

                            Text {
                                width: parent.width
                                text: sharesRow.listing ? qsTr("正在连接 %1…").arg(sharesRow.listedServer)
                                    : sharesRow.listProblem.length > 0 ? sharesRow.listProblem
                                    : window.editor.isWslPath("\\\\" + sharesRow.listedServer)
                                      ? qsTr("已安装的 WSL 发行版，点一下收录 Linux 文件：")
                                      : qsTr("%1 上共享的文件夹，点一下添加：").arg(sharesRow.listedServer)
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                            Flow {
                                width: parent.width
                                spacing: 6

                                FlatButton {
                                    visible: sharesRow.listSignIn
                                    highlighted: true
                                    text: qsTr("登录")
                                    onClicked: window.editor.signInToServer(sharesRow.listedServer)
                                }
                                Repeater {
                                    model: sharesRow.listedShares

                                    delegate: FlatButton {
                                        required property string modelData
                                        readonly property string path: "\\\\" + sharesRow.listedServer + "\\" + modelData
                                        readonly property bool added: window.editor.shares.some(
                                                                          share => share.path.toLowerCase() === path.toLowerCase())
                                        glyph: added ? "" : "" // CheckMark, Add
                                        text: modelData
                                        enabled: !added
                                        onClicked: sharesRow.add(path)
                                    }
                                }
                            }
                        },
                        Column {
                            visible: window.editor.wslFolders.length > 0
                            width: parent.width
                            spacing: 6

                            Text {
                                width: parent.width
                                text: qsTr("检测到 WSL，点一下添加用户目录；也可以选择具体的项目文件夹：")
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                            Flow {
                                width: parent.width
                                spacing: 6

                                Repeater {
                                    model: window.editor.wslFolders

                                    delegate: FlatButton {
                                        required property var modelData
                                        glyph: "\uE710"
                                        text: modelData.name
                                        onClicked: sharesRow.add(modelData.path)
                                    }
                                }
                            }
                        },
                        Column { // opened in File Explorer or mapped before: a click away
                            visible: window.editor.knownShares.length > 0 && sharesRow.listedServer.length === 0
                            width: parent.width
                            spacing: 6

                            Text {
                                text: qsTr("这台电脑访问过的：")
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                            }
                            Flow {
                                width: parent.width
                                spacing: 6

                                Repeater {
                                    model: window.editor.knownShares

                                    delegate: FlatButton {
                                        required property var modelData
                                        glyph: "" // Add
                                        text: modelData.drive.length > 0 ? modelData.path + " (" + modelData.drive + ")" : modelData.path
                                        onClicked: sharesRow.add(modelData.path)
                                    }
                                }
                            }
                        },
                        Column {
                            width: parent.width
                            spacing: 8

                            Repeater {
                                // By count: a card stays as its words change, every few seconds while it is read.
                                model: window.editor.shares.length

                                delegate: ShareCard {
                                    required property int index
                                    width: parent.width
                                    share: window.editor.shares[index] ?? blank // gone, before the card goes
                                    marked: sharesRow.valueFound(share.path)
                                    onVerifyClicked: window.editor.verifyShare(index)
                                    onStartClicked: window.editor.startWslShare(index)
                                    onSignInClicked: window.editor.signInToShare(index)
                                    onAcceptClicked: window.editor.acceptShareMissing(index)
                                    onRemoveClicked: window.editor.removeShare(index)
                                    onContentToggled: (on) => window.editor.setShareContent(index, on)
                                }
                            }
                        }
                    ]
                }
            }

            SettingsSection {
                page: 3
                width: parent.width

                SettingRow {
                    title: qsTr("文件和文件夹的顺序")
                    keywords: qsTr("排序;顺序;文件夹在前;目录;sort;folder").split(";")
                    description: qsTr("文件和文件夹分两组排，选哪组在前")

                    ScopeTabs {
                        labels: [qsTr("文件在前"), qsTr("文件夹在前")]
                        current: window.editor.foldersFirst ? 1 : 0
                        onActivated: (index) => window.editor.foldersFirst = index === 1
                    }
                }

                SettingRow {
                    id: sortRow
                    title: qsTr("排序")
                    keywords: qsTr("排序;修改时间;修改日期;最新;最早;名称;文件名;所在文件夹;目录;类型;扩展名;大小;升序;降序;sort;date;size;type").split(";")
                    description: qsTr("文件和文件夹怎么排（“全部”里应用和设置仍在前面）；底栏左边也能切换（Ctrl+S 在前三种里轮换）")

                    readonly property var sorts: ["match", "newest", "oldest", "name", "folder", "type", "largest", "smallest"]
                    readonly property int chosen: sorts.indexOf(window.editor.sort)

                    ScopeTabs {
                        labels: [qsTr("最匹配"), qsTr("最新在前"), qsTr("最早在前")]
                        current: sortRow.chosen < 3 ? Math.max(0, sortRow.chosen) : -1
                        onActivated: (index) => window.editor.sort = sortRow.sorts[index]
                    }

                    // Too many for one line (#44): the other orders under the first.
                    body: [
                        ScopeTabs {
                            labels: [qsTr("名称"), qsTr("所在文件夹"), qsTr("类型"), qsTr("大的在前"), qsTr("小的在前")]
                            current: sortRow.chosen - 3 // below 0 while one of the three above is in force
                            onActivated: (index) => window.editor.sort = sortRow.sorts[index + 3]
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("优先显示的文件类型")
                    keywords: qsTr("可执行;程序;脚本;优先;排在前面;扩展名;exe;bat;cmd;ps1;msi;Listary").split(";")
                    values: [window.editor.preferredTypes]
                    description: qsTr("按“最匹配”排序时，名字差不多的结果里这些类型排在前面，例如搜 curl 时 curl.exe 排在文件夹 curl 和 curl.h 前面。用分号或逗号隔开，留空就不优先")

                    FlatButton {
                        visible: !window.editor.preferredTypesDefault
                        text: qsTr("恢复默认")
                        onClicked: window.editor.resetPreferredTypes()
                    }

                    body: [
                        InputBox {
                            width: Math.min(parent.width, 360)
                            text: window.editor.preferredTypes
                            placeholder: qsTr("例如 exe; bat; cmd")
                            onEditingFinished: {
                                window.editor.preferredTypes = text
                                text = Qt.binding(() => window.editor.preferredTypes) // as kept: "EXE,.bat" shows "exe; bat"
                            }
                        }
                    ]
                }
            }

            SettingsSection {
                page: 3
                tab: 1
                width: parent.width

                SettingRow {
                    title: qsTr("不输入时显示")
                    keywords: qsTr("最近;最近打开;最近修改;最近使用;历史;recent").split(";")
                    description: qsTr("打开搜索框还没输入时列出什么：最近打开的 5 项，或最近修改的文件（往下滚动接着列出）；底栏也能切换（Ctrl+S）。选“不显示”只列收藏")

                    ScopeTabs {
                        readonly property var lists: ["opened", "modified", "none"]
                        labels: [qsTr("最近打开"), qsTr("最近修改"), qsTr("不显示")]
                        current: Math.max(0, lists.indexOf(window.editor.startList))
                        onActivated: (index) => window.editor.startList = lists[index]
                    }
                }

                SettingRow {
                    title: qsTr("点击结果")
                    keywords: qsTr("单击;双击;点击;鼠标;误触;打开;选中;click;double").split(";")
                    description: qsTr("单击就打开；或者单击只选中（高亮），双击或按 Enter 才打开，结果多时不怕点错")

                    ScopeTabs {
                        labels: [qsTr("单击打开"), qsTr("双击打开")]
                        current: window.editor.doubleClickToOpen ? 1 : 0
                        onActivated: (index) => window.editor.doubleClickToOpen = index === 1
                    }
                }

                SettingRow {
                    objectName: "favoriteFolderTab"
                    title: qsTr("选中收藏文件夹时按 Tab")
                    keywords: qsTr("Tab;Shift+Tab;收藏;文件夹;范围;键盘;favorite;keyboard").split(";")
                    description: qsTr("在“全部”范围中，默认切换搜索范围；也可进入选中的收藏文件夹继续搜索。Shift+Tab 仍反向切换范围。")

                    ScopeTabs {
                        objectName: "favoriteFolderTabChoice"
                        labels: [qsTr("切换范围"), qsTr("进入文件夹")]
                        current: window.editor.tabEntersFavoriteFolder ? 1 : 0
                        onActivated: (index) => window.editor.tabEntersFavoriteFolder = index === 1
                    }
                }

                SettingRow {
                    title: qsTr("显示修改日期")
                    keywords: qsTr("修改时间;修改日期;日期;时间;date").split(";")
                    description: qsTr("在搜索结果右侧显示修改日期；可与文件大小同时显示")

                    ToggleSwitch {
                        checked: window.editor.showModified
                        onToggled: (on) => window.editor.showModified = on
                    }
                    body: [
                        Row {
                            spacing: 10
                            ToggleSwitch {
                                checked: window.editor.showModifiedTime
                                enabled: window.editor.showModified
                                Accessible.name: qsTr("较早的日期也显示时间")
                                onToggled: (on) => window.editor.showModifiedTime = on
                            }
                            Text {
                                text: qsTr("较早的日期也显示时间")
                                color: window.editor.showModified ? Theme.text : Theme.faint
                                font.pixelSize: Theme.fontBody
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("显示文件大小")
                    keywords: qsTr("文件大小;体积;精简;双击Ctrl;KB;MB;GB;size").split(";")
                    description: qsTr("无需展开窗口，在搜索结果右侧显示文件大小；按大小排序时始终显示。文件夹不统计大小。")

                    ToggleSwitch {
                        objectName: "showFileSizeToggle"
                        checked: window.editor.showFileSize
                        Accessible.name: qsTr("显示文件大小")
                        onToggled: (on) => window.editor.showFileSize = on
                    }
                }

                SettingRow {
                    title: qsTr("显示图片缩略图")
                    keywords: qsTr("缩略图;预览;图片;照片;视频;PDF;thumbnail;preview").split(";")
                    description: qsTr("图片、视频、PDF 等显示缩略图，代替类型图标")

                    ToggleSwitch {
                        checked: window.editor.thumbnails
                        onToggled: (on) => window.editor.thumbnails = on
                    }
                }

                SettingRow {
                    title: qsTr("预览文件（Alt+P）")
                    keywords: qsTr("预览;快速查看;看一眼;QuickLook;Seer;空格;Alt+P;preview").split(";")
                    description: window.editor.previewWith === "builtin"
                                 ? qsTr("在结果上按 Alt+P，用 Win顺 自己的预览窗口看一眼")
                                 : qsTr("在结果上按 Alt+P 看一眼；QuickLook 或 Seer 在后台运行时交给它，否则用 Win顺 自己的预览")

                    ScopeTabs {
                        labels: [qsTr("自动"), qsTr("内置")]
                        current: window.editor.previewWith === "builtin" ? 1 : 0
                        onActivated: (index) => window.editor.previewWith = ["auto", "builtin"][index]
                    }
                }
            }

            SettingsSection {
                page: 3
                tab: 2
                width: parent.width
                note: qsTr("在搜索框按 Tab、Shift+Tab 切换“全部”“文件”“内容”；Ctrl+Tab 在“文件”里切换文件类型，在“内容”里切换文件夹。")

                SettingRow {
                    title: qsTr("每次打开时清空搜索")
                    keywords: qsTr("清空;清除;上次;保留;隐私;搜索框;clear;privacy").split(";")
                    description: qsTr("打开时搜索框是空的，回到“全部”；关掉时保留上次的文字并选中，直接输入就会替换")

                    ToggleSwitch {
                        checked: window.editor.clearOnOpen
                        onToggled: (on) => window.editor.clearOnOpen = on
                    }
                }

                SettingRow {
                    title: qsTr("用数字键直接打开")
                    unlocks: [resultNumberHintsRow]
                    keywords: qsTr("快捷键;数字键;序号;Ctrl+1;Alt+1;冲突;hotkey").split(";")
                    description: qsTr("按 %1+1 打开第一行，依此类推到第九行")
                                 .arg(window.editor.numberKeys === "alt" ? "Alt" : "Ctrl")

                    ScopeTabs {
                        labels: ["Ctrl+1–9", "Alt+1–9", qsTr("关闭")]
                        current: Math.max(0, ["ctrl", "alt", "off"].indexOf(window.editor.numberKeys))
                        onActivated: (index) => window.editor.numberKeys = ["ctrl", "alt", "off"][index]
                    }

                    body: Text {
                        visible: window.editor.numberKeysNote.length > 0
                        width: parent.width
                        text: window.editor.numberKeysNote
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    id: resultNumberHintsRow
                    shown: window.editor.numberKeys !== "off"
                    title: qsTr("序号提示")
                    description: qsTr("启动器和剪贴板共用；隐藏提示不影响数字快捷键")
                    keywords: qsTr("快捷键;数字键;图标;按住;显示;隐藏").split(";")

                    ScopeTabs {
                        labels: [qsTr("按住时显示"), qsTr("一直显示"), qsTr("不显示", "number hints")]
                        current: Math.max(0, ["held", "always", "off"].indexOf(window.editor.numberHints))
                        onActivated: (index) => window.editor.numberHints = ["held", "always", "off"][index]
                    }
                }
            }

            FileTypesSection {
                page: 3
                tab: 3
                width: parent.width
                editor: window.editor
            }

            SettingsSection {
                page: 4
                width: parent.width
                note: qsTr("在搜索框按 Tab 切换到“内容”，可以查找文件里的文字。")

                SettingRow {
                    title: qsTr("文件内容搜索")
                    keywords: qsTr("内容;全文;文字;开关;关闭;content").split(";")
                    unlocks: [documentsRow, extensionsRow, lowPriorityRow, contentIndexRow]
                    description: qsTr("关掉后搜索框里不再有“内容”，“全部”只按名字找；Win顺 不读取文件里的文字，内容索引也会清空")

                    ToggleSwitch {
                        checked: window.editor.contentSearch
                        onToggled: (on) => window.editor.contentSearch = on
                    }
                }

                SettingRow {
                    id: documentsRow
                    shown: window.editor.contentSearch
                    title: qsTr("搜索文档")
                    keywords: qsTr("Word;Excel;PowerPoint;PPT;PDF;WPS;docx;xlsx").split(";")
                    unlocks: sizeRows.count > 3 ? [sizeRows.itemAt(3)] : []
                    description: qsTr("Word、Excel、PowerPoint、PDF 和 WPS 文件，图片里的文字除外")

                    ToggleSwitch {
                        checked: window.editor.contentDocuments
                        onToggled: (on) => window.editor.contentDocuments = on
                    }
                }

                SettingRow {
                    id: extensionsRow
                    shown: window.editor.contentSearch
                    title: qsTr("搜索这些类型的文本文件")
                    keywords: qsTr("扩展名;后缀;文件类型;txt;md;extension").split(";")
                    values: window.editor.contentExtensions
                    description: qsTr("按扩展名列出；Word、PDF 等文档由“搜索文档”负责")

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
                                    marked: extensionsRow.valueFound(modelData)
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
                    id: lowPriorityRow
                    shown: window.editor.contentSearch
                    title: qsTr("也搜索系统和程序文件夹")
                    keywords: qsTr("系统文件夹;Program Files;AppData;Windows;node_modules").split(";")
                    description: qsTr("Windows、Program Files、AppData 等；文件很多，搜索会变慢")

                    ToggleSwitch {
                        checked: window.editor.contentInLowPriority
                        onToggled: (on) => window.editor.contentInLowPriority = on
                    }
                }

                SettingRow {
                    id: contentIndexRow
                    shown: window.editor.contentSearch
                    title: qsTr("建立内容索引")
                    keywords: qsTr("索引;加速;index").split(";")
                    description: qsTr("在后台记下文件里的文字，搜索快得多。只用于 NTFS 磁盘，约占一两百 MB")
                                 + (window.editor.contentIndexStatus.length > 0 ? "\n" + window.editor.contentIndexStatus : "")

                    ToggleSwitch {
                        checked: window.editor.contentIndex
                        onToggled: (on) => window.editor.contentIndex = on
                    }
                }
            }

            SettingsSection {
                page: 4
                tab: 1
                width: parent.width
                note: qsTr("比上限大的文件不查找内容。")

                // A size limit for each kind of file (ContentSizeLimits::Kind:
                // text, code, data, documents).
                Repeater {
                    id: sizeRows
                    model: [
                        { title: qsTr("文本和日志的大小上限") },
                        { title: qsTr("源代码的大小上限") },
                        { title: qsTr("数据和网页的大小上限") },
                        { title: qsTr("文档的大小上限"),
                          files: qsTr("Word、Excel、PowerPoint、PDF 和 WPS 文件") }
                    ]

                    delegate: SettingRow {
                        id: sizeRow

                        required property int index
                        required property var modelData
                        readonly property real mb: window.editor.contentMaxSizeMB[index]
                        // Under 1 MB in whole KB (Settings: roundedSizeMB), else MB to a hundredth.
                        readonly property bool inKB: mb < 1
                        readonly property real kb: Math.round(mb * 1024)
                        readonly property string shownText: String(inKB ? kb : Math.round(mb * 100) / 100) // 512, 16, 1.25
                        // − and +: halving and doubling under 1 MB (…, 256, 512 KB, 1 MB), by 1 under 16, by 16 above
                        readonly property real down: mb > 16 ? Math.max(16, mb - 16) : mb > 1 ? Math.max(1, mb - 1)
                                                   : Math.pow(2, Math.ceil(Math.log2(kb)) - 1) / 1024
                        readonly property real up: mb >= 16 ? mb + 16 : mb >= 1 ? Math.min(16, Math.floor(mb) + 1)
                                                 : Math.pow(2, Math.floor(Math.log2(kb)) + 1) / 1024

                        // What was typed: a number in the unit shown, or with its own ("50k", "1.5 MB", "2g").
                        function typedMB(text) {
                            const m = /^\s*(\d*(?:\.\d*)?)\s*([kmg]?)b?\s*$/i.exec(text.replace("。", "."))
                            const n = m ? parseFloat(m[1]) : NaN
                            if (isNaN(n))
                                return NaN
                            const unit = m[2].toLowerCase() || (inKB ? "k" : "m")
                            return unit === "k" ? n / 1024 : unit === "g" ? n * 1024 : n
                        }
                        readonly property string extensions: window.editor.contentKindExtensions[index]

                        shown: window.editor.contentSearch && (modelData.files === undefined || window.editor.contentDocuments)
                        title: modelData.title
                        keywords: qsTr("大小;上限;文件大小;size").split(";")
                        description: modelData.files ?? (extensions.length > 0 ? extensions : qsTr("列表里没有这类文件"))

                        FlatButton {
                            glyph: "" // Remove (minus)
                            enabled: !sizeRow.inKB || sizeRow.kb > 1
                            onClicked: window.editor.setContentMaxSizeMB(sizeRow.index, sizeRow.down)
                        }
                        InputBox {
                            width: 80
                            horizontalAlignment: TextInput.AlignHCenter
                            text: sizeRow.shownText
                            validator: RegularExpressionValidator { regularExpression: /\s*\d{0,4}([.。]\d{0,3})?\s*([kKmMgG][bB]?)?/ }
                            onEditingFinished: {
                                const mb = sizeRow.typedMB(text)
                                if (!isNaN(mb) && mb > 0)
                                    window.editor.setContentMaxSizeMB(sizeRow.index, mb)
                                text = Qt.binding(() => sizeRow.shownText)
                            }
                        }
                        FlatButton {
                            glyph: "" // Add
                            enabled: sizeRow.mb < 4096
                            onClicked: window.editor.setContentMaxSizeMB(sizeRow.index, sizeRow.up)
                        }
                        Text {
                            id: unitText
                            anchors.verticalCenter: parent.verticalCenter
                            width: unitMetrics.advanceWidth("MB") // KB takes the same room: the row stays put
                            text: sizeRow.inKB ? "KB" : "MB"
                            color: Theme.subtext
                            font.pixelSize: Theme.fontBody

                            FontMetrics { id: unitMetrics; font: unitText.font }
                        }
                    }
                }
            }

            SettingsSection {
                id: webSection

                // One modal form for both adding and editing; -1: new, -2: closed.
                property int editing: -2
                property Item returnFocus: null
                property string message // what came of an import or export

                function edit(index, button) {
                    returnFocus = button
                    editing = index
                }

                function closeForm() {
                    editing = -2
                    const button = returnFocus || webAddButton // saving may rebuild the edited row
                    button.forceActiveFocus()
                    returnFocus = null
                }

                page: window.webPage
                width: parent.width
                note: qsTr("输入关键词按 Enter 打开网页；网址里有 %s 的，在关键词后加空格和文字就能搜索。")

                onVisibleChanged: if (!visible) { editing = -2; returnFocus = null; message = "" }

                SettingRow {
                    title: qsTr("关键词")
                    keywords: qsTr("网址;网页;搜索引擎;书签;添加;url").split(";")
                    values: {
                        const words = []
                        for (const shortcut of window.editor.webShortcuts)
                            words.push(shortcut.keyword, shortcut.name, shortcut.shownUrl)
                        return words
                    }
                    description: qsTr("输入名称或拼音也能找到")

                    FlatButton {
                        flat: true
                        text: qsTr("导入")
                        glyph: "\uE8B5" // Import
                        onClicked: {
                            const message = window.editor.importWebShortcuts()
                            if (message.length > 0)
                                webSection.message = message
                        }
                    }
                    FlatButton {
                        flat: true
                        text: qsTr("导出")
                        glyph: "\uEDE1" // Export
                        onClicked: {
                            const message = window.editor.exportWebShortcuts()
                            if (message.length > 0)
                                webSection.message = message
                        }
                    }
                    FlatButton {
                        id: webAddButton
                        text: qsTr("添加")
                        glyph: "\uE710" // Add
                        onClicked: webSection.edit(-1, webAddButton)
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
                            visible: window.editor.webShortcuts.length === 0
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
                                        height: Theme.edge(Screen.devicePixelRatio)
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
                                            source: window.editor.siteIconsArrived >= 0
                                                    ? window.editor.webIconUrl(webRow.modelData.iconRef, Theme.dark) : ""
                                        }
                                    }
                                    Rectangle { // the keyword, as keys to press
                                        id: keycap
                                        anchors.left: siteIcon.right
                                        anchors.leftMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.max(44, keywordText.implicitWidth + 16)
                                        height: 26
                                        radius: Theme.controlRadius
                                        color: Theme.keycap

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
                                            id: webEditButton
                                            objectName: "webEditButton"
                                            implicitHeight: 32
                                            flat: true
                                            text: qsTr("编辑")
                                            onClicked: webSection.edit(webRow.index, webEditButton)
                                        }
                                        FlatButton {
                                            implicitHeight: 32
                                            flat: true
                                            text: qsTr("移除")
                                            onClicked: window.editor.removeWebShortcut(webRow.index)
                                        }
                                    }
                                }
                            }
                        }
                    }]
                }

                SettingRow {
                    title: qsTr("网页搜索建议")
                    keywords: qsTr("搜索建议;联想;补全;suggest;suggestion").split(";")
                    description: qsTr("在网站关键词后输入文字时，列出这个网站的搜索建议。只有这时才会把输入的文字发给那个网站")

                    ToggleSwitch {
                        checked: window.editor.webSuggestions
                        onToggled: (on) => window.editor.webSuggestions = on
                    }
                }
            }

            FavoritesSection {
                id: favoritesSection
                page: window.favoritesPage
                width: parent.width
                favorites: window.editor.favorites
                editor: window.editor
            }

            SettingsSection {
                page: window.clipboardPage
                width: parent.width
                note: qsTr("只存在这台电脑上，不会上传。按住 Ctrl 或 Shift 点击可以多选，合在一起粘贴。")

                SettingRow {
                    title: qsTr("记录剪贴板历史")
                    keywords: qsTr("剪切板;复制;历史;clipboard").split(";")
                    description: qsTr("关闭后停止监听并释放历史缓存，已有记录保留在本机。密码管理器复制的密码不会被记录")

                    ToggleSwitch {
                        checked: window.editor.clipboard
                        onToggled: (on) => window.editor.clipboard = on
                    }
                }

                SettingRow {
                    title: qsTr("记录图片")
                    keywords: qsTr("截图;图片;照片;image").split(";")
                    description: qsTr("截图和复制的图片也记下来")

                    ToggleSwitch {
                        checked: window.editor.clipboardImages
                        onToggled: (on) => window.editor.clipboardImages = on
                    }
                }

                SettingRow {
                    title: qsTr("单击粘贴")
                    keywords: qsTr("单击;点击;点一下;直接粘贴;双击;远程;手机;触屏").split(";")
                    description: qsTr("点一下就粘贴，和 Windows 自带的剪贴板一样，手机远程操作时也方便。按住 Ctrl 或 Shift 点击仍是多选；关掉时单击选中、双击粘贴")

                    ToggleSwitch {
                        checked: window.editor.clipboardClickToPaste
                        onToggled: (on) => window.editor.clipboardClickToPaste = on
                    }
                }

                SettingRow {
                    title: qsTr("粘贴后保持位置")
                    keywords: qsTr("位置;顺序;排序;跳到最前;置顶;不动;固定顺序;粘贴后").split(";")
                    description: qsTr("从列表里粘贴一条后，它留在原来的位置，不再跳到最前面。重新复制同样的内容，仍会排到最前。粘贴过的条目右下角带一个蓝色对勾，也不会因为“保留天数”到期而被删")

                    ToggleSwitch {
                        checked: window.editor.clipboardKeepPlace
                        onToggled: (on) => window.editor.clipboardKeepPlace = on
                    }
                }

                SettingRow {
                    title: qsTr("大图片缩略图")
                    keywords: qsTr("缩略图;图片;截图;放大;看清;细节").split(";")
                    description: qsTr("图片的那一行更高，缩略图大到能看清细节，不用打开预览")

                    ToggleSwitch {
                        checked: window.editor.clipboardLargePictures
                        onToggled: (on) => window.editor.clipboardLargePictures = on
                    }
                }

                SettingRow {
                    // By the caret it opens under it, or over it where more
                    // rows fit there (GitHub #38, Placement.above).
                    title: qsTr("开在光标上方时翻转")
                    keywords: qsTr("布局;上方;下方;翻转;颠倒;倒序;顺序").split(";")
                    description: window.editor.clipboardSearchBoxSide === "far"
                                 ? qsTr("最新的条目挨着光标，越往上越早")
                                 : qsTr("搜索框和最新的条目都挨着光标，越往上越早")

                    ToggleSwitch {
                        checked: window.editor.clipboardFlipAbove
                        onToggled: (on) => window.editor.clipboardFlipAbove = on
                    }
                }

                SettingRow {
                    title: qsTr("搜索框位置")
                    keywords: qsTr("布局;上方;下方;搜索框;位置;输入框;顶部;底部").split(";")
                    // Not flipped, the window over the caret is laid out as
                    // under it, so "away" is then only "under the entries".
                    description: window.editor.clipboardFlipAbove
                                 ? qsTr("远离光标时，条目挨着光标，搜索框在窗口另一头")
                                 : qsTr("远离光标时，搜索框放到条目下面")

                    ScopeTabs {
                        labels: [qsTr("靠近光标"), qsTr("远离光标")]
                        current: window.editor.clipboardSearchBoxSide === "far" ? 1 : 0
                        onActivated: (index) => window.editor.clipboardSearchBoxSide = ["near", "far"][index]
                    }
                }

                SettingRow {
                    id: clipboardAppsRow
                    title: qsTr("不记录这些程序复制的内容")
                    keywords: qsTr("密码;隐私;排除;程序;exe;KeePass").split(";")
                    values: window.editor.clipboardExcludedApps
                    description: qsTr("给密码管理器等再加一层保险")

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
                                    marked: clipboardAppsRow.valueFound(modelData)
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
            }

            SettingsSection {
                page: window.clipboardPage
                tab: 1
                width: parent.width

                SettingRow {
                    title: qsTr("用 Win+V 打开，代替 Windows 自带的剪贴板")
                    keywords: qsTr("Win+V;剪切板;代替;替换").split(";")
                    description: qsTr("Windows 的表情面板改用 Win+. 打开")

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
                                case "failed": return window.editor.clipboardWinV
                                                      ? qsTr("没能修改 Windows 的设置，Win+V 仍是 Windows 自带的剪贴板")
                                                      : qsTr("没能修改 Windows 的设置，Win+V 仍被禁用")
                                case "blocked": return qsTr("Windows 的 Win+V 仍被禁用（可能是旧版 Win顺 或其他软件设置的）")
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
                        Row {
                            visible: window.editor.winVState === "blocked"
                                     || (window.editor.winVState === "failed" && !window.editor.clipboardWinV)
                            spacing: 12

                            FlatButton {
                                text: qsTr("还给 Windows")
                                onClicked: window.editor.giveBackToWindows("V")
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
                    keywords: qsTr("快捷键;热键;组合键;hotkey").split(";")
                    values: [window.editor.clipboardHotkey]
                    description: qsTr("不想换掉 Win+V 时用，例如 Win+Alt+V")

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
                    title: qsTr("按住连按快捷键，松开粘贴")
                    keywords: qsTr("快捷键;按住;连按;松开;选择;Ditto;hotkey").split(";")
                    description: qsTr("按住快捷键里的 Ctrl、Alt、Shift 或 Win，每再按一下就往下选一条，松开就粘贴；按住约一秒再松开，粘贴第一条；按住时按 Esc 不粘贴。开着时，按住再按快捷键不再关闭窗口。")

                    ToggleSwitch {
                        checked: window.editor.clipboardHoldToStep
                        onToggled: (on) => window.editor.clipboardHoldToStep = on
                    }
                }

                SettingRow {
                    title: qsTr("面板内数字键粘贴")
                    unlocks: [clipNumberHintsRow]
                    keywords: qsTr("快捷键;数字键;序号;Alt+1;Ctrl+1;冲突;hotkey").split(";")
                    description: qsTr("打开剪贴板面板后，按 %1+1 粘贴第一条，依此类推到第九条；加按 Shift 粘贴为纯文本")
                                 .arg(window.editor.clipboardNumberKeys === "ctrl" ? "Ctrl" : "Alt")

                    ScopeTabs {
                        labels: ["Alt+1–9", "Ctrl+1–9", qsTr("关闭")]
                        current: Math.max(0, ["alt", "ctrl", "off"].indexOf(window.editor.clipboardNumberKeys))
                        onActivated: (index) => window.editor.clipboardNumberKeys = ["alt", "ctrl", "off"][index]
                    }

                    body: Text {
                        visible: window.editor.clipboardNumberKeysNote.length > 0
                        width: parent.width
                        text: window.editor.clipboardNumberKeysNote
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                SettingRow {
                    id: clipNumberHintsRow
                    shown: window.editor.clipboardNumberKeys !== "off"
                    title: qsTr("序号提示")
                    description: qsTr("启动器和剪贴板共用；隐藏提示不影响数字快捷键")
                    keywords: qsTr("快捷键;数字键;图标;按住;显示;隐藏").split(";")

                    ScopeTabs {
                        labels: [qsTr("按住时显示"), qsTr("一直显示"), qsTr("不显示", "number hints")]
                        current: Math.max(0, ["held", "always", "off"].indexOf(window.editor.numberHints))
                        onActivated: (index) => window.editor.numberHints = ["held", "always", "off"][index]
                    }
                }
                SettingRow {
                    title: qsTr("不打开面板直接粘贴")
                    keywords: qsTr("全局;快捷键;数字键;直接粘贴;无需面板;Ctrl+1;Alt+1;Ditto;QuickClipboard").split(";")
                    description: qsTr("按“全部”列表的前九条粘贴，沿用“固定优先”顺序，不受搜索和分组影响。启用后会占用其他程序的同名快捷键，例如浏览器切换标签页。")

                    ScopeTabs {
                        labels: [qsTr("关闭"), "Ctrl+1–9", "Alt+1–9"]
                        current: Math.max(0, ["off", "ctrl", "alt"].indexOf(window.editor.clipboardQuickPaste))
                        onActivated: (index) => window.editor.clipboardQuickPaste = ["off", "ctrl", "alt"][index]
                    }

                    body: Text {
                        visible: window.editor.clipboardQuickPasteError.length > 0
                        width: parent.width
                        text: window.editor.clipboardQuickPasteError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
            }

            SettingsSection {
                page: window.clipboardPage
                tab: 2
                width: parent.width

                SettingRow {
                    id: clipMaxRow

                    // Three choices, or any number typed in (GitHub #32); 0 = no limit.
                    readonly property var presets: [300, 500, 0]
                    readonly property int maxItems: window.editor.clipboardMaxItems
                    readonly property string typedText: presets.includes(maxItems) ? "" : String(maxItems)

                    title: qsTr("最多保留")
                    keywords: qsTr("数量;条数;上限;自定义").split(";")
                    description: maxItems === 0
                                 ? qsTr("不限条数，只按下面的保留时间删除；“固定”和分组里的一直保留")
                                 : qsTr("“固定”和分组里的不算，一直保留")

                    ScopeTabs {
                        labels: [qsTr("%1 条").arg(300), qsTr("%1 条").arg(500), qsTr("无限", "clipboard items")]
                        current: clipMaxRow.presets.indexOf(clipMaxRow.maxItems)
                        onActivated: (index) => window.editor.clipboardMaxItems = clipMaxRow.presets[index]
                    }
                    InputBox {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 80
                        horizontalAlignment: TextInput.AlignHCenter
                        placeholder: qsTr("自定义")
                        text: clipMaxRow.typedText
                        validator: RegularExpressionValidator { regularExpression: /\s*\d{0,6}\s*/ }
                        onEditingFinished: {
                            const n = parseInt(text, 10)
                            if (n > 0)
                                window.editor.clipboardMaxItems = Math.min(100000, Math.max(10, n))
                            text = Qt.binding(() => clipMaxRow.typedText)
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("条", "clipboard items")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontBody
                    }
                }

                SettingRow {
                    title: qsTr("保留时间")
                    keywords: qsTr("过期;天数;自动删除").split(";")
                    description: qsTr("这么久没用过的自动删除，分组里的除外")

                    ScopeTabs {
                        labels: [qsTr("%n 天", "", 7), qsTr("%n 天", "", 30), qsTr("%n 天", "", 90), qsTr("一直保留")]
                        current: [7, 30, 90, 0].indexOf(window.editor.clipboardMaxDays)
                        onActivated: (index) => window.editor.clipboardMaxDays = [7, 30, 90, 0][index]
                    }
                }

                SettingRow {
                    // Copied files deleted or moved since (GitHub #54); "固定"
                    // and groups are left alone.
                    title: qsTr("已不存在的文件记录")
                    keywords: qsTr("删除;移走;失效;找不到;missing").split(";")
                    description: qsTr("隐藏的不显示，搜索还能找到，仍占条数上限；清理的会删掉，不占条数。U 盘拔了、网络盘断了不算")

                    ScopeTabs {
                        labels: [qsTr("不处理"), qsTr("自动隐藏"), qsTr("自动清理")]
                        current: Math.max(0, ["keep", "hide", "clean"].indexOf(window.editor.clipboardMissingFiles))
                        onActivated: (index) => window.editor.clipboardMissingFiles = ["keep", "hide", "clean"][index]
                    }
                }

                QtObject {
                    id: clipImport

                    // A text file picked, waiting for how to cut it into entries.
                    property string path
                    property int split: 1 // 0 a line each, 1 paragraphs, 2 the whole file
                    property string ditto: ""

                    function pick() {
                        const picked = window.editor.pickClipboardImportFile()
                        if (picked.length === 0)
                            return
                        if (window.editor.clipboardImportAsksSplit(picked)) {
                            path = picked
                        } else {
                            path = ""
                            window.editor.importClipboard(picked, 1)
                        }
                    }
                    function start() {
                        const picked = path
                        path = ""
                        window.editor.importClipboard(picked, split)
                    }

                    Component.onCompleted: ditto = window.editor.dittoDatabase()
                }

                SettingRow {
                    title: qsTr("导入剪贴板历史")
                    keywords: qsTr("Ditto;导入;迁移;文本;表格;txt;csv;import").split(";")
                    description: window.editor.clipboardImportMessage.length > 0
                                 ? window.editor.clipboardImportMessage
                                 : qsTr("从 Ditto、文本文件（.txt）或表格文件（.csv）导入，放进一个新分组里，不会被自动删除")

                    FlatButton {
                        visible: clipImport.ditto.length > 0
                        enabled: !window.editor.clipboardImporting
                        text: qsTr("从 Ditto 导入")
                        glyph: "\uE8B5" // Import
                        onClicked: {
                            clipImport.path = ""
                            window.editor.importClipboard(clipImport.ditto, 1)
                        }
                    }
                    FlatButton {
                        text: window.editor.clipboardImporting ? qsTr("取消") : qsTr("导入…")
                        glyph: window.editor.clipboardImporting ? "\uE711" : "\uE8B5" // Cancel, Import
                        onClicked: window.editor.clipboardImporting ? window.editor.cancelClipboardImport() : clipImport.pick()
                    }
                }

                SettingRow {
                    shown: clipImport.path.length > 0 && !window.editor.clipboardImporting
                    title: qsTr("拆分方式")
                    description: clipImport.path.split(/[\\/]/).pop()

                    ScopeTabs {
                        labels: [qsTr("每行一条"), qsTr("空行分段"), qsTr("整个文件一条")]
                        current: clipImport.split
                        onActivated: (index) => clipImport.split = index
                    }
                    FlatButton {
                        text: qsTr("取消")
                        flat: true
                        onClicked: clipImport.path = ""
                    }
                    FlatButton {
                        text: qsTr("开始导入")
                        highlighted: true
                        onClicked: clipImport.start()
                    }
                }

                SettingRow {
                    title: qsTr("清除剪贴板历史")
                    keywords: qsTr("删除;清空").split(";")
                    description: window.editor.clipboardCount > 0
                                 ? qsTr("共 %1 条，“固定”和分组里的会保留").arg(window.editor.clipboardCount)
                                 : window.editor.clipboardCount < 0
                                   ? qsTr("已关闭，历史记录保留在本机；开启后显示条数")
                                   : qsTr("没有记录")

                    FlatButton {
                        id: clearClipboardButton

                        property bool confirming: false

                        text: confirming ? qsTr("确定清除？再点一次") : qsTr("清除")
                        glyph: confirming ? "" : "" // Delete
                        enabled: window.editor.clipboardCount !== 0 && !window.editor.clipboardImporting
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
                page: window.advancedPage
                width: parent.width
                title: qsTr("更新")
                note: qsTr("检查更新访问 lcstp.com，只发送 Windows 版本和安装方式，不发送个人信息。")

                SettingRow {
                    title: qsTr("版本 %1").arg(window.updater.currentVersion)
                    keywords: qsTr("升级;新版本;version;update").split(";")
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
                    keywords: qsTr("升级;新版本;update").split(";")
                    description: qsTr("启动时和每隔 12 小时检查一次，有新版本时在托盘提示")

                    ToggleSwitch {
                        checked: window.editor.autoUpdate
                        onToggled: (on) => window.editor.autoUpdate = on
                    }
                }
            }

            SettingsSection {
                page: window.advancedPage
                width: parent.width
                title: qsTr("备份")
                note: qsTr("备份设置、收藏夹和添加的插件，不含索引和历史记录。选同步盘里的文件夹（OneDrive、坚果云，或用 alist、RaiDrive 挂载的 WebDAV 盘），备份就跟着同步到云上。")

                SettingRow {
                    objectName: "backupExportRow"
                    title: qsTr("导出设置")
                    keywords: qsTr("备份;导出;迁移;换电脑;ZIP;backup;export;transfer").split(";")
                    description: qsTr("导出一份 ZIP，带到新电脑后从备份恢复；不用开启自动备份。文件夹路径按原样保留，插件密码需重新填写。")
                    bodyShown: window.backup.busy || window.backup.problem.length > 0 || window.backup.message.length > 0

                    FlatButton {
                        objectName: "backupExportButton"
                        enabled: !window.backup.busy
                        text: qsTr("导出备份…")
                        onClicked: window.backup.exportBackup(window)
                    }
                    body: Text {
                        objectName: "backupOperationStatus"
                        width: parent.width
                        text: window.backup.busy ? qsTr("正在处理备份…")
                            : window.backup.problem.length > 0 ? qsTr("备份操作失败：%1").arg(window.backup.problem)
                            : window.backup.message
                        textFormat: Text.PlainText
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    id: backupRow

                    readonly property bool on: window.editor.backupFolder.length > 0

                    title: qsTr("自动备份设置")
                    keywords: qsTr("备份;同步;云;WebDAV;坚果云;OneDrive;网盘;导出;backup;sync").split(";")
                    description: !on ? qsTr("设置有变化时自动备份到你选的文件夹，保留最近 10 份")
                               : (window.backup.busy ? qsTr("正在备份…")
                                  : window.backup.problem.length > 0 ? qsTr("没有备份成功：%1").arg(window.backup.problem)
                                  : !isNaN(window.backup.last.getTime()) ? qsTr("上次备份：%1").arg(window.sinceText(window.backup.last))
                                  : qsTr("还没有备份"))
                                 + "\n" + window.editor.backupFolder

                    FlatButton {
                        visible: backupRow.on
                        text: qsTr("打开")
                        glyph: "" // FolderOpen
                        onClicked: window.backup.openFolder()
                    }
                    FlatButton {
                        visible: backupRow.on
                        text: qsTr("更改")
                        onClicked: window.editor.chooseBackupFolder()
                    }
                    ToggleSwitch {
                        checked: backupRow.on
                        onToggled: (on) => {
                            if (on)
                                window.editor.chooseBackupFolder()
                            else
                                window.editor.stopBackups()
                        }
                    }
                }

                SettingRow {
                    title: qsTr("恢复设置")
                    keywords: qsTr("恢复;还原;导入;换电脑;restore;import").split(";")
                    description: qsTr("选择 ZIP 备份，校验通过后恢复并重启；当前设置先存到本机数据文件夹的 backups 中。换机导出的备份保留本机的备份和索引位置。")

                    FlatButton {
                        visible: window.editor.backupFolder.length > 0
                        enabled: !window.backup.busy
                        text: qsTr("立即备份")
                        onClicked: window.backup.backUpNow()
                    }
                    FlatButton {
                        enabled: !window.backup.busy
                        text: qsTr("从备份恢复…")
                        onClicked: window.backup.restore(window)
                    }
                }
            }

            SettingsSection {
                page: window.advancedPage
                width: parent.width

                SettingRow {
                    id: proxyRow

                    // The address as typed and kept: its problem shows under it
                    // while manual is chosen. The system's proxy is followed
                    // while it is not a good one (App::applySettings).
                    readonly property bool manual: window.editor.proxyMode === "manual"
                    readonly property string problem: manual ? window.editor.proxyProblem(proxyBox.text) : ""
                    readonly property var modes: ["system", "none", "manual"]

                    title: qsTr("网络代理")
                    keywords: qsTr("代理;翻墙;梯子;科学上网;Clash;v2ray;GitHub;连不上;超时;插件库;更新;网络;proxy;vpn;timeout").split(";")
                    description: qsTr("插件库、检查更新等联网都走这里。“跟随系统”用 Windows 设置里的代理（包括自动检测和 PAC 脚本）；用代理软件时，打开它的“系统代理”就行，或者手动填地址")
                    bodyShown: manual

                    ScopeTabs {
                        labels: [qsTr("跟随系统"), qsTr("不使用代理"), qsTr("手动")]
                        current: Math.max(0, proxyRow.modes.indexOf(window.editor.proxyMode))
                        onActivated: (index) => window.editor.proxyMode = proxyRow.modes[index]
                    }

                    body: [
                        InputBox {
                            id: proxyBox
                            width: Math.min(parent.width, 320)
                            text: window.editor.proxyAddress
                            placeholder: qsTr("代理地址，例如 %1").arg("127.0.0.1:7890")
                            onEditingFinished: window.editor.proxyAddress = text
                        },
                        Text {
                            visible: proxyRow.problem.length > 0
                            width: parent.width
                            text: proxyRow.problem
                            textFormat: Text.PlainText
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        },
                        Text {
                            visible: proxyRow.problem.length === 0
                            width: parent.width
                            text: qsTr("HTTP 代理，地址写 127.0.0.1:7890 或 http://127.0.0.1:7890。改完马上生效，不用重启")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    ]
                }

                SettingRow {
                    title: qsTr("界面绘制方式")
                    keywords: qsTr("渲染;GPU;显卡;内存;模糊;清晰;renderer").split(";")
                    description: window.editor.restartRequired
                                 ? qsTr("重启 Win顺后生效")
                                 : qsTr("“省内存”少占约 50 MB，但文字偏模糊")

                    FlatButton { // sized like one of the tabs next to it
                        anchors.verticalCenter: parent.verticalCenter
                        implicitHeight: 32
                        radius: Theme.largeControlRadius
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
                    keywords: qsTr("移动索引;磁盘空间;C盘;D盘;index").split(";")
                    description: (window.editor.indexSize.length > 0
                                  ? qsTr("文件索引和内容索引，共 %1；可以移到其他内置硬盘").arg(window.editor.indexSize)
                                  : qsTr("文件索引和内容索引；可以移到其他内置硬盘"))
                                 + "\n" + window.editor.indexFolder
                    bodyShown: moving || window.editor.indexFolderProblem.length > 0 || window.editor.indexFolderNote.length > 0

                    FlatButton {
                        text: qsTr("打开")
                        glyph: "" // FolderOpen
                        enabled: !indexFolderRow.moving
                        onClicked: window.editor.openIndexFolder()
                    }
                    FlatButton {
                        text: qsTr("更改")
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
                                radius: Theme.progressRadius
                                color: Theme.track

                                Rectangle {
                                    width: parent.width * Math.max(0, window.editor.indexMoveProgress) / 100
                                    height: parent.height
                                    radius: Theme.progressRadius
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
                    keywords: qsTr("日志;记录;log;AppData").split(";")
                    description: qsTr("搜索记录、剪贴板历史和日志") + "\n" + window.editor.dataFolder

                    FlatButton {
                        text: qsTr("打开")
                        glyph: "" // FolderOpen
                        onClicked: window.editor.openDataFolder()
                    }
                }

                SettingRow {
                    title: qsTr("诊断日志")
                    keywords: qsTr("诊断;日志;索引慢;排查;反馈;显示;字体;错字;乱码;截图;光标;指针;卡顿;diagnostics;log;capture;cursor").split(";")
                    description: qsTr("导出后会打开文件夹，把 WinShun-diagnostics.zip 发给开发者即可。包含索引耗时和错误、截图的耗时和卡顿、鼠标指针设置、界面显示和字体的状态，可能含文件路径；不含设置、剪贴板或文档正文。界面显示不对时先导出再重启，重启后就查不到了。诊断记录最多 3 MB，过期自动清理；诊断包只保留一份")
                                 + (window.editor.diagnosticsStatus.length > 0 ? "\n" + window.editor.diagnosticsStatus : "")

                    FlatButton {
                        text: window.editor.diagnosticsBusy ? qsTr("正在导出…") : qsTr("导出诊断日志")
                        enabled: !window.editor.diagnosticsBusy
                        glyph: ""
                        onClicked: window.editor.exportDiagnostics()
                    }
                }

                SettingRow {
                    title: qsTr("恢复默认设置")
                    keywords: qsTr("重置;默认;reset").split(";")
                    description: qsTr("所有设置回到刚安装时的样子")

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

            AiAccessSection {
                page: window.aiPage
                width: parent.width
                editor: window.editor
                lockedElsewhere: aiPrivacySection.lockedRows
            }

            AiClientsSection {
                page: window.aiPage
                tab: 1
                width: parent.width
                editor: window.editor
            }

            AiPrivacySection {
                id: aiPrivacySection
                page: window.aiPage
                tab: 2
                width: parent.width
                editor: window.editor
            }

            PluginsSection {
                id: pluginsSection
                page: window.pluginsPage
                width: parent.width
                plugins: window.editor.plugins
                editor: window.editor
                lockedElsewhere: pluginDevSection.lockedRows
                onSettingRequested: (which) => window.goTo(which === "folderMenu" ? folderMenuRow
                                                           : which === "dialogBar" ? dialogBarRow : explorerTypingRow)
            }

            PluginGallerySection {
                page: window.pluginsPage
                tab: 1
                width: parent.width
                plugins: window.editor.plugins
                onShowInstalled: (pluginId) => window.showPluginPage(pluginId)
                onShowProxySettings: window.goTo(proxyRow)
                onShowDeveloper: {
                    window.editor.plugins.developerMode = true
                    window.showTab(2)
                }
            }

            PluginDevSection {
                id: pluginDevSection
                page: window.pluginsPage
                tab: 2
                width: parent.width
                plugins: window.editor.plugins
            }
        }
    }

    Rectangle { // scroll position
        visible: flick.visible && flick.contentHeight > flick.height
        x: parent.width - 7
        y: flick.y + flick.visibleArea.yPosition * flick.height
        width: 3
        height: Math.max(24, flick.visibleArea.heightRatio * flick.height)
        radius: Theme.smallRadius
        color: Theme.faint
        opacity: 0.6
    }

    Loader { // outside the page's Flickable: always in the visible window
        id: webDialog
        z: 1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        active: webSection.editing >= -1
        visible: active
        sourceComponent: WebShortcutForm {
            editor: window.editor
            index: webSection.editing
            // Copy the draft once. Saving rebuilds the list and its delegates.
            Component.onCompleted: {
                if (index >= 0)
                    shortcut = window.editor.webShortcuts[index]
            }
            onFinished: webSection.closeForm()
        }
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

    PluginDialog { // the 插件 page's questions: permissions, versions, removal
        id: pluginDialog
        z: 1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        plugins: window.editor.plugins
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
