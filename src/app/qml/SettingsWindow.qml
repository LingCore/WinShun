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
        { title: qsTr("结果列表"), glyph: "\uE8FD" }, // BulletedList
        { title: qsTr("文件内容搜索"), glyph: "" }, // Document
        { title: qsTr("网页搜索"), glyph: "\uE774" }, // Globe
        { title: qsTr("剪贴板"), glyph: "\uE77F" }, // Paste
        { title: qsTr("高级"), glyph: "" }, // Settings
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
        qsTr("剪切板;粘贴板;复制;粘贴;clipboard"),
        qsTr("其他;更多;advanced"),
        ""
    ]
    // The tabs of the pages too long for one screen, each a few sections
    // (SettingsSection.tab); [] for a page without tabs.
    readonly property var pageTabs: [
        [qsTr("快捷键", "settings tab"), qsTr("任务栏"), qsTr("对话框"), qsTr("启动与历史")],
        [],
        [qsTr("文件夹"), qsTr("文件夹名称"), qsTr("磁盘")],
        [],
        [qsTr("常规"), qsTr("大小上限")],
        [],
        [qsTr("记录"), qsTr("快捷键", "settings tab"), qsTr("保留")],
        [],
        []
    ]
    readonly property int gleaningPage: 8
    readonly property int webPage: 5
    readonly property int clipboardPage: 6
    readonly property int advancedPage: 7 // where the updates are
    property int currentPage: 0
    property int currentTab: 0
    property var lastTabs: [] // by page: the tab it was left on, to come back to
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

    function clearFocus() { window.contentItem.forceActiveFocus() }

    // The taskbar button is pinned, Windows' search hidden, a file manager
    // installed, outside Win顺: looked at again when the user comes back.
    onActiveChanged: {
        if (active) {
            editor.refreshTaskbarState()
            editor.refreshFileManagers()
            editor.refreshNumberKeys()
        }
    }

    // Settings::fileManager's names, as the user knows them.
    function fileManagerName(name) {
        switch (name) {
        case "totalcmd": return "Total Commander"
        case "dopus": return "Directory Opus"
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
        currentTab = tab ?? lastTabs[index] ?? 0
        flick.cancelFlick()
        flick.contentY = 0
    }

    function showTab(tab) {
        if (tab === currentTab)
            return
        currentTab = tab
        flick.cancelFlick()
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
            flick.cancelFlick()
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
        enabled: !window.editor.recordingHotkey && !updateDialog.open
        onActivated: searchBox.focusAll()
    }
    Shortcut { // Ctrl+Tab, Ctrl+Shift+Tab: through the page's tabs
        sequences: [StandardKey.NextChild]
        enabled: !window.searching && !window.warm && !window.editor.recordingHotkey && !updateDialog.open
        onActivated: window.cycleTab(1)
    }
    Shortcut {
        // Not StandardKey.PreviousChild: its Ctrl+Shift+Backtab never matches
        // on Windows (Qt 6.12), the written-out keys do.
        sequences: ["Ctrl+Shift+Tab"]
        enabled: !window.searching && !window.warm && !window.editor.recordingHotkey && !updateDialog.open
        onActivated: window.cycleTab(-1)
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
            radius: 4
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
                    radius: 1.5
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
            transform: Translate {
                y: {
                    const dpr = Screen.devicePixelRatio
                    const deviceY = (flick.y + flick.contentItem.y + page.y) * dpr
                    return (Math.round(deviceY) - deviceY + 0.02) / dpr
                }
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
                    radius: 6
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
                                    radius: 5
                                    color: windowsArea.pressed ? Theme.selection : windowsArea.containsMouse ? Theme.navHover : "transparent"
                                }
                                Rectangle {
                                    visible: windowsRow.index > 0
                                    x: 1
                                    width: parent.width - 2
                                    height: 1
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
                    title: qsTr("双击 Ctrl 打开")
                    keywords: qsTr("双击;呼出;唤出;double ctrl").split(";")
                    unlocks: [gamesRow, fullScreenRow, doubleCtrlAppsRow]
                    description: qsTr("连按两下 Ctrl，打开或关闭搜索框")

                    ToggleSwitch {
                        checked: window.editor.doubleCtrl
                        onToggled: (on) => window.editor.doubleCtrl = on
                    }
                }

                SettingRow {
                    id: gamesRow
                    shown: window.editor.doubleCtrl
                    title: qsTr("玩游戏时不响应双击 Ctrl")
                    keywords: qsTr("游戏;蹲下;误触;game").split(";")
                    description: qsTr("游戏全屏或藏起鼠标时不打开，免得蹲下时误触")

                    ToggleSwitch {
                        checked: window.editor.doubleCtrlPauseInGames
                        onToggled: (on) => window.editor.doubleCtrlPauseInGames = on
                    }
                }

                SettingRow {
                    id: fullScreenRow
                    shown: window.editor.doubleCtrl
                    title: qsTr("任何程序全屏时都不响应双击 Ctrl")
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
                    title: qsTr("在这些程序里不响应双击 Ctrl")
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
            }

            SettingsSection {
                id: taskbarSteps

                // The four steps, in order, each ticked off once done, here
                // or outside Win顺 (the editor watches the taskbar and the
                // setting of Windows' search box). Windows' own search may
                // stay next to Win顺's; hiding it is only recommended. The
                // first: Win顺's box on the taskbar, or its pinned button.
                readonly property var done: [(window.editor.taskbarSearchBox && window.editor.searchBoxShown)
                                                 || window.editor.taskbarButtonPinned,
                                             !window.editor.windowsSearchShown,
                                             window.editor.winSState === "on", window.editor.taskbarStartTyping]
                readonly property int doneCount: done.filter(d => d).length
                readonly property int current: done.indexOf(false) // its button stands out

                page: 0
                tab: 1
                width: parent.width
                title: qsTr("代替任务栏上的 Windows 搜索")
                note: doneCount === done.length
                      ? qsTr("都设好了：任务栏、Win+S 和开始菜单都用 Win顺 搜索。")
                      : qsTr("按顺序做完这四步，做好的会自动打勾。已完成 %1/4。")
                        .arg(doneCount)

                SettingRow {
                    id: taskbarButtonRow

                    property bool missing: false // no WinShunSearch.exe next to Win顺
                    readonly property bool boxes: window.editor.searchBoxSupported // Windows 11; else the button alone
                    readonly property bool boxOn: window.editor.taskbarSearchBox
                    readonly property bool pinned: window.editor.taskbarButtonPinned

                    title: boxes ? qsTr("1. 在任务栏上放 Win顺 的搜索框") : qsTr("1. 把 Win顺 按钮固定到任务栏")
                    keywords: qsTr("固定;任务栏;按钮;搜索框;pin;taskbar").split(";")
                    description: {
                        if (!boxes)
                            return pinned ? qsTr("已固定，点它就能打开 Win顺")
                                          : qsTr("点“固定”后选“是”；打开的若是文件夹，右键其中的图标选“固定到任务栏”")
                        if (boxOn && window.editor.searchBoxShown)
                            return qsTr("已放好，点它就能打字")
                        if (boxOn)
                            return qsTr("任务栏太挤，放不下：取消固定几个图标，或改为固定按钮")
                        return pinned ? qsTr("已固定按钮，也可以再放一个搜索框")
                                      : qsTr("放在任务栏图标旁边，点它直接打字")
                    }
                    // From the conditions, not from the texts' `visible`: false
                    // while the body is hidden, it would keep it hidden for good.
                    bodyShown: boxes || (missing && !taskbarSteps.done[0])

                    Glyph {
                        visible: taskbarSteps.done[0]
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: "" // CheckMark
                        size: 20
                        color: Theme.accent
                    }
                    ToggleSwitch {
                        visible: taskbarButtonRow.boxes
                        checked: taskbarButtonRow.boxOn
                        onToggled: (on) => window.editor.taskbarSearchBox = on
                    }
                    FlatButton {
                        visible: !taskbarButtonRow.boxes && !taskbarSteps.done[0]
                        text: qsTr("固定")
                        glyph: "" // Pin
                        highlighted: taskbarSteps.current === 0
                        onClicked: taskbarButtonRow.missing = !window.editor.pinTaskbarButton()
                    }

                    body: Column {
                        width: parent.width
                        spacing: 8

                        Row { // the other way in: a button of its own among the taskbar's icons
                            visible: taskbarButtonRow.boxes
                            spacing: 12

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: taskbarButtonRow.pinned ? qsTr("Win顺 按钮也已固定到任务栏")
                                                              : qsTr("也可以只固定一个按钮")
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                            }
                            FlatButton {
                                visible: !taskbarButtonRow.pinned
                                text: qsTr("固定按钮")
                                glyph: "" // Pin
                                onClicked: taskbarButtonRow.missing = !window.editor.pinTaskbarButton()
                            }
                        }
                        Text {
                            visible: taskbarButtonRow.missing && !taskbarButtonRow.pinned
                            width: parent.width
                            text: qsTr("没找到 WinShunSearch.exe，重新安装 Win顺 就有了")
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                }

                SettingRow {
                    title: qsTr("2. 隐藏 Windows 自带的搜索（推荐）")
                    keywords: qsTr("隐藏;任务栏;搜索框;taskbar").split(";")
                    description: taskbarSteps.done[1]
                                 ? qsTr("已隐藏")
                                 : qsTr("在任务栏设置里把“搜索”选成“隐藏”")

                    Glyph {
                        visible: taskbarSteps.done[1]
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: "" // CheckMark
                        size: 20
                        color: Theme.accent
                    }
                    FlatButton {
                        visible: !taskbarSteps.done[1]
                        text: qsTr("打开任务栏设置")
                        highlighted: taskbarSteps.current === 1
                        onClicked: window.editor.openUrl("ms-settings:taskbar")
                    }
                }

                SettingRow {
                    title: qsTr("3. 用 Win+S 打开 Win顺")
                    keywords: qsTr("Win+S;Windows 搜索;截图;Win+Shift+S").split(";")
                    description: qsTr("切换时资源管理器会重启，打开的文件夹窗口会关闭")

                    Glyph {
                        visible: taskbarSteps.done[2]
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: "" // CheckMark
                        size: 20
                        color: Theme.accent
                    }
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
                                case "failed": return qsTr("没能修改 Windows 的设置，Win+S 仍是 Windows 搜索")
                                default: return ""
                                }
                            }
                            color: window.editor.winSState === "failed" ? Theme.danger : Theme.text
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
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
                    title: qsTr("4. 在开始菜单里打字，也用 Win顺 搜索")
                    keywords: qsTr("开始菜单;打字;Win;start menu").split(";")
                    description: qsTr("想用 Windows 自带的搜索时，先按一下左 Alt 再打字")

                    Glyph {
                        visible: taskbarSteps.done[3]
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: "" // CheckMark
                        size: 20
                        color: Theme.accent
                    }
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
                    keywords: qsTr("文件管理器;资源管理器;打开文件夹;打开所在位置;Total Commander;Directory Opus;TC;Opus").split(";")
                    description: qsTr("打开文件夹、打开文件所在位置时用它")

                    body: ScopeTabs { // under the text: the names are long
                        labels: window.editor.fileManagers.map(name => window.fileManagerName(name))
                        current: Math.max(0, window.editor.fileManagers.indexOf(window.editor.fileManager))
                        onActivated: (index) => window.editor.fileManager = window.editor.fileManagers[index]
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
                    description: qsTr("对话框出现时，自动转到文件管理器当前的文件夹")

                    ToggleSwitch {
                        checked: window.editor.dialogAutoJump
                        onToggled: (on) => window.editor.dialogAutoJump = on
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
                    description: qsTr("没输入时列出最近打开的项目，搜索时排在前面")

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
                    title: qsTr("不搜索的文件夹")
                    keywords: qsTr("排除;忽略;隐藏;黑名单;exclude").split(";")
                    values: window.editor.excludedPaths
                    description: qsTr("这些文件夹里的内容都不出现在结果里")

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
                tab: 2
                width: parent.width
                note: qsTr("修改后在后台重新整理，不影响搜索。")

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
            }

            SettingsSection {
                page: 3
                width: parent.width
                note: qsTr("在搜索框按 Tab 切换“全部”“文件”“内容”；“文件”里还能只找文件夹。")

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
                    title: qsTr("最近修改的排在前面")
                    keywords: qsTr("排序;修改时间;修改日期;最新;sort;date").split(";")
                    description: qsTr("在“文件”里按修改时间排，最新的在前；底栏按钮也能切换")

                    ToggleSwitch {
                        checked: window.editor.sortByModified
                        onToggled: (on) => window.editor.sortByModified = on
                    }
                }

                SettingRow {
                    title: qsTr("显示修改日期")
                    keywords: qsTr("修改时间;修改日期;日期;时间;date").split(";")
                    description: qsTr("显示在名字右边，例如“昨天 14:32”")

                    ToggleSwitch {
                        checked: window.editor.showModified
                        onToggled: (on) => window.editor.showModified = on
                    }
                }

                SettingRow {
                    title: qsTr("用数字键直接打开")
                    keywords: qsTr("快捷键;数字键;序号;Ctrl+1;Alt+1;冲突;hotkey").split(";")
                    description: qsTr("按住 %1 标出序号，按 %1+1 打开第一行")
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
            }

            SettingsSection {
                page: 4
                width: parent.width
                note: qsTr("在搜索框按 Tab 切换到“内容”，可以查找文件里的文字。")

                SettingRow {
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
                    title: qsTr("也搜索系统和程序文件夹")
                    keywords: qsTr("系统文件夹;Program Files;AppData;Windows;node_modules").split(";")
                    description: qsTr("Windows、Program Files、AppData 等；文件很多，搜索会变慢")

                    ToggleSwitch {
                        checked: window.editor.contentInLowPriority
                        onToggled: (on) => window.editor.contentInLowPriority = on
                    }
                }

                SettingRow {
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
                        readonly property int mb: window.editor.contentMaxSizeMB[index]
                        readonly property string extensions: window.editor.contentKindExtensions[index]

                        shown: modelData.files === undefined || window.editor.contentDocuments
                        title: modelData.title
                        keywords: qsTr("大小;上限;文件大小;size").split(";")
                        description: modelData.files ?? (extensions.length > 0 ? extensions : qsTr("列表里没有这类文件"))

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
            }

            SettingsSection {
                id: webSection

                // The shortcut being changed in its row; -1: a new one, at the end of the list; -2: none.
                property int editing: -2

                page: window.webPage
                width: parent.width
                note: qsTr("输入关键词按 Enter 打开网页；网址里有 %s 的，在关键词后加空格和文字就能搜索。")

                onVisibleChanged: if (!visible) editing = -2

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
                page: window.clipboardPage
                width: parent.width
                note: qsTr("只存在这台电脑上，不会上传。按住 Ctrl 点击可以多选，合在一起粘贴。")

                SettingRow {
                    title: qsTr("记录剪贴板历史")
                    keywords: qsTr("剪切板;复制;历史;clipboard").split(";")
                    description: qsTr("密码管理器复制的密码不会被记录")

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
                    title: qsTr("用数字键直接粘贴")
                    keywords: qsTr("快捷键;数字键;序号;Alt+1;Ctrl+1;冲突;hotkey").split(";")
                    description: qsTr("按 %1+1 粘贴第一条，加按 Shift 粘贴为纯文本")
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
            }

            SettingsSection {
                page: window.clipboardPage
                tab: 2
                width: parent.width

                SettingRow {
                    title: qsTr("最多保留")
                    keywords: qsTr("数量;条数;上限").split(";")
                    description: qsTr("“固定”和分组里的不算，一直保留")

                    ScopeTabs {
                        labels: [qsTr("%1 条").arg(50), qsTr("%1 条").arg(100), qsTr("%1 条").arg(200),
                                 qsTr("%1 条").arg(300)]
                        current: [50, 100, 200, 300].indexOf(window.editor.clipboardMaxItems)
                        onActivated: (index) => window.editor.clipboardMaxItems = [50, 100, 200, 300][index]
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
                    title: qsTr("清除剪贴板历史")
                    keywords: qsTr("删除;清空").split(";")
                    description: window.editor.clipboardCount > 0
                                 ? qsTr("共 %1 条，“固定”和分组里的会保留").arg(window.editor.clipboardCount)
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
                page: window.advancedPage
                width: parent.width
                title: qsTr("更新")
                note: qsTr("检查更新只访问 GitHub，不发送任何个人信息。")

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

                SettingRow {
                    title: qsTr("界面绘制方式")
                    keywords: qsTr("渲染;GPU;显卡;内存;模糊;清晰;renderer").split(";")
                    description: window.editor.restartRequired
                                 ? qsTr("重启 Win顺后生效")
                                 : qsTr("“省内存”少占约 50 MB，但文字偏模糊")

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
                    keywords: qsTr("日志;记录;log;AppData").split(";")
                    description: qsTr("搜索记录、剪贴板历史和日志") + "\n" + window.editor.dataFolder

                    FlatButton {
                        text: qsTr("打开")
                        glyph: "" // FolderOpen
                        onClicked: window.editor.openDataFolder()
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
