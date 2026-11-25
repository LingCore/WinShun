pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as Controls
import WinShun

// The settings window: categories on the left, the chosen one's options on the
// right. Changes are saved and applied as soon as they are made.
Window {
    id: window

    required property SettingsEditor editor
    required property WindowFrame frame
    required property Updater updater
    required property Backup backup

    // Read by the items, sections and rows here instead of a Screen attached
    // object each: every one of those joins and leaves the QScreen on each move
    // to another monitor, a hitch with thousands (GitHub #111; docs/pitfalls.md).
    readonly property real dpr: Screen.devicePixelRatio

    readonly property var renderers: ["software", "d3d11"]
    readonly property var languages: ["system", "zh", "en"]
    readonly property var transparencies: ["off", "on"]
    readonly property var dialogBarPlaces: ["auto", "below", "left", "right"]
    // The categories, in the order listed. Each one's place is its index
    // (generalPage …); its id names it where a number would go stale as the
    // order changes (Shortcuts' 去设置, AiSettings, --settings-at: pageIndex).
    // `group`: the first of a group, set a little apart from the one above.
    readonly property var pages: [
        { id: "general", title: qsTr("常规"), glyph: "" }, // Settings
        { id: "keys", title: qsTr("快捷键", "settings page"), glyph: "" }, // Keyboard
        { id: "appearance", title: qsTr("外观"), glyph: "\uE771" }, // Personalize
        { id: "scope", title: qsTr("搜索范围"), glyph: "", group: true }, // Folder
        { id: "results", title: qsTr("结果列表"), glyph: "\uE8FD" }, // BulletedList
        { id: "integration", title: qsTr("系统整合"), glyph: "ph:app-window", group: true }, // the taskbar, file managers, file dialogs
        { id: "favorites", title: qsTr("收藏夹"), glyph: "\uE734" }, // FavoriteStar
        { id: "clipboard", title: qsTr("剪贴板"), glyph: "\uE77F" }, // Paste
        { id: "plugins", title: qsTr("插件"), glyph: "" }, // Puzzle
        { id: "ai", title: qsTr("AI 助手"), glyph: "" }, // Robot
        { id: "health", title: qsTr("运行状况"), glyph: "\uE9D9", group: true }, // Diagnostic: pulse
        { id: "gleaning", title: Gleaning.title, gleaning: true } // the author's works, set apart at the bottom of the list
    ]
    // Other names of each page, for the search; ";" between them. A word
    // here brings up the whole page, so never one of its options' words.
    readonly property var pageKeywords: [
        qsTr("通用;基本;关于;其他;更多;高级;general;advanced;about"),
        qsTr("热键;唤出;呼出;打开方式;hotkey;keyboard"),
        qsTr("皮肤;颜色;界面;appearance"),
        qsTr("搜索位置;磁盘;scope"),
        qsTr("搜索结果;结果;列表;results"),
        qsTr("集成;系统;integration"),
        qsTr("快捷访问;常去;quick access"),
        qsTr("剪切板;粘贴板;复制;粘贴;clipboard"),
        qsTr("扩展;脚本;PowerShell;Python;内置功能;模块;网页搜索;搜索引擎;网址;web;plugin;extension;module"),
        qsTr("MCP;人工智能;大模型;Claude;Cursor;ai"),
        qsTr("资源;性能;走势;图表;进程;health;performance"),
        ""
    ]
    // The tabs of the pages too long for one screen, each a few sections
    // (SettingsSection.tab); [] for a page without tabs.
    readonly property var pageTabs: [
        [],
        [qsTr("打开 Win顺", "settings tab"), qsTr("按键一览")],
        [],
        [qsTr("文件夹"), qsTr("名称和类型"), qsTr("磁盘"), qsTr("网络与 WSL"), qsTr("文件内容")],
        [qsTr("排序"), qsTr("显示与预览"), qsTr("操作习惯"), qsTr("类型与筛选")],
        [qsTr("任务栏"), qsTr("文件管理器"), qsTr("文件夹菜单"), qsTr("对话框")],
        [],
        [qsTr("记录"), qsTr("快捷键", "settings tab"), qsTr("保留")],
        window.editor.plugins.developerMode ? [qsTr("已安装"), qsTr("插件库"), qsTr("开发者")] : [qsTr("已安装"), qsTr("插件库")], // 开发者: in developer mode only
        window.editor.mcp ? [qsTr("权限"), qsTr("连接客户端"), qsTr("隐私与记录")] : [], // the others: nothing while off
        [],
        []
    ]
    readonly property int keysListTab: 1 // 快捷键 › 按键一览
    // 按键一览: every shortcut of the search box and the clipboard (Shortcuts),
    // in sections [{title, note, rows: [{keys, text, setting}]}].
    readonly property var shortcutSections: {
        const context = {
            all: true,
            openKeys: [window.editor.doubleCtrl && window.editor.doubleTapAction === "launcher" ? qsTr("双击 %1").arg(window.tapKey) : "", window.editor.hotkey, window.editor.taskbarWinS ? "Win+S" : ""].filter(keys => keys.length > 0).join(" / "),
            clipboardHotkey: [window.editor.doubleCtrl && window.editor.doubleTapAction === "clipboard" ? qsTr("双击 %1").arg(window.tapKey) : "", window.editor.clipboardWinV ? "Win+V" : "", window.editor.clipboardHotkey].filter(keys => keys.length > 0).join(" / "),
            folderMenuHotkey: window.editor.folderMenuHotkey,
            panelHotkey: window.editor.panelHotkey,
            settingsHotkey: window.editor.settingsHotkey,
            pastePlainHotkey: window.editor.pastePlainHotkey,
            previewKey: window.editor.previewHotkey,
            openKey: window.editor.openHotkey,
            historyModifier: !window.editor.recordHistory || window.editor.queryHistoryKeys === "off" ? "" : window.editor.queryHistoryKeys === "alt" ? "Alt" : "Ctrl",
            numberModifier: window.editor.numberKeys === "alt" ? "Alt" : window.editor.numberKeys === "ctrl" ? "Ctrl" : "",
            holdKey: window.editor.clipboardHotkey,
            captureKeys: window.editor.screenshotHotkeys.join(" / "),
            folderTyping: true,
            intoField: false,
            plainByDefault: window.editor.clipboardPlainByDefault
        }
        const sections = []
        for (const where of ["global", "launcher", "clipboard"]) {
            const clipboard = where === "clipboard"
            if (clipboard)
                context.numberModifier = window.editor.clipboardNumberKeys === "alt" ? "Alt" : window.editor.clipboardNumberKeys === "ctrl" ? "Ctrl" : ""
            for (const entry of Shortcuts.entries(where, context, false)) {
                const title = where === "global" ? qsTr("打开 Win顺")
                              : clipboard ? qsTr("剪贴板 · %1").arg(entry.group) : qsTr("搜索框 · %1").arg(entry.group)
                let section = sections.find(candidate => candidate.title === title)
                if (!section) {
                    section = { title: title, rows: [] }
                    sections.push(section)
                }
                section.rows.push(entry)
            }
            if (where === "global") {
                const rows = window.editor.clipboardEntryHotkeys
                    .filter(row => row.hotkey.length > 0)
                    .map(row => ({ keys: row.hotkey, text: qsTr("粘贴“%1”").arg(row.title),
                                   setting: { page: "clipboard", tab: 1, part: "clipEntryHotkeysSection" } }))
                if (rows.length > 0)
                    sections.push({ title: qsTr("剪贴板 · 单条内容"), rows: rows })
            }
        }
        return sections
    }
    // A page by its id ("scope") or its place; -1: no such page.
    function pageIndex(which): int {
        if (typeof which === "number")
            return Number.isInteger(which) && which >= 0 && which < pages.length ? which : -1
        return pages.findIndex(page => page.id === which)
    }
    readonly property int generalPage: pageIndex("general") // 常规: updates, starting with Windows, backups, diagnostics; where the updates are
    readonly property int keysPage: pageIndex("keys") // 快捷键: opening Win顺, 按键一览
    readonly property int appearancePage: pageIndex("appearance")
    readonly property int scopePage: pageIndex("scope") // 搜索范围, file contents too
    readonly property int resultsPage: pageIndex("results") // 结果列表, the usage history too
    readonly property int integrationPage: pageIndex("integration") // 系统整合: the taskbar, file managers, folder menu, file dialogs
    readonly property int favoritesPage: pageIndex("favorites") // 收藏夹 (FavoritesSection)
    readonly property int clipboardPage: pageIndex("clipboard")
    readonly property int pluginsPage: pageIndex("plugins") // 插件: 已安装 (PluginsSection), 插件库 (PluginGallerySection), 开发者 (PluginDevSection)
    readonly property int aiPage: pageIndex("ai") // AI 助手: the MCP server and the command line (AiAccessSection, AiCommandLineSection, AiClientsSection, AiPrivacySection)
    readonly property int healthPage: pageIndex("health") // 运行状况 (HealthPage)
    readonly property int gleaningPage: pageIndex("gleaning")
    readonly property int advancedPage: generalPage // compatibility for preview clients: where the updates are
    readonly property int webPage: pluginsPage // compatibility for preview clients
    readonly property string tapKey: editor.doubleTapKey === "alt" ? "Alt" : editor.doubleTapKey === "shift" ? "Shift" : "Ctrl" // the key tapped twice to open Win顺
    property int webEditing: -2
    property Item webReturnFocus: null
    function editWebShortcut(index, button) {
        webReturnFocus = button
        webEditing = index
    }
    function closeWebForm() {
        webEditing = -2
        const parts = pageItem(pluginsPage)?.parts
        const card = currentTab === 1
            ? parts?.pluginGallerySection?.cardOf("winshun.web")
            : parts?.pluginsSection?.cardOf("winshun.web") // the form opens from either 插件 tab
        const button = webReturnFocus || card?.webOptions?.addButton
        if (button && button.visible)
            button.forceActiveFocus()
        webReturnFocus = null
    }
    Connections {
        target: window.editor.plugins
        function onWebSettingsRequested() { window.showWebPage() }
    }
    property int currentPage: 0
    // A category chosen in the list whose page is not made yet: the list
    // shows the choice at once, the page follows a frame later (navigate). -1: none.
    property int navPending: -1
    readonly property int navPage: navPending >= 0 ? navPending : currentPage
    property int navHovered: -1 // the category under the pointer (hoverNav)
    property int currentTab: 0
    property var lastTabs: [] // by page: the tab it was left on, to come back to
    // A tab gone from the page on show (its switch turned off): the last one left.
    onPageTabsChanged: {
        const clamped = Math.min(currentTab, Math.max(0, (pageTabs[currentPage]?.length ?? 0) - 1))
        if (clamped !== currentTab) {
            currentTab = clamped
            if (!navHistory.restoring && navHistory.count > 0 && !searching)
                navHistory.replaceCurrent(pages[currentPage].id, currentTab, activePluginDetailId(), "", currentScrollY())
        }
    }
    // On the 拾穗计划 page the whole window, sidebar included, is a warm scene.
    // Not while searching: the results are on the usual page.
    readonly property bool warm: currentPage === gleaningPage && !searching
    readonly property bool searching: settingsSearch.active
    readonly property bool modalOpen: updateDialog.open || readmeDialog.open || notesDialog.open
                                      || pluginDialog.open || donateDialog.open || webDialog.active
    readonly property bool pluginDetailActive: !searching && currentPage === pluginsPage
                                               && ((currentTab === 0 && (pluginsSlot.item?.parts.pluginsSection.detailOpen ?? false))
                                                   || (currentTab === 1 && (pluginsSlot.item?.parts.pluginGallerySection.detailOpen ?? false)))
    readonly property bool canNavigateBack: navHistory.canBack || searching
    readonly property bool canNavigateForward: navHistory.canForward
    readonly property string backTargetLabel: searching ? placeLabel(navHistory.currentEntry)
                                                        : placeLabel(navHistory.backEntry)
    readonly property string forwardTargetLabel: placeLabel(navHistory.forwardEntry)
    property var beforeSearch: null // {page, tab, y}: where the search started, to go back to when the box is cleared
    property bool leavingSearch: false // for another page: no going back
    property bool programmaticSearch: false // showSetting's internal lookup: not a user search history step
    property bool scrollToBottomAfterFrame: false // upward edge-overscroll into a freshly loaded page

    width: 920
    height: 720
    minimumWidth: 760
    minimumHeight: 420
    // Mica follows activation; liquid Acrylic keeps its background when inactive.
    // Use transparent rather than a translucent clear colour (Qt clears it
    // unpremultiplied, which makes a light colour appear white).
    // Opened to be put in front (App::showSettings): drawn as an active window
    // from its first frame, not one frame as an inactive one and then changed.
    // Until it is active, or for a second should it never be (another window kept the front).
    property bool arriving: false
    Timer { interval: 1000; running: window.arriving; onTriggered: window.arriving = false }
    color: Theme.backdrop && SystemTheme.materials && (window.active || window.arriving || SystemTheme.uiStyle === "liquid") ? "transparent" : Theme.page
    title: qsTr("设置") // shown as "设置 - Win顺"
    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)

    // Forcing it onto contentItem would hand it straight back to the item
    // inside that has it (a hotkey recorder would go on recording).
    function clearFocus() {
        if (window.activeFocusItem && window.activeFocusItem !== window.contentItem)
            window.activeFocusItem.focus = false
    }

    // The taskbar button is pinned, Windows' search hidden, a file manager
    // installed, outside Win顺: looked at again when the user comes back
    // (SettingsEditor::windowActivated; on opening, after the first frame).
    onActiveChanged: {
        if (!active)
            return
        arriving = false
        editor.windowActivated()
    }

    // Closed (the title bar, Alt+F4, the taskbar): hidden and kept, not
    // destroyed; App::showSettings shows the same window again, at once.
    onClosing: (close) => {
        close.accepted = false
        window.hide()
    }

    function closePopups() {
        // Dropped menus and hover tips live on the content item (DropMenu, HoverTip).
        for (const child of contentItem.children) {
            if (child.open === true && child.entries !== undefined && child.anchor !== undefined)
                child.open = false
            else if (child.shown === true && child.shownText !== undefined)
                child.shown = false
        }
    }

    // Hidden: back as a window made anew would be, nothing left running or
    // half done (the hotkey recorders above all: one recording keeps the
    // global hotkeys suspended).
    function resetForReopen() {
        clearFocus()
        fillTimer.stop()
        navPrefetch.stop()
        navPending = -1
        navHovered = -1
        edgeGate.cancel()
        navHistory.clear()
        // Each page made so far its own (its recorders stopped, its fields cleared, …).
        for (let i = 0; i < pageSlots.length; ++i)
            pageSlots[i].item?.resetForReopen()
        editor.plugins.recordingHotkey = false
        updateDialog.close()
        readmeDialog.close()
        notesDialog.close()
        if (pluginDialog.open)
            pluginDialog.reject()
        donateDialog.close()
        webEditing = -2
        webReturnFocus = null
        closePopups()
        uiSizeToast.dismiss()
        editor.plugins.gallery.filter = ""
        editor.plugins.gallery.updatesOnly = false
        editor.forgetMessages()

        leavingSearch = true
        searchBox.clear()
        leavingSearch = false
        programmaticSearch = false
        beforeSearch = null
        settingsSearch.current = null
        activatePending = false
        // To the first page as showPage would go, but without making it now:
        // it is made when the window is shown again (onVisibleChanged), unless
        // that opening goes elsewhere.
        currentPage = 0
        currentTab = 0
        flick.stopScrolling()
        flick.contentY = 0
        lastTabs = []
        heldTimer.stop()
        heldItem = null
        revealAfterFrame = ""
        scrollAfterFrame = null
        scrollToBottomAfterFrame = false
        fadeOut.stop()
        snapshot.source = ""
        navFlick.contentY = 0
        gleaningView.contentY = 0
    }

    // App::showSettings, after the first frame of each opening: what may have
    // changed while the window was hidden, read again.
    function refreshAfterOpen() {
        for (let i = 0; i < pageSlots.length; ++i)
            pageSlots[i].item?.opened()
        // The pages not looked at stay unmade: each costs ~20 MB, all ten
        // ~250 MB (2026-11-06). Made when gone to, or for the search (fillFast).
    }

    // ---- The pages, made when needed (PageSlot) -----------------------------
    // Opening makes only the page shown (onVisibleChanged). Whatever needs a
    // page not made yet makes it at once: going to it (showPage, part),
    // showSetting (loadAllPages). The search waits for them all (searchReady),
    // never makes them all at once: the box taking the keyboard starts making
    // the others one at a time between frames (fillTimer, fillFast), then
    // 按键一览's sections one by one; something typed makes the rest one per
    // turn (fillNow).
    property bool allPagesLoaded: false
    readonly property list<PageSlot> pageSlots: [generalSlot, keysSlot, appearanceSlot, scopeSlot, resultsSlot, integrationSlot,
        favoritesSlot, clipboardSlot, pluginsSlot, aiSlot, healthSlot] // in the order of `pages`
    // 按键一览's sections made so far (its Repeater's model, one element each).
    ListModel { id: shortcutModel }
    function makeShortcuts(count: int): void {
        const wanted = Math.min(count, shortcutSections.length)
        while (shortcutModel.count < wanted)
            shortcutModel.append({ made: true })
    }
    onShortcutSectionsChanged: {
        if (shortcutModel.count > shortcutSections.length)
            shortcutModel.remove(shortcutSections.length, shortcutModel.count - shortcutSections.length)
        else if (shortcutsWanted)
            makeShortcuts(shortcutSections.length)
    }
    // 按键一览 shown: its sections all made at once (else one at a time, filling).
    property bool shortcutsWanted: false
    onShortcutsWantedChanged: if (shortcutsWanted) makeShortcuts(shortcutSections.length)
    onCurrentPageChanged: if (currentPage === keysPage && currentTab === keysListTab) shortcutsWanted = true
    onCurrentTabChanged: if (currentPage === keysPage && currentTab === keysListTab) shortcutsWanted = true

    // Closed a while ago (App, half a minute after the window was hidden): the
    // pages made for that opening go, one per turn of the event loop, all but
    // the first, which the next opening shows. Every page looked at, and all
    // of them once the search was used, stayed for as long as Win顺 ran:
    // ~250 MB at 150% (GitHub #459, 587 MB of 窗口和数据 on a larger screen).
    function unloadPages(): void {
        if (visible)
            return
        for (let i = pageSlots.length - 1; i > 0; --i) {
            if (pageSlots[i].unload()) {
                allPagesLoaded = false
                Qt.callLater(window.unloadPages)
                return
            }
        }
        if (keysSlot.item === null) { // 按键一览's sections went with their page: made again as on a first opening
            shortcutsWanted = false
            shortcutModel.clear()
        }
        gc() // the pages' JavaScript side
    }

    // The page if it has been made; null if not (or no such page).
    function pageItem(index: int): SettingsPage {
        return index >= 0 && index < pageSlots.length ? pageSlots[index].item : null
    }
    // The page, made now if it is not yet; null for 拾穗计划 (GleaningPage), no slot.
    function loadPage(index: int): SettingsPage {
        if (index < 0 || index >= pageSlots.length)
            return null
        const made = pageSlots[index].load()
        if (!allPagesLoaded) {
            let all = true
            for (let i = 0; i < pageSlots.length && all; ++i)
                all = pageSlots[i].item !== null
            allPagesLoaded = all
        }
        return made
    }
    // The first page (in the categories' order) neither made nor being made; -1: none.
    function nextToMake(): int {
        for (let i = 0; i < pageSlots.length; ++i) { // a list property: no Array.prototype.find
            if (pageSlots[i].item === null && pageSlots[i].incubator === null)
                return i
        }
        return -1
    }
    // Everything made now (showSetting, which looks a row up at once).
    function loadAllPages() {
        makeShortcuts(shortcutSections.length) // before allPagesLoaded: the search finds them too
        for (let i = 0; i < pageSlots.length; ++i)
            pageSlots[i].load()
        fillTimer.stop()
        allPagesLoaded = true
    }
    // Every page and 按键一览's sections made: the search may look (its query
    // is the box's from then on, worked out over all of them at once).
    readonly property bool searchReady: allPagesLoaded && shortcutModel.count >= shortcutSections.length
    // The box has the keyboard: the filling as fast as it goes in the background
    // (the next page as soon as one is made).
    readonly property bool fillFast: !searchReady && (searchBox.editing || searchBox.query.length > 0)
    onFillFastChanged: {
        if (fillFast && visible) {
            fillInBackground()
            Qt.callLater(window.fillStep)
        }
    }
    // The pages not made yet, one after another in the background (fillTimer;
    // it stops once all are made, or the window is hidden).
    function fillInBackground(): void {
        if (visible && !searchReady)
            fillTimer.start()
    }
    // Something typed waits for searchReady: the rest made now, a page (or a
    // few of 按键一览's sections) per turn of the event loop (fillNow), keys
    // and frames in between; sooner than the background's few ms per frame.
    readonly property bool fillWaited: !searchReady && searchBox.query.length > 0
    onFillWaitedChanged: if (fillWaited && visible) Qt.callLater(window.fillNow)
    function fillNow(): void {
        if (!fillWaited || !visible)
            return
        for (let i = 0; i < pageSlots.length; ++i) {
            if (pageSlots[i].incubator !== null) { // being made in the background: finished now
                loadPage(i)
                Qt.callLater(window.fillNow)
                return
            }
        }
        const next = nextToMake()
        if (next >= 0) {
            loadPage(next)
            Qt.callLater(window.fillNow)
            return
        }
        allPagesLoaded = true
        if (shortcutModel.count < shortcutSections.length) {
            makeShortcuts(shortcutModel.count + 3)
            Qt.callLater(window.fillNow)
        }
    }
    // Enter pressed before searchReady: done once the results are there.
    property bool activatePending: false
    // A row or section of a page (its index or id) by name (SettingsPage.parts), its page made if need be.
    function part(which, name) {
        const index = pageIndex(which)
        const made = loadPage(index)
        const found = made ? made.parts[name] : undefined
        if (!found)
            console.warn("settings: no part", name, "on page", index)
        return found ?? null
    }
    Timer {
        id: fillTimer
        interval: window.fillFast ? 16 : 80
        repeat: true
        onTriggered: window.fillStep()
    }
    // One page at a time, made in the background a few milliseconds per frame
    // (PageSlot.prefetch: the engine's incubation), not 20–60 ms at once; none
    // started while the page is being scrolled. Then 按键一览's sections, one
    // per step (a few rows each).
    function fillStep(): void {
        if (!visible || searchReady) {
            fillTimer.stop()
            return
        }
        if (fillWaited) // fillNow's
            return
        for (let i = 0; i < pageSlots.length; ++i) {
            if (pageSlots[i].incubator !== null)
                return
        }
        const next = nextToMake()
        if (next >= 0) {
            if (!flick.moving && !flick.flicking && !wheelScroll.running)
                pageSlots[next].prefetch(window.pageMade)
            return
        }
        allPagesLoaded = true // the last one made since
        if (shortcutModel.count < shortcutSections.length)
            makeShortcuts(shortcutModel.count + 1)
        if (searchReady)
            fillTimer.stop()
    }
    // A page made in the background: the next one at once while fillFast.
    function pageMade(): void {
        if (fillFast && !fillWaited && fillTimer.running)
            Qt.callLater(window.fillStep)
    }

    // Settings::fileManager's names, as the user knows them.
    function fileManagerName(name: string): string {
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

    // ---- Navigation history & edge overscroll -------------------------------
    SettingsHistory {
        id: navHistory
        objectName: "settingsHistory"
    }

    EdgeScrollGate {
        id: edgeGate
        objectName: "settingsEdgeScrollGate"
        sensitivity: window.editor.settingsEdgeScroll
        enabled: window.visible && window.editor.settingsEdgeScroll !== "off" && !window.searching && !window.modalOpen
                 && !window.editor.recordingHotkey && !window.pluginDetailActive
        canPrevious: window.adjacentView(-1) !== null
        canNext: window.adjacentView(1) !== null
        onArmedDirectionChanged: (dir) => {
            if (dir === 0)
                return
            const target = window.adjacentView(dir)
            if (target && target.page !== window.currentPage && target.page < window.pageSlots.length) {
                const slot = window.pageSlots[target.page]
                if (slot && !slot.item && !slot.incubator)
                    slot.prefetch(null)
            }
        }
        onTriggered: (dir) => window.stepAdjacentView(dir)
    }
    // Subtle elastic pull on the scrolled content while pushing past a boundary;
    // strictly 0 at rest so ScrollPixelAlignment keeps resting text razor-sharp.
    readonly property real edgePullY: edgeGate.direction !== 0
                                      ? -edgeGate.direction * Theme.snap(7 * (1 - Math.pow(1 - edgeGate.progress, 2)), window.dpr)
                                      : 0

    Connections {
        target: window.frame
        function onNavigateBack() { window.navigateBack() }
        function onNavigateForward() { window.navigateForward() }
    }
    Component.onCompleted: window.frame.browserNavigation = true

    function currentScrollY() {
        return currentPage === gleaningPage && !searching ? gleaningView.contentY : flick.contentY
    }

    function activePluginDetailId() {
        if (searching || currentPage !== pluginsPage)
            return ""
        if (currentTab === 0) {
            const section = pageItem(pluginsPage)?.parts.pluginsSection
            return section && section.detailOpen && section.expandedIds.length > 0 ? section.expandedIds[0] : ""
        }
        if (currentTab === 1) {
            const section = pageItem(pluginsPage)?.parts.pluginGallerySection
            return section && section.detailOpen ? section.detailPluginId : ""
        }
        return ""
    }

    function recordPlace(scrollY) {
        if (navHistory.restoring)
            return
        const pid = pages[currentPage]?.id ?? "general"
        const y = scrollY !== undefined ? scrollY : currentScrollY()
        if (!visible)
            navHistory.reset(pid, currentTab, activePluginDetailId(), "", y)
        else
            navHistory.push(pid, currentTab, activePluginDetailId(), "", y)
    }

    function placeLabel(entry) {
        if (!entry)
            return ""
        const q = entry.query ?? ""
        if (q.length > 0)
            return qsTr("搜索“%1”").arg(q.replace(/\s+/g, " ").trim())
        const index = pageIndex(entry.pageId ?? "")
        if (index < 0)
            return ""
        const title = pages[index].title
        const pid = entry.pluginId ?? ""
        if (index === pluginsPage && ((entry.tab ?? 0) === 0 || (entry.tab ?? 0) === 1) && pid.length > 0) {
            const card = pageItem(pluginsPage)?.parts.pluginsSection.cardOf(pid)
            return title + " › " + (card?.name ?? pid)
        }
        const tabs = pageTabs[index] ?? []
        const tabIdx = entry.tab ?? 0
        if (tabs.length > 0 && tabIdx >= 0 && tabIdx < tabs.length)
            return title + " › " + tabs[tabIdx]
        return title
    }

    function placeIcon(entry) {
        if (!entry)
            return { iconName: "", glyph: "" }
        if ((entry.query ?? "").length > 0)
            return { iconName: "magnifying-glass", glyph: "" }
        const index = pageIndex(entry.pageId ?? "")
        if (index < 0)
            return { iconName: "", glyph: "" }
        if (pages[index].gleaning === true)
            return { iconName: "sparkle", glyph: "" }
        return { iconName: "", glyph: pages[index].glyph ?? "" }
    }

    function backMenuEntries() {
        const list = navHistory.backEntries
        if (searching && navHistory.count > 0)
            return [navHistory.currentEntry].concat(list)
        return list
    }

    function openHistoryMenu(anchorButton, rawEntries) {
        if (!rawEntries || rawEntries.length === 0)
            return
        if (historyMenu.open && historyMenu.anchor === anchorButton) {
            historyMenu.open = false
            return
        }
        const items = []
        const indices = []
        for (const entry of rawEntries) {
            const icon = placeIcon(entry)
            items.push({ text: placeLabel(entry), iconName: icon.iconName, glyph: icon.glyph })
            indices.push(entry.historyIndex)
        }
        historyMenu.anchor = anchorButton
        historyMenu.entries = items
        historyMenu.historyIndices = indices
        historyMenu.open = true
    }

    function restorePlace(entry) {
        if (!entry || (!entry.pageId && !entry.query))
            return
        clearFocus()
        closePopups()
        navPrefetch.stop()
        navPending = -1
        edgeGate.cancel()
        scrollAfterFrame = null
        scrollToBottomAfterFrame = false
        navHistory.setRestoring(true)
        try {
            const targetQuery = entry.query ?? ""
            if (targetQuery.length > 0) {
                loadAllPages()
                const originIndex = pageIndex(entry.pageId ?? "")
                beforeSearch = originIndex >= 0
                    ? { page: originIndex, tab: entry.tab ?? 0, y: 0 }
                    : null
                searchBox.setText(targetQuery)
                layoutNow()
                flick.stopScrolling()
                const maxY = Math.max(0, flick.contentHeight - flick.height)
                flick.contentY = Math.max(0, Math.min(maxY, entry.scrollY ?? 0))
                edgeGate.noteSwitched(0)
                return
            }
            if (searching) {
                leavingSearch = true
                searchBox.clear()
                leavingSearch = false
            }
            beforeSearch = null
            const index = pageIndex(entry.pageId)
            if (index < 0)
                return
            if (index !== gleaningPage)
                loadPage(index)
            lastTabs[currentPage] = currentTab
            currentPage = index
            const maxTab = Math.max(0, (pageTabs[index]?.length ?? 0) - 1)
            currentTab = Math.max(0, Math.min(entry.tab ?? 0, maxTab))
            if (index === pluginsPage && currentTab === 0) {
                const section = pageItem(pluginsPage)?.parts.pluginsSection
                if (section) {
                    const targetPlugin = entry.pluginId ?? ""
                    if (targetPlugin.length > 0 && section.cardOf(targetPlugin))
                        section.open(targetPlugin)
                    else
                        section.expandedIds = []
                }
            } else if (index === pluginsPage && currentTab === 1) {
                const section = pageItem(pluginsPage)?.parts.pluginGallerySection
                if (section) {
                    const targetPlugin = entry.pluginId ?? ""
                    if (targetPlugin.length > 0 && window.editor.plugins.hasPlugin(targetPlugin))
                        section.open(targetPlugin)
                    else
                        section.closeDetail()
                }
            }
            if (index === gleaningPage) {
                gleaningView.cancelFlick()
                const maxGY = Math.max(0, gleaningView.contentHeight - gleaningView.height)
                gleaningView.contentY = Math.max(0, Math.min(maxGY, entry.scrollY ?? 0))
            } else {
                flick.stopScrolling()
                layoutNow()
                const maxY = Math.max(0, flick.contentHeight - flick.height)
                flick.contentY = Math.max(0, Math.min(maxY, entry.scrollY ?? 0))
            }
            const item = navItemAt(currentPage)
            if (item)
                revealNavItem(item)
            edgeGate.noteSwitched(0)
        } finally {
            navHistory.setRestoring(false)
        }
    }

    function navigateBack() {
        if (modalOpen || editor.recordingHotkey)
            return
        if (searching) {
            const activeQuery = searchBox.query
            const searchY = flick.contentY
            const originPage = beforeSearch ? beforeSearch.page : currentPage
            const originTab = beforeSearch ? beforeSearch.tab : currentTab
            if (beforeSearch)
                navHistory.updateScroll(beforeSearch.y)
            if (activeQuery.trim().length > 0) {
                navHistory.push(pages[originPage]?.id ?? "general", originTab, "", activeQuery, searchY)
                restorePlace(navHistory.back())
            } else {
                restorePlace(navHistory.currentEntry)
            }
            return
        }
        if (!navHistory.canBack)
            return
        navHistory.updateScroll(currentScrollY())
        restorePlace(navHistory.back())
    }

    function navigateForward() {
        if (modalOpen || editor.recordingHotkey || !navHistory.canForward)
            return
        if (!searching)
            navHistory.updateScroll(currentScrollY())
        else if (beforeSearch)
            navHistory.updateScroll(beforeSearch.y)
        restorePlace(navHistory.forward())
    }

    function navigateToHistory(historyIndex) {
        if (modalOpen || editor.recordingHotkey)
            return
        if (searching && historyIndex === navHistory.currentIndex) {
            navigateBack()
            return
        }
        if (!searching)
            navHistory.updateScroll(currentScrollY())
        else if (beforeSearch)
            navHistory.updateScroll(beforeSearch.y)
        const target = navHistory.jumpTo(historyIndex)
        if (target && (target.pageId || target.query))
            restorePlace(target)
    }

    // Linear adjacent view for boundary-overscroll navigation: steps through
    // each page's sub-tabs in order, then moves to the neighbouring category.
    function adjacentView(direction) {
        if (searching || modalOpen || editor.recordingHotkey || pluginDetailActive)
            return null
        const tabs = pageTabs[currentPage]?.length ?? 0
        if (direction > 0) {
            if (tabs > 1 && currentTab + 1 < tabs)
                return { page: currentPage, tab: currentTab + 1 }
            if (currentPage + 1 < pages.length)
                return { page: currentPage + 1, tab: 0 }
            return null
        }
        if (tabs > 1 && currentTab > 0)
            return { page: currentPage, tab: currentTab - 1 }
        if (currentPage > 0) {
            const prev = currentPage - 1
            const prevTabs = pageTabs[prev]?.length ?? 0
            return { page: prev, tab: Math.max(0, prevTabs - 1) }
        }
        return null
    }

    function adjacentLabel(target) {
        if (!target)
            return ""
        const tabs = pageTabs[target.page] ?? []
        if (target.page === currentPage && target.tab >= 0 && target.tab < tabs.length)
            return tabs[target.tab]
        const base = pages[target.page]?.title ?? ""
        if (tabs.length > 0 && target.tab >= 0 && target.tab < tabs.length)
            return base + " › " + tabs[target.tab]
        return base
    }

    function stepAdjacentView(direction) {
        const target = adjacentView(direction)
        if (!target)
            return
        clearFocus()
        closePopups()
        const wasLoaded = target.page === gleaningPage || pageItem(target.page) !== null
        if (target.page === currentPage)
            showTab(target.tab)
        else
            showPage(target.page, target.tab)
        if (direction < 0) {
            // Crossing upward past the top lands at the bottom of the previous view.
            if (currentPage === gleaningPage) {
                gleaningView.cancelFlick()
                gleaningView.contentY = Math.max(0, gleaningView.contentHeight - gleaningView.height)
            } else {
                flick.stopScrolling()
                layoutNow()
                flick.contentY = Math.max(0, flick.contentHeight - flick.height)
                if (!wasLoaded)
                    scrollToBottomAfterFrame = true
            }
            navHistory.updateScroll(currentScrollY())
        }
        const item = navItemAt(currentPage)
        if (item)
            revealNavItem(item)
        edgeGate.noteSwitched(direction)
    }

    // To a page (its index or id), on `tab` if given; else on the tab it was left on.
    function showPage(which, tab) {
        const index = pageIndex(which)
        if (index < 0) {
            console.warn("settings: no page", which)
            return
        }
        navPending = -1
        edgeGate.cancel()
        scrollToBottomAfterFrame = false
        const wasSearching = searching
        if (!navHistory.restoring && visible) {
            if (wasSearching && beforeSearch)
                navHistory.updateScroll(beforeSearch.y)
            else if (!wasSearching)
                navHistory.updateScroll(currentScrollY())
        }
        if (index !== gleaningPage)
            loadPage(index) // made now if it is not yet: in this frame, not after it
        if (searching) {
            leavingSearch = true
            searchBox.clear()
            leavingSearch = false
        }
        lastTabs[currentPage] = currentTab
        currentPage = index
        // A tab that has gone since (大小上限 with content search off): the first.
        currentTab = Math.min(tab ?? lastTabs[index] ?? 0, Math.max(0, (pageTabs[index]?.length ?? 0) - 1))
        if (index === gleaningPage) {
            gleaningView.cancelFlick()
            gleaningView.contentY = 0
        } else {
            flick.stopScrolling()
            flick.contentY = 0
        }
        recordPlace(0)
        if (visible && !navHistory.restoring)
            edgeGate.noteSwitched(0)
    }

    // A category chosen in the list (a press, ↑ ↓, Enter). Its page at once
    // if it is made. If not (20–120 ms to make one never shown, measured
    // 2026-11-19), the list shows the choice in the next frame and the page
    // is made right after, instead of nothing moving until it is there.
    function navigate(index: int): void {
        navPrefetch.stop()
        if (index === gleaningPage || searching || pageItem(index) !== null) {
            showPage(index)
            return
        }
        navPending = index
        navFrames.frames = 0
    }
    FrameAnimation { // navPending's page made on the second tick: a frame with the choice has been synced in between
        id: navFrames
        property int frames: 0
        running: window.navPending >= 0
        onTriggered: {
            if (++frames < 2)
                return
            const index = window.navPending
            window.navPending = -1
            if (!window.searching) // typed into the box meanwhile: the search's
                window.showPage(index)
        }
    }
    // The pointer resting on a category whose page is not made: the page made
    // meanwhile in the background (PageSlot.prefetch, a few ms a frame), so
    // that the press finds it made, or nearly. Not while the pointer only
    // passes over the list; only the pages pointed at (each ~20 MB).
    function hoverNav(index: int, on: bool): void {
        if (!on) {
            if (navHovered === index) {
                navHovered = -1
                navPrefetch.stop()
            }
            return
        }
        navHovered = index
        const slot = index < pageSlots.length ? pageSlots[index] : null
        if (slot && !slot.item && !slot.incubator && index !== navPending)
            navPrefetch.restart()
        else
            navPrefetch.stop()
    }
    Timer {
        id: navPrefetch
        interval: 60
        onTriggered: {
            const index = window.navHovered
            if (window.visible && index >= 0 && index < window.pageSlots.length && index !== window.navPending)
                window.pageSlots[index].prefetch(null)
        }
    }
    // That page made faster than the window's own pace meanwhile.
    readonly property bool navHurry: visible && navHovered >= 0 && navHovered < pageSlots.length
                                     && pageSlots[navHovered].incubator !== null
    onNavHurryChanged: editor.hurryIncubation(window, navHurry)

    function showTab(tab) {
        if (tab === currentTab)
            return
        edgeGate.cancel()
        scrollToBottomAfterFrame = false
        if (!navHistory.restoring && visible && !searching)
            navHistory.updateScroll(currentScrollY())
        currentTab = tab
        flick.stopScrolling()
        flick.contentY = 0
        recordPlace(0)
        if (visible && !navHistory.restoring)
            edgeGate.noteSwitched(0)
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
        // Only once every page is made (the box makes them: onQueryChanged
        // there), so that each row is there to be matched and counted.
        query: window.searchReady ? searchBox.query : ""
        highlightColor: Theme.matchColor
        onQueryChanged: {
            if (active && !window.beforeSearch)
                window.beforeSearch = { page: window.currentPage, tab: window.currentTab, y: window.currentScrollY() }
        }
        onQueryApplied: (wasActive) => window.searchChanged(wasActive)
    }

    // What the pages read from the window (SettingsPage), and ask of it.
    SettingsContext {
        id: settingsContext
        search: settingsSearch
        dpr: window.dpr
        currentPage: window.currentPage
        currentTab: window.currentTab
        pages: window.pages
        pageKeywords: window.pageKeywords
        onOpenRequested: (which, tab) => window.showPage(which, tab)
        onPartRequested: (which, name) => window.showPart(which, name)
        onHoldRequested: (item, change) => window.holdInView(item, change)
    }

    function searchChanged(wasActive) {
        if (searching) {
            edgeGate.cancel()
            flick.stopScrolling()
            flick.contentY = 0
            pickBest()
            if (activatePending) {
                activatePending = false
                Qt.callLater(window.goToCurrent) // not inside the search's own signal
            }
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
        if (currentPage === gleaningPage) {
            gleaningView.cancelFlick()
            gleaningView.contentY = Math.max(0, Math.min(gleaningView.contentHeight - gleaningView.height, before.y))
        } else {
            layoutNow()
            flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, before.y))
        }
        edgeGate.noteSwitched(0)
    }

    // The rows on show, in the order they stand, with their scores.
    function results() {
        const list = []
        for (const result of settingsSearch.results())
            list.push({ row: result.row, score: result.score, key: (result.row.section?.page ?? 0) * 1000000 + result.row.orderKey() }) // orderKey: within its page
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
            if (delta > 0 && searchBox.query.length === 0) // typed, not searched yet (searchReady): nothing
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
        const wasSearching = searching
        const activeQuery = wasSearching ? searchBox.query : ""
        const searchY = flick.contentY
        const originPage = beforeSearch ? beforeSearch.page : currentPage
        const originTab = beforeSearch ? beforeSearch.tab : currentTab
        const originY = beforeSearch ? beforeSearch.y : currentScrollY()
        if (!navHistory.restoring && visible) {
            if (wasSearching && !programmaticSearch && activeQuery.trim().length > 0) {
                navHistory.updateScroll(originY)
                navHistory.push(pages[originPage]?.id ?? "general", originTab, "", activeQuery, searchY)
            } else if (!wasSearching) {
                navHistory.updateScroll(currentScrollY())
            }
        }
        row.revealRequested()
        navHistory.setRestoring(true)
        try {
            showPage(row.section.page, row.section.tab)
            layoutNow()
            scrollToRow(row)
        } finally {
            navHistory.setRestoring(false)
        }
        recordPlace(flick.contentY)
        if (visible)
            edgeGate.noteSwitched(0)
        row.focusControl() // a switch; else the box keeps the keyboard, for the next search
        row.flash()
    }

    // Jumps to a named row or section on a page (SettingsPage.parts): switches
    // page and tab, scrolls it into view (again after a frame if the page was
    // just made so wrapped heights have settled), and flashes it.
    function showPart(which, name) {
        const index = pageIndex(which)
        const wasLoaded = pageItem(index) !== null
        const target = part(which, name)
        if (!target) {
            if (index >= 0)
                showPage(index)
            return
        }
        if (target.section) {
            goTo(target)
        } else {
            if (!navHistory.restoring && visible) {
                if (searching && beforeSearch)
                    navHistory.updateScroll(beforeSearch.y)
                else if (!searching)
                    navHistory.updateScroll(currentScrollY())
            }
            navHistory.setRestoring(true)
            try {
                showPage(target.page ?? index, target.tab)
                layoutNow()
                scrollToRow(target)
            } finally {
                navHistory.setRestoring(false)
            }
            recordPlace(flick.contentY)
            if (visible)
                edgeGate.noteSwitched(0)
        }
        if (!wasLoaded)
            scrollAfterFrame = target
    }

    // From the top of the page while the row shows there whole; else with
    // the row near the top.
    function scrollToRow(row) {
        const top = row.mapToItem(flick.contentItem, 0, 0).y
        flick.contentY = top + row.height + 24 <= flick.height
                         ? 0 : Math.max(0, Math.min(flick.contentHeight - flick.height, top - 24))
    }

    // Tab (or a click) to a control that is not all in view: the page follows it.
    onActiveFocusItemChanged: {
        for (let at = activeFocusItem; at; at = at.parent) {
            if (at === page) {
                revealRow(activeFocusItem)
                return
            }
        }
    }

    // Scrolls the page just enough for the row to be in view.
    function revealRow(row) {
        flick.stopScrolling()
        const top = row.mapToItem(flick.contentItem, 0, 0).y
        const bottom = top + row.height
        if (row.height > flick.height - 72) {
            flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, top - 12))
            return
        }
        if (top - 12 < flick.contentY) // the page's name and tabs are above the view (pageHeader), not in it
            flick.contentY = Math.max(0, top - 12)
        else if (bottom + 24 > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.contentHeight - flick.height, bottom + 24 - flick.height)
    }

    // Keeps `item` where it stands on screen while `change` rearranges what is above it
    // (a switch turned on shows rows, and the page tabs), rather than letting the page
    // push it out of the window from under the pointer. The rows come in with an
    // animation, so it holds for a moment after (heldItem), not just at the change;
    // scrolling by hand lets go.
    property Item heldItem: null
    property real heldTop: 0
    function holdInView(item, change) {
        flick.stopScrolling()
        heldItem = item
        heldTop = item.mapToItem(flick, 0, 0).y
        change()
        layoutNow()
        keepHeld()
        heldTimer.restart()
    }
    function keepHeld() {
        if (!heldItem)
            return
        const shift = heldItem.mapToItem(flick, 0, 0).y - heldTop
        if (Math.abs(shift) >= 0.5)
            flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + shift))
    }
    Timer {
        id: heldTimer
        interval: 800
        onTriggered: window.heldItem = null
    }
    Connections {
        target: page
        enabled: window.heldItem !== null
        function onImplicitHeightChanged() { window.keepHeld() }
    }
    Connections {
        target: flick
        enabled: window.heldItem !== null
        function onMovementStarted() { window.heldItem = null }
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
        enabled: !window.editor.recordingHotkey && !window.modalOpen
        onActivated: searchBox.focusAll()
    }
    Shortcut { // Alt+Left / browser Back key: step back in navigation history
        sequences: ["Alt+Left", "Back"]
        enabled: !window.editor.recordingHotkey && !window.modalOpen && window.canNavigateBack
        onActivated: window.navigateBack()
    }
    Shortcut { // Alt+Right / browser Forward key: step forward in navigation history
        sequences: ["Alt+Right", "Forward"]
        enabled: !window.editor.recordingHotkey && !window.modalOpen && window.canNavigateForward
        onActivated: window.navigateForward()
    }
    Shortcut { // Ctrl+Tab, Ctrl+Shift+Tab: through the page's tabs
        sequences: [StandardKey.NextChild]
        enabled: !window.searching && !window.warm && !window.editor.recordingHotkey && !window.modalOpen
        onActivated: window.cycleTab(1)
    }
    Shortcut {
        // Not StandardKey.PreviousChild: its Ctrl+Shift+Backtab never matches
        // on Windows (Qt 6.12), the written-out keys do.
        sequences: ["Ctrl+Shift+Tab"]
        enabled: !window.searching && !window.warm && !window.editor.recordingHotkey && !window.modalOpen
        onActivated: window.cycleTab(-1)
    }

    function stepUiSize(delta) {
        const result = window.editor.stepUiSize(delta)
        uiSizeToast.present(result.message, result.level, result.count)
    }
    Shortcut { // Ctrl++ / Ctrl+=: larger interface size
        sequences: SearchKeys.zoomInSequences
        enabled: !window.editor.recordingHotkey && !window.modalOpen
        autoRepeat: false
        onActivated: window.stepUiSize(1)
    }
    Shortcut { // Ctrl+-: smaller interface size
        sequences: SearchKeys.zoomOutSequences
        enabled: !window.editor.recordingHotkey && !window.modalOpen
        autoRepeat: false
        onActivated: window.stepUiSize(-1)
    }
    Shortcut { // Ctrl+0: restore default interface size
        sequences: SearchKeys.zoomResetSequences
        enabled: !window.editor.recordingHotkey && !window.modalOpen
        autoRepeat: false
        onActivated: window.stepUiSize(0)
    }

    function showUpdateDialog() { updateDialog.show() } // App::showUpdate
    function showClipboardPage() { showPage(clipboardPage) } // App::showClipboardSettings
    // App::showEntryHotkeySettings: that entry's row in 单条内容的快捷键, recording its keys.
    function showClipboardEntryHotkey(id) {
        const wasLoaded = pageItem(clipboardPage) !== null
        const section = part(clipboardPage, "clipEntryHotkeysSection")
        const row = section ? section.rowOf(id) : null
        if (!row) {
            showPart(clipboardPage, "clipEntryHotkeysSection")
            return
        }
        goTo(row)
        if (!wasLoaded)
            scrollAfterFrame = row
        row.recorder.forceActiveFocus()
    }
    function showWebPage() { showPluginPage("winshun.web") } // App::showWebSettings
    function showShortcutsPage() { showPage(keysPage, keysListTab) } // App::showShortcutSettings: the shortcut panels' last row
    // App::showQuickFilterSettings: the + of the search panel's quick filters, what was typed there as a new one's query
    function showQuickFiltersPage(draft) {
        const section = part(resultsPage, "quickFiltersSection")
        section.draft(draft)
        goTo(section.filtersRow)
    }
    function showFileTypesPage() { goTo(part(resultsPage, "fileTypesSection").typesRow) } // App::showFileTypeSettings: the launcher strip's +
    // Win顺 restarted from here (App::restart, --settings-at): back where it
    // was, "page,tab" (the page by its id; a number too, as older versions
    // wrote it); after 索引方式 changed the rights it runs with, that row.
    function place() { return pages[currentPage].id + "," + currentTab }
    function showPlace(where) {
        if (where === "indexAccess") {
            goTo(part(scopePage, "indexAccessRow"))
            return
        }
        if (where === "indexService") { // the portable copy's install button (App::adminTapCheck)
            const service = part(scopePage, "indexServiceRow")
            const row = service && service.shown ? service : part(scopePage, "indexAccessRow")
            goTo(row)
            // Opened just now, the page is not laid out to its full height
            // yet: scrolled there once more after a frame.
            scrollAfterFrame = row
            return
        }
        const at = String(where).split(",")
        const index = pageIndex(/^\d+$/.test(at[0]) ? Number(at[0]) : at[0])
        if (at.length === 2 && index >= 0 && /^\d+$/.test(at[1]))
            showPage(index, Number(at[1]))
    }
    // open_settings (App::openAiSettings): that page and tab, then the row
    // whose title is given, found with the search box and gone to (its page,
    // the row in view and flashed). No row by that title: the page it is.
    function showSetting(page, tab, title) { // page: its index or id; none (""): the page on show
        if (pageIndex(page) >= 0)
            showPage(page, tab >= 0 ? tab : undefined)
        if (!title)
            return
        loadAllPages() // searchReady now: the row is looked up right after
        programmaticSearch = true
        searchBox.setText(title)
        Qt.callLater(() => {
            try {
                if (settingsSearch.current)
                    goTo(settingsSearch.current)
                else
                    searchBox.clear()
            } finally {
                programmaticSearch = false
            }
        })
    }
    function showFavoritesPage(path) { // App::showFavoriteSettings: that favorite's form open
        showPage(favoritesPage)
        part(favoritesPage, "favoritesSection").edit(path)
    }
    // The 插件 page, scrolled to the plugin's card, which is marked for a
    // moment (a plugin's row opening "winshun:settings": Launcher.pluginSettingsRequested).
    function showPluginPage(pluginId) {
        const wasSearching = searching
        if (!navHistory.restoring && visible) {
            if (wasSearching && beforeSearch)
                navHistory.updateScroll(beforeSearch.y)
            else if (!wasSearching)
                navHistory.updateScroll(currentScrollY())
        }
        navHistory.setRestoring(true)
        let openedCard = false
        try {
            showPage(pluginsPage, 0) // the cards' tab, whichever was left on
            const plugins = part(pluginsPage, "pluginsSection")
            const card = plugins.cardOf(pluginId)
            if (!card) {
                navHistory.setRestoring(false)
                showPluginLibrary(pluginId)
                return
            }
            plugins.expand(pluginId) // its settings are what was asked for
            layoutNow()
            revealRow(card)
            plugins.reveal(pluginId)
            // A window opened just now has not laid out the cards' column yet (layoutNow
            // places only the sections): every card is still at y 0. Once more after a frame.
            revealAfterFrame = pluginId
            openedCard = true
        } finally {
            navHistory.setRestoring(false)
        }
        if (openedCard) {
            recordPlace(flick.contentY)
            if (visible)
                edgeGate.noteSwitched(0)
        }
    }
    // 已安装的插件 / 插件库: a plugin opened in place of the grid shows from the top;
    // back, the grid is where it was left.
    property real pluginGridY: 0
    property real galleryGridY: 0
    function pluginDetailToggled(open, tab) {
        if (navHistory.restoring)
            return
        const targetTab = tab ?? 0
        const previousY = flick.contentY
        flick.stopScrolling()
        edgeGate.cancel()
        if (open) {
            if (targetTab === 1)
                galleryGridY = previousY
            else
                pluginGridY = previousY
            flick.contentY = 0
            if (visible && !searching && currentPage === pluginsPage && currentTab === targetTab) {
                navHistory.updateScroll(previousY)
                recordPlace(0)
                edgeGate.noteSwitched(0)
            }
            return
        }
        const savedY = targetTab === 1 ? galleryGridY : pluginGridY
        if (visible && !searching && currentPage === pluginsPage && currentTab === targetTab) {
            navHistory.updateScroll(previousY)
            recordPlace(savedY)
            edgeGate.noteSwitched(0)
        }
        layoutNow()
        Qt.callLater(() => flick.contentY = Math.max(0, Math.min(savedY, flick.contentHeight - flick.height)))
    }
    function showPluginLibrary(pluginId) {
        pageItem(pluginsPage)?.parts.pluginGallerySection?.closeDetail()
        window.editor.plugins.gallery.updatesOnly = false
        window.editor.plugins.gallery.filter = pluginId
        showPage(pluginsPage, 1)
    }
    function showPluginUpdates() {
        pageItem(pluginsPage)?.parts.pluginGallerySection?.closeDetail()
        window.editor.plugins.gallery.filter = ""
        window.editor.plugins.gallery.updatesOnly = true
        showPage(pluginsPage, 1)
    }
    property string revealAfterFrame
    property Item scrollAfterFrame: null // showPlace
    Connections {
        target: window
        enabled: window.scrollAfterFrame !== null
        function onFrameSwapped() {
            const row = window.scrollAfterFrame
            window.scrollAfterFrame = null
            window.scrollToRow(row)
            navHistory.updateScroll(flick.contentY)
        }
    }
    Connections {
        target: window
        enabled: window.scrollToBottomAfterFrame
        function onFrameSwapped() {
            window.scrollToBottomAfterFrame = false
            if (!window.searching && window.currentPage !== window.gleaningPage) {
                flick.stopScrolling()
                flick.contentY = Math.max(0, flick.contentHeight - flick.height)
                navHistory.updateScroll(flick.contentY)
            }
        }
    }
    // The capture's card, unfolded at its hotkeys (快捷键 › 打开 Win顺's 截图的快捷键 row); its library entry when it is not installed.
    function showCaptureCard() {
        showPluginPage("winshun.capture")
    }
    Connections {
        target: window
        enabled: window.revealAfterFrame.length > 0
        function onFrameSwapped() {
            const card = window.part(window.pluginsPage, "pluginsSection").cardOf(window.revealAfterFrame)
            if (card) {
                window.revealRow(card)
                if (card.detailsSettled)
                    window.revealAfterFrame = ""
            } else {
                window.revealAfterFrame = ""
            }
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
            // The page on show: made already.
            if (window.currentPage === window.favoritesPage) {
                window.part(window.favoritesPage, "favoritesSection").favorites.addPaths(urls)
            } else if (window.currentPage === window.pluginsPage) {
                const pluginsSection = window.part(window.pluginsPage, "pluginsSection")
                for (const url of urls)
                    pluginsSection.plugins.install(String(url))
            } else if (window.currentPage === window.scopePage && window.currentTab === 0) {
                const foldersRow = window.part(window.scopePage, "foldersRow")
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
    function sinceText(date: date): string {
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
        return index === gleaningPage ? gleaningNav : navItems.itemAt(index)?.item ?? null
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

    onVisibleChanged: {
        if (visible) {
            // Emitted before the native window shows (QWindow::setVisible): the
            // page on show is there for the first frame, laid out at the final size.
            if (currentPage !== gleaningPage)
                loadPage(currentPage)
            if (navHistory.count === 0)
                navHistory.reset(pages[currentPage].id, currentTab, activePluginDetailId(), "", currentScrollY())
            edgeGate.noteSwitched(0)
            now = new Date()
            if (warm)
                AuthorClock.play()
        } else {
            resetForReopen()
        }
    }

    GleaningBackdrop { // under the title bar too: the sky reaches the top of the window
        anchors.fill: parent
        visible: window.warm
        // Painted on the CPU: only while someone is looking at it.
        running: window.visible && window.active && window.visibility !== Window.Minimized && SystemTheme.animations
        leadingInset: nav.width
    }

    Binding { // suspends the global hotkey and hands the recorder the keyboard (see ShortcutCapture)
        target: window.editor
        property: "recordingHotkey"
        value: (keysSlot.item?.recordingHotkey ?? false) || (resultsSlot.item?.recordingHotkey ?? false)
               || (integrationSlot.item?.recordingHotkey ?? false) || (clipboardSlot.item?.recordingHotkey ?? false)
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
    Timer { // the 插件库's entries made from the kept list while 已安装 is looked at, a few at a time between
        // frames (PluginGallerySection): the first click on its tab held the window some 150 ms making them all,
        // its line not moving meanwhile. After the page's own motion; once a run; nothing fetched (that waits for the tab).
        interval: 300
        running: window.visible && !window.searching && window.currentPage === window.pluginsPage && window.currentTab === 0
        onTriggered: window.editor.plugins.gallery.readList()
    }

    TitleBar {
        id: titleBar
        z: 1
        width: parent.width
        frame: window.frame
        title: window.title + " - " + Qt.application.displayName
        iconX: navList.x + 16 // over the categories' icons, the title over their names
        foreground: window.warm ? WarmPalette.ink : Theme.text
        leadingToolsX: nav.width // icon ink sits at nav.width + 8, lined up with the page title and cards below
        leadingTools: [
            TitleNavButton {
                id: backButton
                objectName: "settingsBackButton"
                anchors.verticalCenter: parent.verticalCenter
                iconName: "arrow-left"
                active: window.canNavigateBack
                warm: window.warm
                ink: titleBar.ink
                strongInk: window.warm ? WarmPalette.ink : Theme.text
                dpr: window.dpr
                accessibleName: window.backTargetLabel.length > 0
                                ? qsTr("返回「%1」").arg(window.backTargetLabel)
                                : qsTr("后退")
                onClicked: window.navigateBack()
                onMenuRequested: window.openHistoryMenu(backButton, window.backMenuEntries())
            },
            TitleNavButton {
                id: forwardButton
                objectName: "settingsForwardButton"
                anchors.verticalCenter: parent.verticalCenter
                iconName: "arrow-right"
                active: window.canNavigateForward
                warm: window.warm
                ink: titleBar.ink
                strongInk: window.warm ? WarmPalette.ink : Theme.text
                dpr: window.dpr
                accessibleName: window.forwardTargetLabel.length > 0
                                ? qsTr("前进到「%1」").arg(window.forwardTargetLabel)
                                : qsTr("前进")
                onClicked: window.navigateForward()
                onMenuRequested: window.openHistoryMenu(forwardButton, navHistory.forwardEntries)
            }
        ]
        tools: Row { // 捐赠, and the × that asks to move it to the 拾穗计划 page (GitHub #402)
            objectName: "titleDonateButton"
            visible: Gleaning.donateUrl.length > 0 && !window.editor.donateOnGleaningPage
            spacing: 2

            DonateButton {
                anchors.verticalCenter: parent.verticalCenter
                dpr: window.dpr
                onClicked: window.editor.openUrl(Gleaning.donateUrl)
            }
            DonateSideKey {
                objectName: "titleDonateTuck"
                anchors.verticalCenter: parent.verticalCenter
                iconName: "x"
                tip: qsTr("挪到%1页").arg(Gleaning.title)
                dpr: window.dpr
                onClicked: donateDialog.ask()
            }
        }
    }

    HoverTip { // shared gliding tooltip for the title bar's Back / Forward keys
        target: historyMenu.open ? null
                : backButton.hovered ? backButton
                : forwardButton.hovered ? forwardButton
                : null
        text: backButton.hovered ? backButton.accessibleName : forwardButton.accessibleName
        shortcut: backButton.hovered ? "Alt+←" : "Alt+→"
        dpr: window.dpr
    }

    DropMenu { // right-click or press-and-hold on Back / Forward: jump multiple steps
        id: historyMenu
        objectName: "settingsHistoryMenu"
        property var historyIndices: []
        anchor: backButton
        alignLeft: true
        menuWidth: 230
        onActivated: (index) => {
            if (index >= 0 && index < historyIndices.length)
                window.navigateToHistory(historyIndices[index])
        }
    }

    Item { // category list
        id: nav
        y: titleBar.height
        width: [236, 248, 268][Theme.step]
        height: parent.height - y
        // The window's parts each one element for UI Automation, so that a
        // client (screen reader, input method, assistant) finds a handful
        // under the window, not every text of every page (see SettingsSection).
        Accessible.role: Accessible.Pane

        MouseArea { // clicking empty space finishes editing a field
            anchors.fill: parent
            onClicked: window.clearFocus()
        }

        component NavItem: Rectangle {
            id: navItem

            required property int index
            required property var modelData
            readonly property bool selected: index === window.navPage && !window.searching
            // While searching: its options on show.
            readonly property int found: window.searching ? settingsSearch.counts[index] ?? 0 : -1
            readonly property bool gleaning: modelData.gleaning === true
            // 插件: how many plugins have a newer version waiting (not while searching, which shows the counts)
            readonly property int updates: index === window.pluginsPage && !window.searching ? window.editor.plugins.gallery.updateCount : 0
            readonly property color foreground: window.warm ? (selected ? Theme.accentText : WarmPalette.ink)
                                              : navArea.pressed ? Theme.subtext : Theme.text

            function select(to) {
                window.navigate(to)
                window.navItemAt(to).forceActiveFocus()
            }

            width: navList.width
            height: navList.itemHeight
            radius: Theme.controlRadius
            opacity: found === 0 ? 0.45 : 1
            // On the warm scene the item itself is drawn; else the style's row highlight (below).
            color: window.warm && navArea.containsMouse && !selected ? WarmPalette.chip : "transparent"
            activeFocusOnTab: true

            Keys.onSpacePressed: window.navigate(navItem.index)
            Keys.onReturnPressed: window.navigate(navItem.index)
            Keys.onEnterPressed: window.navigate(navItem.index)
            Keys.onUpPressed: {
                if (navItem.index === 0)
                    searchBox.focusAll()
                else
                    select(navItem.index - 1)
            }
            Keys.onDownPressed: select(Math.min(window.pages.length - 1, navItem.index + 1))
            onActiveFocusChanged: if (activeFocus) window.revealNavItem(navItem)

            Hook { // the style's highlight (Theme.rowHighlight): the chosen category, or the one under the mouse
                anchors.fill: parent
                visible: !window.warm
                part: window.warm ? null : Theme.rowHighlight
                selected: navItem.selected
                hovered: navArea.containsMouse
                role: "panel"
                dpr: window.dpr
            }

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
                width: parent.width - x - 12 - (count.visible ? count.width + 8 : badge.visible ? badge.width + 8 : 0)
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
                // Chosen on the press, as Windows' lists and Explorer's
                // navigation pane choose: a click's ~100 ms sooner than on release.
                onPressed: {
                    window.clearFocus()
                    window.navigate(navItem.index)
                }
                onContainsMouseChanged: window.hoverNav(navItem.index, containsMouse)
            }

            HoverTip { // 插件: what the number is
                target: navItem.updates > 0 && navArea.containsMouse ? navItem : null
                text: qsTr("%n 个插件有新版本，点击查看", "", navItem.updates)
            }

            Rectangle { // 插件: the updates waiting; a click leads to them
                id: badge
                visible: navItem.updates > 0
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(height, badgeText.implicitWidth + 10)
                height: [18, 18, 22][Theme.step]
                radius: Theme.pillRadius(height)
                color: Theme.accentFill
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("%n 个插件有新版本", "", navItem.updates)

                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: navItem.updates
                    color: Theme.accentText
                    font.pixelSize: Theme.fontCaption
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        window.clearFocus()
                        window.showPluginUpdates()
                    }
                }
            }
        }

        SettingsSearchBox {
            id: searchBox
            objectName: "settingsSearchBox"
            x: 12
            y: 8
            width: parent.width - 24
            warm: window.warm
            onMoved: (delta) => window.moveCurrent(delta)
            onActivated: {
                if (!window.searchReady && query.length > 0)
                    window.activatePending = true // gone to once searched
                else
                    window.goToCurrent()
            }
            onEscaped: window.clearFocus()
            // Typed before searchReady: nothing made at once, the page stays as
            // it is (no 没有找到 meanwhile) until the search can look (fillFast).
            onQueryChanged: if (query.length === 0) window.activatePending = false
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

                    readonly property int itemHeight: [36, 40, 44][Theme.step]

                    x: 12
                    y: 8
                    width: parent.width - 24
                    spacing: 4

                    Repeater {
                        id: navItems
                        model: window.pages.slice(0, window.gleaningPage)
                        // The first of a group (pages' `group`) a little apart from the one above.
                        delegate: Item {
                            id: navSlot

                            required property int index
                            required property var modelData
                            readonly property alias item: slotItem

                            width: navList.width
                            // All twelve fit the window's first height (720) unscrolled, gaps included.
                            height: slotItem.height + (modelData.group === true ? 6 : 0)

                            NavItem {
                                id: slotItem
                                anchors.bottom: parent.bottom
                                index: navSlot.index
                                modelData: navSlot.modelData
                            }
                        }
                    }

                    Item { // sets 拾穗计划 apart from the settings, a little more than a group
                        width: 1
                        height: 8
                    }

                    NavItem {
                        id: gleaningNav
                        index: window.gleaningPage
                        modelData: window.pages[window.gleaningPage]
                    }
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
            color: window.warm && versionArea.containsMouse ? WarmPalette.chip : "transparent"

            Hook { // the style's hover (Theme.rowHighlight)
                anchors.fill: parent
                part: window.warm || !versionArea.containsMouse ? null : Theme.rowHighlight
                hovered: true
                role: "panel"
                dpr: window.dpr
            }

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
                    window.showPage(window.generalPage)
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
        id: gleaningView
        visible: window.warm
        Accessible.role: Accessible.Pane // hidden: one element skipped whole (see nav)
        Accessible.name: Gleaning.title
        anchors.left: nav.right
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        editor: window.editor
        updater: window.updater
        edgePullY: window.warm ? window.edgePullY : 0
        onContentYChanged: {
            if (gleaningWheelScroll.writing || !visible)
                return
            const maxY = Math.max(0, contentHeight - height)
            if (maxY > 0.5 && (contentY <= 0.5 || contentY >= maxY - 0.5))
                edgeGate.noteBoundaryReached()
            else
                edgeGate.cancel()
        }
        ListWheelScroll {
            id: gleaningWheelScroll
            view: gleaningView
            animate: SystemTheme.animations
            consumeWheel: (dir) => edgeGate.shouldConsumeWheel(dir)
            onRunningChanged: if (running) edgeGate.cancel()
            onBoundaryReached: edgeGate.noteBoundaryReached()
            onOverscrolled: (dir, delta) => edgeGate.overscroll(dir, delta)
        }
    }

    Item {
        id: pageHeader
        visible: !window.warm
        Accessible.role: Accessible.Pane // see nav
        anchors.left: nav.right
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        height: Theme.snap(pageHeaderColumn.implicitHeight + 16, window.dpr)

        Column {
            id: pageHeaderColumn
            x: 8
            y: 8
            width: parent.width - 36
            spacing: 6

            Row {
                spacing: 12

                Text {
                    id: pageTitle
                    text: window.searching ? qsTr("搜索结果") : window.pages[window.currentPage].title
                    color: Theme.text
                    font.pixelSize: Theme.fontDisplay
                    font.weight: Theme.fontStrong
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

        Rectangle {
            x: 8
            width: parent.width - 36
            anchors.bottom: parent.bottom
            height: Theme.edge(window.dpr)
            color: Theme.divider
            opacity: flick.contentY > 2 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
    }

    Flickable {
        id: flick
        visible: !window.warm
        Accessible.role: Accessible.Pane // see nav; the pages under it (PageSlot)
        Accessible.name: pageTitle.text
        anchors.left: nav.right
        anchors.right: parent.right
        anchors.top: pageHeader.bottom
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: page.implicitHeight + 48
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        function stopScrolling() {
            wheelScroll.cancel()
            cancelFlick()
        }
        onContentYChanged: {
            if (wheelScroll.writing || !visible)
                return
            const maxY = Math.max(0, contentHeight - height)
            if (maxY > 0.5 && (contentY <= 0.5 || contentY >= maxY - 0.5))
                edgeGate.noteBoundaryReached()
            else
                edgeGate.cancel()
        }
        ListWheelScroll {
            id: wheelScroll
            view: flick
            animate: SystemTheme.animations
            consumeWheel: (dir) => edgeGate.shouldConsumeWheel(dir)
            onRunningChanged: if (running) edgeGate.cancel()
            onBoundaryReached: edgeGate.noteBoundaryReached()
            onOverscrolled: (dir, delta) => edgeGate.overscroll(dir, delta)
        }

        MouseArea { // clicking empty space finishes editing a field
            width: flick.width
            height: Math.max(flick.height, flick.contentHeight)
            onClicked: window.clearFocus()
        }

        Column { // the current category's options; the other sections are hidden
            id: page

            // Each category a PageSlot below: its SettingsPage, the column its
            // sections read (search, currentPage, … from settingsContext).
            x: 8
            y: 12
            width: flick.width - 36
            spacing: 20
            // Drawn on whole device pixels wherever it has scrolled to (see
            // Main.qml), plus a fiftieth of one: much of the text inside sits
            // on half pixels (odd logical pixels at 150 %), where the text
            // shader's rounding would go either way with float error from one
            // scroll position to the next. A hair past the half, it always
            // goes the same way.
            transform: [
                ScrollPixelAlignment {
                    view: flick
                    viewY: flick.y
                    itemY: page.y
                    pixelBias: 0.02
                },
                Translate {
                    y: !window.warm ? window.edgePullY : 0
                    Behavior on y {
                        enabled: SystemTheme.animations
                        NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                    }
                }
            ]

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
                Item {
                    visible: noResults.windowsSettings.length > 0
                    width: parent.width
                    height: windowsList.implicitHeight

                    Hook { // the style's card (Theme.cardPart)
                        anchors.fill: parent
                        part: Theme.cardPart
                        dpr: window.dpr
                    }

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

                                Hook { // under the mouse: the style's highlight (Theme.rowHighlight)
                                    anchors.fill: parent
                                    anchors.margins: Theme.edge(window.dpr) // inside the card's border
                                    part: windowsArea.containsMouse ? Theme.rowHighlight : null
                                    hovered: true
                                    role: "panel"
                                    dpr: window.dpr
                                }
                                Hook { // between the rows (Theme.separator)
                                    x: Theme.edge(window.dpr) // inside the card's border
                                    width: parent.width - 2 * x
                                    height: Theme.edge(window.dpr)
                                    part: windowsRow.index > 0 ? Theme.separator : null
                                    extra: ({ card: true })
                                    dpr: window.dpr
                                }
                                Item {
                                    id: windowsIcon
                                    x: 18
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 24
                                    height: 24

                                    Image { // whole device pixels, unsmoothed (see ResultRow)
                                        readonly property real dpr: window.dpr
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

            PageSlot {
                id: generalSlot
                index: window.generalPage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({ proxyRow: proxyRow, rendererRow: rendererRow, indexFolderRow: indexFolderRow })
                    onResetForReopen: resetButton.confirming = false
            // <page general>
            SettingsSection {
                page: window.generalPage
                width: parent.width
                title: qsTr("更新")
                glyph: "\uE895"
                note: qsTr("检查 Win顺 更新访问 lcstp.com，只发送 Windows 版本和安装方式；检查插件更新从 GitHub 或镜像下载插件库列表。匿名统计每天最多一次，只发送电脑标识的单向哈希（不能还原）、版本号和 Windows 版本，用来统计用户数。不发送文件、剪贴板或任何个人信息。")

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
                        dpr: window.dpr
                        visible: !window.updater.available
                        enabled: !window.updater.checking
                        text: qsTr("检查更新")
                        glyph: "\uE895" // Sync
                        onClicked: window.updater.check(true)
                    }
                    FlatButton {
                        dpr: window.dpr
                        visible: window.updater.available
                        highlighted: true
                        text: qsTr("查看 %1").arg(window.updater.availableVersion)
                        onClicked: window.showUpdateDialog()
                    }
                }

                SettingRow {
                    title: qsTr("自动更新")
                    keywords: qsTr("升级;新版本;插件;update;plugin").split(";")
                    description: qsTr("启动后和每隔 10 分钟检查 Win顺 新版本，在托盘提示；插件每天检查一次，不要新权限的更新自动安装")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.autoUpdate
                        onToggled: (on) => window.editor.autoUpdate = on
                    }
                }

                SettingRow {
                    title: qsTr("发送匿名统计")
                    keywords: qsTr("统计;用户数;隐私;联网;usage;statistics;privacy;telemetry").split(";")
                    description: qsTr("每天最多一次告诉 lcstp.com 这台电脑在用 Win顺，用来统计用户数")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.usageReport
                        onToggled: (on) => window.editor.usageReport = on
                    }
                }
            }

            SettingsSection {
                page: window.generalPage
                width: parent.width
                title: qsTr("开机启动")
                glyph: "\uE7E8"

                SettingRow {
                    title: qsTr("开机时自动启动")
                    keywords: qsTr("自启;开机启动;启动;autostart;startup").split(";")
                    description: qsTr("登录 Windows 后在后台运行")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.autostart
                        onToggled: (on) => window.editor.autostart = on
                    }
                }
            }

            SettingsSection {
                page: window.generalPage
                width: parent.width
                title: qsTr("备份")
                glyph: "\uE8F1"
                note: qsTr("备份设置、收藏夹和添加的插件，不含索引和历史记录。选同步盘里的文件夹（OneDrive、坚果云，或用 alist、RaiDrive 挂载的 WebDAV 盘），备份就跟着同步到云上。")

                SettingRow {
                    objectName: "backupExportRow"
                    title: qsTr("导出设置")
                    keywords: qsTr("备份;导出;迁移;换电脑;ZIP;backup;export;transfer").split(";")
                    description: qsTr("导出一份 ZIP，带到新电脑后从备份恢复；不用开启自动备份。文件夹路径按原样保留，插件密码需重新填写。")
                    bodyShown: window.backup.busy || window.backup.problem.length > 0 || window.backup.message.length > 0

                    FlatButton {
                        dpr: window.dpr
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
                        dpr: window.dpr
                        visible: backupRow.on
                        text: qsTr("打开")
                        glyph: "" // FolderOpen
                        onClicked: window.backup.openFolder()
                    }
                    FlatButton {
                        dpr: window.dpr
                        visible: backupRow.on
                        text: qsTr("更改")
                        onClicked: window.editor.chooseBackupFolder()
                    }
                    ToggleSwitch {
                        dpr: window.dpr
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
                        dpr: window.dpr
                        visible: window.editor.backupFolder.length > 0
                        enabled: !window.backup.busy
                        text: qsTr("立即备份")
                        onClicked: window.backup.backUpNow()
                    }
                    FlatButton {
                        dpr: window.dpr
                        enabled: !window.backup.busy
                        text: qsTr("从备份恢复…")
                        onClicked: window.backup.restore(window)
                    }
                }
            }

            SettingsSection {
                page: window.generalPage
                width: parent.width
                title: qsTr("网络、存储与诊断")
                glyph: "\uE713"

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
                            dpr: window.dpr
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
                    id: rendererRow
                    title: qsTr("界面绘制方式")
                    keywords: qsTr("渲染;GPU;显卡;内存;模糊;清晰;renderer").split(";")
                    description: window.editor.restartRequired
                                 ? qsTr("重启 Win顺后生效")
                                 : qsTr("“省内存”少占约 50 MB，但文字偏模糊")

                    FlatButton { // sized like one of the tabs next to it
                        dpr: window.dpr
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
                        dpr: window.dpr
                        text: qsTr("打开")
                        glyph: "" // FolderOpen
                        enabled: !indexFolderRow.moving
                        onClicked: window.editor.openIndexFolder()
                    }
                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("更改")
                        glyph: "" // MoveToFolder
                        enabled: !indexFolderRow.moving
                        onClicked: window.editor.chooseIndexFolder()
                    }
                    FlatButton {
                        dpr: window.dpr
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
                        dpr: window.dpr
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
                        dpr: window.dpr
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
                        dpr: window.dpr
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
            // </page general>
                } }
            }

            PageSlot {
                id: keysSlot
                index: window.keysPage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({ hotkeyRow: hotkeyRow, panelHotkeyRow: panelHotkeyRow, settingsHotkeyRow: settingsHotkeyRow })
                    recordingHotkey: hotkeyRecorder.recording || panelHotkeyRecorder.recording || settingsHotkeyRecorder.recording
                    onResetForReopen: {
                        hotkeyRecorder.stop()
                        panelHotkeyRecorder.stop()
                        settingsHotkeyRecorder.stop()
                        doubleCtrlAppInput.clear()
                    }
            // <page keys>
            SettingsSection {
                page: window.keysPage
                tab: 0
                width: parent.width
                title: qsTr("双击修饰键呼出")
                glyph: "\uE765"

                SettingRow {
                    title: qsTr("双击 Ctrl、Alt 或 Shift 打开")
                    keywords: qsTr("双击;连按;呼出;唤出;double ctrl;double alt;double shift").split(";")
                    unlocks: [doubleTapActionRow, doubleCtrlKeyRow, gamesRow, fullScreenRow, doubleCtrlAppsRow]
                    description: qsTr("连按两下 %1，执行下方选择的动作").arg(window.tapKey)
                    tip: window.tapKey === "Shift"
                         ? qsTr("输入法用 Shift 切换中英文时，双击会切换两次并回到原状态。管理员或部分输入法窗口中需要 Win顺 服务支持")
                         : qsTr("管理员或部分输入法窗口中需要 Win顺 服务支持")

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
                    id: doubleTapActionRow
                    sub: true
                    shown: window.editor.doubleCtrl
                    title: qsTr("双击后做什么")
                    description: qsTr("每个双击键只执行一个动作。列表只显示支持此功能的已启用插件；原组合键仍可使用")
                    keywords: qsTr("剪贴板;截图;插件;双击;动作;clipboard;plugin;double tap").split(";")

                    ChoiceBox {
                        id: doubleTapActionPicker
                        objectName: "doubleTapActionPicker"
                        implicitWidth: 280
                        implicitHeight: 36
                        model: window.editor.doubleTapActions
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: window.editor.doubleTapActions.findIndex(action => action.value === window.editor.doubleTapAction)
                        onActivated: window.editor.doubleTapAction = currentValue
                        Accessible.name: qsTr("双击后做什么")
                        dpr: window.dpr
                        leftPadding: 12
                        contentItem: Text {
                            text: doubleTapActionPicker.displayText
                            font: doubleTapActionPicker.font
                            color: doubleTapActionPicker.palette.buttonText
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        Connections {
                            target: doubleTapActionPicker.popup
                            function onOpened() { window.frame.addPopup(doubleTapActionPicker.popup.background) }
                        }
                    }
                }

                SettingRow {
                    id: doubleCtrlKeyRow
                    sub: true
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
                            dpr: window.dpr
                            id: clashButton
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.other === "right" ? qsTr("Win顺 改用右 Ctrl") : qsTr("Win顺 改用左 Ctrl")
                            onClicked: window.editor.doubleCtrlKey = parent.other
                        }
                    }
                }

                SettingRow {
                    id: gamesRow
                    sub: true
                    shown: window.editor.doubleCtrl
                    title: qsTr("玩游戏时不响应双击 %1").arg(window.tapKey)
                    keywords: qsTr("游戏;蹲下;误触;game").split(";")
                    description: window.tapKey !== "Ctrl" ? qsTr("游戏全屏或藏起鼠标时不打开，免得误触")
                                                         : qsTr("游戏全屏或藏起鼠标时不打开，免得蹲下时误触")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.doubleCtrlPauseInGames
                        onToggled: (on) => window.editor.doubleCtrlPauseInGames = on
                    }
                }

                SettingRow {
                    id: fullScreenRow
                    sub: true
                    shown: window.editor.doubleCtrl
                    title: qsTr("任何程序全屏时都不响应双击 %1").arg(window.tapKey)
                    keywords: qsTr("全屏;视频;幻灯片;误触;fullscreen").split(";")
                    description: qsTr("看视频、放幻灯片时也不打开")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.doubleCtrlPauseInFullScreen
                        onToggled: (on) => window.editor.doubleCtrlPauseInFullScreen = on
                    }
                }

                SettingRow {
                    id: doubleCtrlAppsRow
                    sub: true
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
                                dpr: window.dpr
                                id: doubleCtrlAppInput
                                width: 300
                                placeholder: qsTr("输入程序文件名，例如 TheFinals.exe")
                                onAccepted: if (window.editor.addDoubleCtrlExcludedApp(text)) clear()
                            }
                            FlatButton {
                                dpr: window.dpr
                                text: qsTr("添加")
                                enabled: doubleCtrlAppInput.text.trim().length > 0
                                onClicked: if (window.editor.addDoubleCtrlExcludedApp(doubleCtrlAppInput.text))
                                               doubleCtrlAppInput.clear()
                            }
                        }
                    ]
                }
            }

            SettingsSection {
                page: window.keysPage
                tab: 0
                width: parent.width
                title: qsTr("全局组合键与连按行为")
                glyph: "\uE92E"

                SettingRow {
                    id: hotkeyRow
                    title: qsTr("快捷键")
                    keywords: qsTr("热键;组合键;hotkey;shortcut").split(";")
                    values: [window.editor.hotkey]
                    description: qsTr("另设一个组合键打开搜索框，例如 Alt+Space")

                    FlatButton {
                        dpr: window.dpr
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
                    id: panelHotkeyRow
                    title: qsTr("直接打开搜索面板")
                    description: qsTr("一个组合键打开完整搜索面板并聚焦搜索框，例如 Ctrl+Alt+F")
                    keywords: qsTr("完整窗口;面板;全局;热键;快捷键;full search;panel;hotkey").split(";")
                    values: [window.editor.panelHotkey]
                    FlatButton {
                        dpr: window.dpr
                        visible: window.editor.panelHotkey.length > 0 && !panelHotkeyRecorder.recording
                        flat: true
                        text: qsTr("清除")
                        onClicked: window.editor.panelHotkey = ""
                    }
                    HotkeyRecorder {
                        id: panelHotkeyRecorder
                        objectName: "panelHotkeyRecorder"
                        hotkey: window.editor.panelHotkey
                        onRecorded: hotkey => window.editor.panelHotkey = hotkey
                    }
                    body: Text {
                        visible: window.editor.panelHotkeyError.length > 0
                        width: parent.width
                        text: window.editor.panelHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    id: settingsHotkeyRow
                    title: qsTr("打开设置")
                    description: qsTr("一个组合键打开 Win顺 设置，再次按下可关闭，例如 Alt+X")
                    keywords: qsTr("设置;偏好;全局;热键;快捷键;settings;preferences;hotkey").split(";")
                    values: [window.editor.settingsHotkey]
                    FlatButton {
                        dpr: window.dpr
                        visible: window.editor.settingsHotkey.length > 0 && !settingsHotkeyRecorder.recording
                        flat: true
                        text: qsTr("清除")
                        onClicked: window.editor.settingsHotkey = ""
                    }
                    HotkeyRecorder {
                        id: settingsHotkeyRecorder
                        objectName: "settingsHotkeyRecorder"
                        hotkey: window.editor.settingsHotkey
                        onRecorded: hotkey => window.editor.settingsHotkey = hotkey
                    }
                    body: Text {
                        visible: window.editor.settingsHotkeyError.length > 0
                        width: parent.width
                        text: window.editor.settingsHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                // The capture's own hotkey is recorded on its card (插件 › 截图);
                // here so that whoever looks among the hotkeys finds it.
                SettingRow {
                    objectName: "captureHotkeyRow"
                    title: qsTr("截图的快捷键")
                    keywords: qsTr("截图;截屏;快捷键;热键;F1;PrtScn;PixPin;Snipaste;screenshot").split(";")
                    values: window.editor.screenshotHotkeys
                    description: !window.editor.screenshotInstalled ? qsTr("截图是可选插件：在“插件”的插件库里安装，再录一个快捷键，例如 F1")
                               : !window.editor.screenshotHistory ? qsTr("截图插件还没打开：在“插件”的截图卡片上打开，再录一个快捷键，例如 F1")
                               : window.editor.screenshotHotkeys.length > 0 ? qsTr("现在是 %1，在“插件”的截图卡片上改；贴图等其他快捷键也在那里").arg(window.editor.screenshotHotkeys.join(" / "))
                               : qsTr("还没设，在“插件”的截图卡片上录一个，例如 F1；贴图等其他快捷键也在那里")

                    FlatButton {
                        dpr: window.dpr
                        objectName: "captureHotkeyButton"
                        text: window.editor.screenshotInstalled ? qsTr("去设置") : qsTr("去插件库")
                        onClicked: window.showCaptureCard()
                    }
                }

                SettingRow {
                    title: qsTr("再次按下快捷键时")
                    keywords: qsTr("再次;两次;再按;搜索面板;展开;Ctrl+E;面板;关闭;panel;again;expand").split(";")
                    description: qsTr("搜索框已打开时再按一次呼出键的动作")
                    tip: qsTr("包括自己设的组合键，或双击 Ctrl、Alt、Shift：可直接关闭，或展开为搜索面板并保留已输入的字；面板里再按一次才关闭")

                    ScopeTabs {
                        labels: [qsTr("关闭 Win顺"), qsTr("展开为搜索面板")]
                        current: window.editor.shortcutAgain === "panel" ? 1 : 0
                        onActivated: (index) => window.editor.shortcutAgain = index === 1 ? "panel" : "close"
                    }
                }
            }

            SettingsSection {
                page: window.keysPage
                tab: 0
                width: parent.width
                title: qsTr("其他功能的快捷键")
                glyph: "ph:arrow-square-out"

                SettingsLinkRow {
                    objectName: "keysClipboardLink"
                    iconName: "clipboard-text"
                    title: qsTr("剪贴板快捷键")
                    description: {
                        const openKeys = [window.editor.clipboardWinV ? "Win+V" : "", window.editor.clipboardHotkey]
                            .filter(keys => keys.length > 0).join(" / ")
                        const plain = window.editor.pastePlainHotkey
                        if (openKeys.length > 0 && plain.length > 0)
                            return qsTr("打开剪贴板：%1 · 粘贴为纯文本：%2").arg(openKeys).arg(plain)
                        if (openKeys.length > 0)
                            return qsTr("打开剪贴板：%1 · 可另设粘贴为纯文本和数字键粘贴").arg(openKeys)
                        if (plain.length > 0)
                            return qsTr("粘贴为纯文本：%1 · 尚未设置剪贴板呼出键").arg(plain)
                        return qsTr("可开启 Win+V、另设组合键，或配置粘贴为纯文本与数字键")
                    }
                    destination: qsTr("剪贴板 › 快捷键")
                    targetPage: "clipboard"
                    targetPart: "clipboardWinVRow"
                }

                SettingsLinkRow {
                    objectName: "keysFolderMenuLink"
                    iconName: "folder-open"
                    title: qsTr("文件夹菜单快捷键")
                    description: window.editor.folderMenuHotkey.length > 0
                                 ? qsTr("当前为 %1 · 在文件管理器或对话框中弹出常用文件夹菜单").arg(window.editor.folderMenuHotkey)
                                 : qsTr("未设置；也可在资源管理器或桌面空白处双击或按中键弹出")
                    destination: qsTr("系统整合 › 文件夹菜单")
                    targetPage: "integration"
                    targetPart: "folderMenuHotkeyRow"
                }

                SettingsLinkRow {
                    objectName: "keysWinSLink"
                    iconName: "magnifying-glass"
                    title: qsTr("用 Win+S 打开 Win顺")
                    description: window.editor.taskbarWinS
                                 ? qsTr("已开启 · 用 Win+S 代替 Windows 自带搜索")
                                 : qsTr("未开启 · 也可在开始菜单里直接打字搜索")
                    destination: qsTr("系统整合 › 任务栏")
                    targetPage: "integration"
                    targetPart: "taskbarWinSRow"
                }
            }

            Repeater { // 按键一览: the shortcuts, a section for each group of them
                // Not made with the page: some 70 rows, a section at a time after the
                // pages (fillStep), or all at once when that tab is shown (shortcutsWanted).
                model: shortcutModel

                delegate: SettingsSection {
                    id: shortcutSection

                    required property int index
                    readonly property var group: window.shortcutSections[index] ?? null

                    page: window.keysPage
                    tab: 1
                    width: parent.width
                    title: group?.title ?? ""
                    note: index === 0 ? qsTr("所有快捷键都在这里；带“去设置”的可以自己改。") : ""

                    Repeater {
                        model: shortcutSection.group?.rows ?? []

                        delegate: SettingRow {
                            id: shortcutRow

                            required property var modelData

                            title: modelData?.text ?? ""
                            keywords: [modelData?.keys ?? ""]
                            values: [modelData?.keys ?? ""]

                            Item { // the keys, as a key cap
                                anchors.verticalCenter: parent.verticalCenter
                                visible: (shortcutRow.modelData?.keys.length ?? 0) > 0
                                implicitWidth: capText.implicitWidth + 14
                                implicitHeight: Theme.keycapHeight + 4

                                Hook { // the style's key cap (Theme.keyHint): it only tells
                                    id: capPart
                                    anchors.fill: parent
                                    part: Theme.keyHint
                                    dpr: window.dpr
                                }
                                Text {
                                    id: capText
                                    anchors.centerIn: parent
                                    anchors.verticalCenterOffset: capPart.out("contentOffsetY", 0)
                                    text: shortcutRow.modelData?.keys ?? ""
                                    color: capPart.out("foreground", Theme.subtext)
                                    font.pixelSize: Theme.fontCaption
                                }
                            }
                            Text { // nothing to press: off, or not set yet
                                anchors.verticalCenter: parent.verticalCenter
                                visible: shortcutRow.modelData?.keys.length === 0
                                text: qsTr("未设置")
                                color: Theme.faint
                                font.pixelSize: Theme.fontCaption
                            }
                            FlatButton {
                                dpr: window.dpr
                                anchors.verticalCenter: parent.verticalCenter
                                visible: (shortcutRow.modelData?.setting ?? null) !== null
                                text: qsTr("去设置")
                                onClicked: {
                                    const target = shortcutRow.modelData.setting
                                    if (target.capture)
                                        window.showCaptureCard()
                                    else if (target.part)
                                        window.showPart(target.page, target.part)
                                    else
                                        window.showSetting(target.page, target.tab)
                                }
                            }
                        }
                    }
                }
            }
            // </page keys>
                } }
            }

            PageSlot {
                id: appearanceSlot
                index: window.appearancePage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({ uiSizeRow: uiSizeRow })
            // <page appearance>
            SettingsSection {
                page: window.appearancePage
                width: parent.width
                title: qsTr("主题与窗口质感")
                glyph: "\uE790"

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
                    title: qsTr("界面风格")
                    keywords: qsTr("风格;外观;样式;皮肤;精修原生;品牌色彩;轻拟物;拟物;质感;液态玻璃;玻璃;Fluent;Liquid Glass;style;skin;look").split(";")
                    description: qsTr("所有窗口的样子；浅色、深色仍按上面的主题")

                    body: Flow {
                        id: styleCards

                        readonly property var styles: ["fluent", "brand", "tactile", "liquid"]
                        readonly property var labels: [qsTr("精修原生"), qsTr("品牌色彩"), qsTr("轻拟物质感"), qsTr("液态玻璃")]
                        readonly property int columns: width >= 586 ? 4 : 2

                        width: parent.width
                        spacing: 14

                        Repeater {
                            id: styleRepeater
                            model: styleCards.styles

                            delegate: ThemeCard {
                                required property int index
                                required property string modelData

                                objectName: "styleCard_" + modelData
                                width: Math.min(196, (styleCards.width - styleCards.spacing * (styleCards.columns - 1)) / styleCards.columns)
                                look: Theme.lookFor(modelData)
                                label: styleCards.labels[index]
                                selected: window.editor.uiStyle === modelData
                                onActivated: if (!selected) window.changeAppearance(() => window.editor.uiStyle = modelData)
                                Keys.onLeftPressed: styleRepeater.itemAt(Math.max(0, index - 1)).forceActiveFocus()
                                Keys.onRightPressed: styleRepeater.itemAt(Math.min(styleCards.styles.length - 1, index + 1)).forceActiveFocus()
                                Keys.onUpPressed: styleRepeater.itemAt(Math.max(0, index - styleCards.columns)).forceActiveFocus()
                                Keys.onDownPressed: styleRepeater.itemAt(Math.min(styleCards.styles.length - 1, index + styleCards.columns)).forceActiveFocus()
                            }
                        }
                    }
                }

                SettingRow {
                    id: transparencyRow
                    shown: SystemTheme.backdropSystem // not on Windows 10
                    title: qsTr("透明效果")
                    unlocks: [glassTransparencyRow, glassClarityRow]
                    keywords: qsTr("云母;Mica;毛玻璃;亚克力;半透明;壁纸;transparency").split(";")
                    description: !Theme.backdropCapable
                                 ? qsTr("当前界面风格的窗口不透明；可切换到精修原生、品牌色彩或液态玻璃")
                                 : !SystemTheme.backdropAvailable
                                 ? qsTr("需要把“常规”里的界面绘制方式设为“显卡加速”")
                                 : !SystemTheme.materials
                                 ? qsTr("要先打开 Windows 设置里的“透明效果”")
                                 : Theme.style === "liquid"
                                 ? qsTr("使用系统亚克力模糊窗口背后的内容；关闭后保留实色玻璃质感")
                                 : qsTr("窗口背景透出桌面壁纸的颜色（云母效果）")

                    FlatButton {
                        objectName: "appearanceRendererButton"
                        dpr: window.dpr
                        visible: Theme.backdropCapable && !SystemTheme.backdropAvailable
                        flat: true
                        text: qsTr("去设置")
                        trailingGlyph: "\uE76C"
                        onClicked: window.showPart(window.generalPage, "rendererRow")
                    }
                    ScopeTabs {
                        enabled: SystemTheme.backdropAvailable && Theme.backdropCapable
                        opacity: enabled ? 1 : 0.4
                        labels: [qsTr("关"), qsTr("开")]
                        current: Math.max(0, window.transparencies.indexOf(window.editor.transparency))
                        onActivated: (index) => window.editor.transparency = window.transparencies[index]
                    }
                    FlatButton {
                        visible: window.editor.uiStyle === "liquid" && window.editor.transparency === "on"
                        enabled: window.editor.glassTransparency !== 50 || window.editor.glassClarity !== 50
                        text: qsTr("恢复默认")
                        onClicked: window.editor.resetGlassAppearance()
                    }
                }

                SettingRow {
                    id: glassTransparencyRow
                    objectName: "glassTransparencyRow"
                    sub: true
                    shown: SystemTheme.backdropSystem && window.editor.uiStyle === "liquid" && window.editor.transparency === "on"
                    title: qsTr("透明程度")
                    description: qsTr("越高，窗口背后的颜色越明显；文字和按钮保持清晰")
                    keywords: qsTr("透明度;通透;玻璃;opacity;transparency;glass").split(";")
                    SettingSlider {
                        objectName: "glassTransparencyControl"
                        label: glassTransparencyRow.title
                        enabled: SystemTheme.backdropAvailable && SystemTheme.materials
                        value: window.editor.glassTransparency
                        onPreviewed: (value) => window.editor.previewGlassAppearance(value, window.editor.glassClarity)
                        onCommitted: (value) => window.editor.glassTransparency = value
                    }
                }

                SettingRow {
                    id: glassClarityRow
                    objectName: "glassClarityRow"
                    sub: true
                    shown: glassTransparencyRow.shown
                    title: qsTr("卡片清晰度")
                    description: qsTr("越高，卡片和工具栏的底色越实，更容易阅读")
                    keywords: qsTr("清晰度;卡片;玻璃;clarity;surface;glass").split(";")
                    SettingSlider {
                        objectName: "glassClarityControl"
                        label: glassClarityRow.title
                        enabled: SystemTheme.backdropAvailable && SystemTheme.materials
                        value: window.editor.glassClarity
                        onPreviewed: (value) => window.editor.previewGlassAppearance(window.editor.glassTransparency, value)
                        onCommitted: (value) => window.editor.glassClarity = value
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

                SettingRow { // GitHub #412
                    id: searchShadowRow
                    // 浅 is a rounded window's lighter shadow: a square one has none at all.
                    readonly property bool light: SystemTheme.nativeWindowBorder && window.editor.corners === "round"
                    title: qsTr("搜索框阴影")
                    description: light ? qsTr("“浅”阴影更小更淡；“无”不显示阴影，搜索框随之变成直角")
                                       : qsTr("“无”时搜索框周围不显示阴影")
                    tip: light ? qsTr("Windows 只给带阴影的窗口画圆角。搜索面板和其他窗口照常有阴影") : qsTr("搜索面板和其他窗口照常有阴影")
                    keywords: qsTr("阴影;投影;影子;黑边;发黑;搜索框;搜索窗口;shadow;drop shadow").split(";")

                    ScopeTabs {
                        objectName: "searchShadowTabs"
                        readonly property var shadows: searchShadowRow.light ? ["standard", "light", "none"] : ["standard", "none"]
                        readonly property string shown: window.editor.searchShadow === "light" && !searchShadowRow.light
                            ? "none" : window.editor.searchShadow // the same as none here
                        labels: searchShadowRow.light ? [qsTr("标准"), qsTr("浅"), qsTr("无")] : [qsTr("标准"), qsTr("无")]
                        current: Math.max(0, shadows.indexOf(shown))
                        onActivated: (index) => window.editor.searchShadow = shadows[index]
                    }
                }
            }

            SettingsSection {
                page: window.appearancePage
                width: parent.width
                title: qsTr("尺寸与布局")
                glyph: "\uE740"

                SettingRow {
                    title: qsTr("界面字体")
                    stackControl: width < 600
                    keywords: qsTr("字体;雅黑;普惠体;清晰;font;typeface;YaHei").split(";")
                    values: [window.editor.uiFont]
                    description: window.editor.uiFontAvailable
                        ? qsTr("可在列表里查找名称，示例即时预览；重启后应用")
                        : qsTr("此电脑未安装“%1”，暂用默认字体").arg(window.editor.uiFont)
                    FontPicker {
                        frame: window.frame
                        families: window.editor.uiFonts
                        selectedFamily: window.editor.uiFont
                        onChosen: family => window.editor.uiFont = family
                    }
                    body: Item {
                        width: parent.width
                        height: Math.max(fontSample.implicitHeight, fontRestart.visible ? fontRestart.height : 0)
                        Text {
                            id: fontSample
                            objectName: "uiFontSample"
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - (fontRestart.visible ? fontRestart.width + 16 : 0)
                            text: qsTr("文件搜索 · 剪贴板 · 设置  WinShun 0123456789")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            font.family: window.editor.uiFontPreview
                            wrapMode: Text.Wrap
                        }
                        FlatButton {
                            id: fontRestart
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: window.editor.fontRestartRequired
                            text: qsTr("重启后应用")
                            onClicked: window.editor.restart()
                        }
                    }
                }

                SettingRow {
                    id: uiSizeRow
                    title: qsTr("界面大小")
                    keywords: qsTr("缩放;大小;字号;字体大小;行高;密集;紧凑;宽松;一行;小字;Ctrl++;Ctrl+-;Ctrl+0;scale;size;zoom;font;dense;compact").split(";")
                    description: qsTr("搜索框、搜索面板、设置窗口、剪贴板和对话框旁搜索框的文字与行高；也可按 Ctrl++ / Ctrl+- / Ctrl+0 快速切换。密集是每条结果一行的小字")

                    ScopeTabs {
                        readonly property var sizes: ["dense", "compact", "standard", "roomy"]
                        labels: [qsTr("密集"), qsTr("紧凑"), qsTr("标准"), qsTr("宽松")]
                        current: Math.max(0, sizes.indexOf(window.editor.uiSize))
                        onActivated: (index) => window.editor.uiSize = sizes[index]
                    }

                    body: AppearancePreview { width: parent.width }
                }

                SettingsLinkRow {
                    objectName: "appearanceClipboardSizeLink"
                    sub: true
                    iconName: "clipboard-text"
                    title: qsTr("剪贴板可单独设置大小")
                    description: {
                        const labels = {
                            follow: qsTr("跟随界面大小"),
                            dense: qsTr("密集"),
                            compact: qsTr("紧凑"),
                            standard: qsTr("标准"),
                            roomy: qsTr("宽松")
                        }
                        return qsTr("当前：%1").arg(labels[window.editor.clipboardUiSize] ?? qsTr("跟随界面大小"))
                    }
                    destination: qsTr("剪贴板 › 剪贴板大小")
                    targetPage: "clipboard"
                    targetPart: "clipboardUiSizeRow"
                }

                SettingRow {
                    title: qsTr("搜索框宽度")
                    keywords: qsTr("宽度;加宽;拉宽;调宽;变宽;窄;文件名太长;显示不全;width;wide").split(";")
                    description: qsTr("可拖动搜索框左右边缘调宽，双击边缘恢复默认")
                    tip: qsTr("文件名仍显示不全时，鼠标悬停在该行上会显示完整名称和路径")

                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("恢复默认宽度")
                        enabled: window.editor.launcherWidthSet
                        onClicked: window.editor.resetLauncherWidth()
                    }
                }

                SettingRow {
                    title: qsTr("搜索框显示行数")
                    keywords: qsTr("行数;高度;加高;变高;结果数;条数;显示更多;rows;height;results").split(";")
                    description: qsTr("结果超过这个行数时滚动查看，可填 4–20 行，默认 8 行；屏幕下方放不下时自动减少")

                    InputBox {
                        objectName: "maxRowsInput"
                        dpr: window.dpr
                        anchors.verticalCenter: parent.verticalCenter
                        width: 80
                        horizontalAlignment: TextInput.AlignHCenter
                        text: String(window.editor.maxRows)
                        Accessible.name: qsTr("搜索框显示行数")
                        validator: IntValidator { bottom: 1; top: 20 }
                        onEditingFinished: {
                            const rows = parseInt(text, 10)
                            if (rows > 0)
                                window.editor.maxRows = rows
                            text = Qt.binding(() => String(window.editor.maxRows))
                        }
                    }
                }

                SettingRow { // GitHub #401
                    title: qsTr("记住窗口大小")
                    description: qsTr("设置、截图设置和图片文字搜索窗口按你上次拖出的大小打开，上次最大化的仍然最大化。关掉后每次启动都用默认大小，已记住的大小会清除")
                    tip: qsTr("双击这些窗口的边框，随时恢复默认大小")
                    keywords: qsTr("记住;窗口大小;尺寸;大小;最大化;默认大小;设置窗口;remember;window size;size;maximize").split(";")
                    ToggleSwitch {
                        objectName: "rememberWindowSizeToggle"
                        dpr: window.dpr
                        checked: window.editor.rememberWindowSize
                        Accessible.name: qsTr("记住窗口大小")
                        onToggled: (on) => window.editor.rememberWindowSize = on
                    }
                }

                SettingRow { // GitHub #445
                    id: settingsEdgeScrollRow
                    objectName: "settingsEdgeScrollRow"
                    readonly property var modes: ["off", "light", "standard", "steady"]
                    title: qsTr("设置页滚轮触边翻页")
                    keywords: qsTr("翻页;自动翻页;滚轮;触底;触顶;切页;灵敏度;误触;后退;前进;edge;scroll;page;sensitivity").split(";")
                    description: {
                        switch (window.editor.settingsEdgeScroll) {
                        case "off":
                            return qsTr("滚到顶部或底部时停在本页，不自动切到上一页或下一页")
                        case "light":
                            return qsTr("滚到头略停后继续滚约 2 格即可切页，反应更轻快")
                        case "steady":
                            return qsTr("滚到头停顿更久并继续滚约 5 格才切页，最大限度防误触")
                        default:
                            return qsTr("滚到头停顿后继续滚约 3 格切页，快速连滚不会误触发")
                        }
                    }

                    ScopeTabs {
                        objectName: "settingsEdgeScrollTabs"
                        labels: [qsTr("关闭"), qsTr("轻快"), qsTr("标准"), qsTr("稳重")]
                        current: Math.max(0, settingsEdgeScrollRow.modes.indexOf(window.editor.settingsEdgeScroll))
                        onActivated: (index) => window.editor.settingsEdgeScroll = settingsEdgeScrollRow.modes[index]
                    }
                }

                SettingRow { // GitHub #365, #377
                    title: qsTr("弹出动画")
                    description: qsTr("搜索框和剪贴板打开时轻轻滑入并淡入，关闭时淡出。关掉后立即出现、立即消失")
                    tip: qsTr("Windows 设置里关掉了“动画效果”时，始终没有动画")
                    keywords: qsTr("动画;动效;淡入;淡出;弹出;升起;过渡;拖沓;立即显示;animation;motion;fade").split(";")
                    ToggleSwitch {
                        objectName: "openAnimationToggle"
                        dpr: window.dpr
                        checked: window.editor.openAnimation
                        Accessible.name: qsTr("弹出动画")
                        onToggled: (on) => window.editor.openAnimation = on
                    }
                }

                SettingRow { // GitHub #368, #440
                    id: hoverTipsRow
                    title: qsTr("悬停提示")
                    description: qsTr("鼠标停在按钮、图标或文件夹菜单上时弹出说明；开启后可在下方按类别细分")
                    tip: qsTr("关掉后，设置里带“i”的说明照常显示；名字太长被截断时，停在行上仍会显示完整名字")
                    keywords: qsTr("悬停;提示;气泡;工具提示;说明;遮挡;tooltip;hover;tip").split(";")
                    unlocks: [hoverTipKindsRow]
                    ToggleSwitch {
                        objectName: "hoverTipsToggle"
                        dpr: window.dpr
                        checked: window.editor.hoverTips
                        Accessible.name: qsTr("悬停提示")
                        onToggled: (on) => window.editor.hoverTips = on
                    }
                }

                SettingRow { // GitHub #440
                    id: hoverTipKindsRow
                    sub: true
                    shown: window.editor.hoverTips
                    stackControl: true
                    title: qsTr("显示哪些悬停提示")
                    description: qsTr("至少保留一项；未勾选的不再弹出。截断的完整名称和设置里的“i”说明不受影响")
                    keywords: qsTr("悬停;提示;分类;界面;按钮;类型条;结果行;操作;剪贴板;文件夹菜单;路径;tooltip;hover;actions;paths").split(";")

                    ScopeTabs {
                        objectName: "hoverTipKindsTabs"
                        multiSelect: true
                        dpr: window.dpr
                        labels: [qsTr("界面与按钮"), qsTr("结果行操作"), qsTr("文件夹菜单路径")]
                        checkedIndices: {
                            const list = []
                            if (window.editor.hoverTipsInterface) list.push(0)
                            if (window.editor.hoverTipsActions) list.push(1)
                            if (window.editor.hoverTipsPaths) list.push(2)
                            return list
                        }
                        onToggled: (index, on) => {
                            if (index === 0) window.editor.hoverTipsInterface = on
                            else if (index === 1) window.editor.hoverTipsActions = on
                            else if (index === 2) window.editor.hoverTipsPaths = on
                        }
                    }
                }

                SettingRow { // with what the box looks like, where it is looked for (GitHub #185)
                    title: qsTr("纯净搜索模式")
                    description: qsTr("搜索框空着时只保留输入栏，输入后恢复结果和范围标签；右上角的搜索历史和展开按钮一直隐藏，快捷键照常可用。完整搜索窗口、文件夹内搜索和任务栏搜索不受影响")
                    keywords: qsTr("纯净;极简;空白;底栏;最近;收藏;pure;minimal").split(";")
                    ToggleSwitch {
                        objectName: "pureSearchToggle"
                        dpr: window.dpr
                        checked: window.editor.pureSearch
                        Accessible.name: qsTr("纯净搜索模式")
                        onToggled: (on) => window.editor.pureSearch = on
                    }
                }

                SettingRow { // GitHub #321
                    title: qsTr("搜索面板空着时列出全部文件")
                    description: qsTr("关掉后，搜索面板里没输入文字、也没选筛选条件时不列任何文件，输入后才显示结果")
                    keywords: qsTr("面板;完整搜索窗口;空白;默认;全部文件;不显示结果;panel;empty;everything").split(";")
                    ToggleSwitch {
                        objectName: "panelListsAllToggle"
                        dpr: window.dpr
                        checked: window.editor.panelListsAll
                        Accessible.name: qsTr("搜索面板空着时列出全部文件")
                        onToggled: (on) => window.editor.panelListsAll = on
                    }
                }

                SettingRow { // GitHub #185, #244
                    title: qsTr("范围标签")
                    keywords: qsTr("范围;全部;文件;内容;标签;合并;三合一;按钮;极简;scope;tabs").split(";")
                    description: qsTr("搜索框右边的“全部 / 文件 / 内容”：拆分成几个标签，或合并成一个按钮，点一下切到下一个")
                    tip: qsTr("两种样式下 Tab / Shift+Tab 都能切换；展开的搜索面板始终拆分显示")

                    ScopeTabs {
                        objectName: "scopeButtonChoice"
                        labels: [qsTr("拆分"), qsTr("合并")]
                        current: window.editor.scopeButton ? 1 : 0
                        onActivated: (index) => window.editor.scopeButton = index === 1
                    }
                }
            }

            SettingsSection {
                page: window.appearancePage
                width: parent.width
                title: qsTr("图标与语言")
                glyph: "\uE774"

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
            // </page appearance>
                } }
            }

            PageSlot {
                id: scopeSlot
                index: window.scopePage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({ foldersRow: foldersRow, sharesRow: sharesRow, indexAccessRow: indexAccessRow, indexServiceRow: indexServiceRow, nameInput: nameInput })
                    onResetForReopen: {
                        for (const field of [nameInput, typeInput, fileNameInput, shareInput, extensionInput])
                            field.clear()
                        foldersRow.problem = ""
                        foldersRow.networkPath = ""
                        sharesRow.listedServer = ""
                        sharesRow.listedShares = []
                        sharesRow.listProblem = ""
                        sharesRow.listSignIn = false
                        sharesRow.listing = false
                        shareProblem.text = ""
                    }
            // <page scope>
            SettingsSection {
                page: window.scopePage
                tab: 0
                width: parent.width
                title: qsTr("索引范围与包含目录")
                glyph: "\uE8B7"
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
                            window.showPage(window.scopePage, 3) // 网络共享, where it is now
                    }

                    title: qsTr("索引的文件夹")
                    keywords: qsTr("文件夹;添加文件夹;手动输入;环境变量;工作文件夹;桌面;文档;下载;立即核对;folders").split(";")
                    values: window.editor.folders.map(folder => folder.path)
                    description: window.editor.foldersInEffect
                                 ? qsTr("只有这些文件夹里的文件能搜到；可手动输入（支持 %USERPROFILE% 等环境变量）或拖入文件夹")
                                 : qsTr("普通文件夹已包含在整盘索引中；mklink 创建的链接文件夹可在这里额外添加，管理员和索引服务模式也能用。")

                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("选择文件夹…")
                        glyph: "" // Add
                        onClicked: {
                            const path = window.editor.pickFolder(qsTr("选择要索引的文件夹"))
                            if (path.length > 0)
                                foldersRow.add(path)
                        }
                    }

                    body: [
                        Row {
                            width: parent.width
                            spacing: 8

                            InputBox {
                                id: indexFolderInput
                                objectName: "indexFolderInput"
                                dpr: window.dpr
                                width: parent.width - addIndexFolderButton.width - 8
                                placeholder: qsTr("输入或粘贴文件夹路径，例如 D:\\Work 或 %USERPROFILE%\\Documents")
                                onTextChanged: if (foldersRow.problem.length > 0) { foldersRow.problem = ""; foldersRow.networkPath = "" }
                                onAccepted: if (foldersRow.add(text)) clear()
                            }
                            FlatButton {
                                id: addIndexFolderButton
                                dpr: window.dpr
                                text: qsTr("添加")
                                enabled: indexFolderInput.text.trim().length > 0
                                onClicked: if (foldersRow.add(indexFolderInput.text)) indexFolderInput.clear()
                            }
                        },
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
                                dpr: window.dpr
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
                                        dpr: window.dpr
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
                    sub: true

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
            }

            SettingsSection {
                page: window.scopePage
                tab: 0
                width: parent.width
                title: qsTr("排除目录")
                glyph: "\uE8F8"

                SettingRow {
                    id: excludedFoldersRow
                    property string problem
                    function add(text) {
                        const problem = window.editor.addExcludedPath(text)
                        excludedFoldersRow.problem = problem
                        return problem.length === 0
                    }

                    title: qsTr("不搜索的文件夹")
                    keywords: qsTr("排除;忽略;隐藏;黑名单;手动输入;环境变量;exclude").split(";")
                    values: window.editor.excludedPaths
                    // GitHub #351: indexed all the same for the programs asking over the Everything interface.
                    description: window.editor.everythingCompat
                        ? qsTr("这些文件夹里的内容都不出现在结果里，支持 %TEMP% 等环境变量。“让其他软件把 Win顺 当作 Everything 搜索”开着时仍建索引，其他软件照样查得到（如 Total Commander 的文件夹大小）")
                        : qsTr("这些文件夹里的内容都不出现在结果里，支持手动输入和 %TEMP% 等环境变量")

                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("选择文件夹…")
                        glyph: "" // Add
                        onClicked: {
                            const path = window.editor.pickFolder(qsTr("选择不需要搜索的文件夹"))
                            if (path.length > 0)
                                excludedFoldersRow.add(path)
                        }
                    }

                    body: [
                        Row {
                            width: parent.width
                            spacing: 8

                            InputBox {
                                id: excludedFolderInput
                                objectName: "excludedFolderInput"
                                dpr: window.dpr
                                width: parent.width - addExcludedFolderButton.width - 8
                                placeholder: qsTr("输入或粘贴文件夹路径，例如 D:\\Cache 或 %TEMP%")
                                onTextChanged: if (excludedFoldersRow.problem.length > 0) excludedFoldersRow.problem = ""
                                onAccepted: if (excludedFoldersRow.add(text)) clear()
                            }
                            FlatButton {
                                id: addExcludedFolderButton
                                dpr: window.dpr
                                text: qsTr("添加")
                                enabled: excludedFolderInput.text.trim().length > 0
                                onClicked: if (excludedFoldersRow.add(excludedFolderInput.text)) excludedFolderInput.clear()
                            }
                        },
                        Text {
                            visible: excludedFoldersRow.problem.length > 0
                            width: parent.width
                            text: excludedFoldersRow.problem
                            textFormat: Text.PlainText
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        },
                        Rectangle {
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
                                    readonly property string resolvedPath: window.editor.resolveEnvPath(modelData)

                                    width: pathList.width
                                    height: 42

                                    Rectangle {
                                        visible: pathRow.index > 0
                                        x: 12
                                        width: parent.width - 24
                                        height: Theme.edge(window.dpr)
                                        color: Theme.divider
                                    }
                                    Glyph {
                                        id: folderIcon
                                        x: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        glyph: "" // Folder
                                        size: 16
                                    }
                                    Text {
                                        id: excludedPathLabel
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

                                        HoverHandler { id: excludedPathHover }
                                        HoverTip {
                                            target: excludedPathHover.hovered && pathRow.resolvedPath.length > 0 ? excludedPathLabel : null
                                            text: qsTr("当前展开为：%1").arg(pathRow.resolvedPath)
                                        }
                                    }
                                    FlatButton {
                                        dpr: window.dpr
                                        id: removePath
                                        anchors.right: parent.right
                                        anchors.rightMargin: 6
                                        anchors.verticalCenter: parent.verticalCenter
                                        implicitHeight: 32
                                        flat: true
                                        text: qsTr("移除")
                                        onClicked: {
                                            excludedFoldersRow.problem = ""
                                            window.editor.removeExcludedPath(pathRow.index)
                                        }
                                    }
                                }
                            }
                        }
                    }, Text { // some files only: by name, on the next tab (GitHub #374)
                        text: qsTr("只想跳过某些文件？到“名称和类型”里按文件名（可用 *）或扩展名跳过") + "  ›"
                        color: filesLink.pressed ? Theme.subtext : Theme.accent
                        font.pixelSize: Theme.fontCaption
                        font.underline: filesLink.containsMouse
                        Accessible.role: Accessible.Link
                        Accessible.name: text

                        MouseArea {
                            id: filesLink
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: window.showTab(1)
                        }
                    }]
                }
            }

            SettingsSection {
                page: window.scopePage
                tab: 1
                width: parent.width
                title: qsTr("按名称与扩展名跳过")
                glyph: "\uE71C"
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
                                dpr: window.dpr
                                id: nameInput
                                width: 240
                                placeholder: qsTr("输入文件夹名称，例如 build")
                                onAccepted: if (window.editor.addExcludedName(text)) clear()
                            }
                            FlatButton {
                                dpr: window.dpr
                                text: qsTr("添加")
                                enabled: nameInput.text.trim().length > 0
                                onClicked: if (window.editor.addExcludedName(nameInput.text)) nameInput.clear()
                            }
                        }
                    ]
                }
            }

            SettingsSection {
                page: window.scopePage
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
                                dpr: window.dpr
                                id: typeInput
                                width: 240
                                placeholder: qsTr("例如 wbk, bak")
                                onAccepted: if (window.editor.addExcludedTypes(text)) clear()
                            }
                            FlatButton {
                                dpr: window.dpr
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
                                dpr: window.dpr
                                id: fileNameInput
                                width: 240
                                placeholder: qsTr("例如 ~$*, Thumbs.db")
                                onAccepted: if (window.editor.addExcludedFileNames(text)) clear()
                            }
                            FlatButton {
                                dpr: window.dpr
                                text: qsTr("添加")
                                enabled: fileNameInput.text.trim().length > 0
                                onClicked: if (window.editor.addExcludedFileNames(fileNameInput.text)) fileNameInput.clear()
                            }
                        }
                    ]
                }
            }

            SettingsSection {
                page: window.scopePage
                tab: 2
                width: parent.width
                title: qsTr("磁盘范围")
                glyph: "\uEDA2"
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
                                        dpr: window.dpr
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
                            dpr: window.dpr
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
                        dpr: window.dpr
                        checked: window.editor.includeRemovableDrives
                        onToggled: (on) => window.editor.includeRemovableDrives = on
                    }
                }

                SettingRow {
                    title: qsTr("启动时检查文件变化")
                    keywords: qsTr("重新扫描;扫描;rescan").split(";")
                    description: qsTr("补上 U 盘等非 NTFS 磁盘在 Win顺 关闭期间的改动")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.rescanOnStartup
                        onToggled: (on) => window.editor.rescanOnStartup = on
                    }
                }
            }

            SettingsSection {
                page: window.scopePage
                tab: 2
                width: parent.width
                title: qsTr("索引引擎与服务")
                glyph: "\uE950"

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
                            what = qsTr("启动时请求管理员权限，直接读取主文件表和变更日志；取消后本次用普通权限，保留选择。开机自启也以管理员身份")
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
                        dpr: window.dpr
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
                    objectName: "indexServiceRow"
                    sub: true

                    // A portable copy has no installer to register the service:
                    // done from here, as administrator (UAC asks once).
                    // An account that cannot install it (not an administrator's,
                    // GitHub #52) gets the reason instead of the buttons.
                    shown: window.editor.isPortable
                    title: qsTr("索引服务")
                    keywords: qsTr("便携;安装服务;更新服务;卸载服务;索引服务;没有管理员权限;装不了;portable;service").split(";")
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
                                 : window.editor.indexServiceUpdatable
                                 ? qsTr("索引服务还是 %1 版，比这份 Win顺 旧，有些功能用不上。点“更新索引服务”，Windows 会请你确认一次").arg(window.editor.indexServiceVersion)
                                 : window.editor.indexServiceInstalled
                                 ? qsTr("已安装 Windows 服务“Win顺 索引服务”，它替普通权限的 Win顺 读取主文件表")
                                 : qsTr("安装一个 Windows 服务，替普通权限的 Win顺 读取主文件表，建索引快得多；需要管理员权限一次")
                    bodyShown: window.editor.indexServiceMessage.length > 0

                    FlatButton {
                        dpr: window.dpr
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
                        dpr: window.dpr
                        // This copy's own service, older than it: --install upgrades it in place.
                        visible: window.editor.indexServiceUpdatable
                        text: qsTr("更新索引服务")
                        enabled: !window.editor.indexServiceBusy
                        onClicked: window.editor.installIndexService()
                    }
                    FlatButton {
                        dpr: window.dpr
                        visible: window.editor.indexServiceInstallable && window.editor.indexServiceInstalled
                                 && !window.editor.indexServiceUnsafeLocation
                        text: qsTr("卸载索引服务")
                        enabled: !window.editor.indexServiceBusy
                        onClicked: window.editor.uninstallIndexService()
                    }

                    body: [
                        Text {
                            width: parent.width
                            text: window.editor.indexServiceMessage
                            textFormat: Text.PlainText
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            lineHeight: 1.3
                            wrapMode: Text.Wrap
                        },
                        // A folder's permissions stopped the install: the
                        // commands that put them right, to copy (GitHub #426).
                        Row {
                            id: serviceRepair
                            objectName: "indexServiceRepair"
                            property bool copied: false
                            width: parent.width
                            spacing: 8
                            visible: window.editor.indexServiceRepair.length > 0

                            Text {
                                width: parent.width - copyServiceRepair.width - parent.spacing
                                anchors.verticalCenter: parent.verticalCenter
                                text: window.editor.indexServiceRepair
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontCaption
                                lineHeight: 1.3
                                wrapMode: Text.WrapAnywhere
                            }
                            FlatButton {
                                id: copyServiceRepair
                                dpr: window.dpr
                                anchors.verticalCenter: parent.verticalCenter
                                text: serviceRepair.copied ? qsTr("已复制") : qsTr("复制命令")
                                onClicked: {
                                    window.editor.copyText(window.editor.indexServiceRepair.replace(/\n/g, "\r\n"))
                                    serviceRepair.copied = true
                                    serviceRepairCopied.restart()
                                }
                            }
                            Timer {
                                id: serviceRepairCopied
                                interval: 2000
                                onTriggered: serviceRepair.copied = false
                            }
                        }
                    ]
                }

                SettingsLinkRow {
                    objectName: "scopeIndexFolderLink"
                    iconName: "folder-open"
                    title: qsTr("索引位置")
                    description: window.editor.indexSize.length > 0
                                 ? qsTr("当前 %1 · %2").arg(window.editor.indexSize).arg(window.editor.indexFolder)
                                 : window.editor.indexFolder
                    destination: qsTr("常规 › 存储")
                    targetPage: "general"
                    targetPart: "indexFolderRow"
                }
            }

            SettingsSection {
                page: window.scopePage
                tab: 3
                width: parent.width
                note: qsTr("默认只收录文件名和文件夹名；打开“搜索文件内容”的，会在电脑空闲时读取文件内容。服务器离线或 WSL 停止时，收录过的照样能搜到；恢复后自动更新。")

                SettingRow {
                    id: sharesRow
                    title: qsTr("网络与 WSL 文件夹")
                    keywords: qsTr("网络;共享;局域网;内网;NAS;服务器;SMB;映射;网络驱动器;WSL;Linux;Ubuntu;子系统;WebDAV;内容;全文;搜索文件内容;扫描;时段;时间;层数;深度;network;share;content;schedule;depth").split(";")
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
                                dpr: window.dpr
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
                                dpr: window.dpr
                                visible: sharesRow.typedServer.length === 0
                                text: qsTr("添加")
                                enabled: shareInput.text.trim().length > 0
                                onClicked: sharesRow.add(shareInput.text)
                            }
                            FlatButton {
                                dpr: window.dpr
                                visible: sharesRow.typedServer.length > 0
                                text: sharesRow.typedWsl ? qsTr("查看 WSL 发行版") : qsTr("查看共享的文件夹")
                                onClicked: sharesRow.listShares(sharesRow.typedServer)
                            }
                            FlatButton {
                                dpr: window.dpr
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
                                    dpr: window.dpr
                                    visible: sharesRow.listSignIn
                                    highlighted: true
                                    text: qsTr("登录")
                                    onClicked: window.editor.signInToServer(sharesRow.listedServer)
                                }
                                Repeater {
                                    model: sharesRow.listedShares

                                    delegate: FlatButton {
                                        dpr: window.dpr
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
                                        dpr: window.dpr
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
                                        dpr: window.dpr
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
                                    onScanEdited: (scheduled, from, until, depth) => window.editor.setShareScan(index, scheduled, from, until, depth)
                                }
                            }
                        }
                    ]
                }
            }

            SettingsSection {
                page: window.scopePage
                tab: 4
                width: parent.width
                title: qsTr("全文搜索与内容索引")
                glyph: "\uE8A5"
                note: qsTr("在搜索框按 Tab 切换到“内容”，可以查找文件里的文字。")

                SettingRow {
                    title: qsTr("文件内容搜索")
                    keywords: qsTr("内容;全文;文字;开关;关闭;content").split(";")
                    unlocks: [documentsRow, extensionsRow, lowPriorityRow, contentIndexRow]
                    description: qsTr("关掉后搜索框里不再有“内容”，“全部”只按名字找；Win顺 不读取文件里的文字，内容索引也会清空")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.contentSearch
                        onToggled: (on) => window.editor.contentSearch = on
                    }
                }

                SettingRow {
                    id: documentsRow
                    sub: true
                    shown: window.editor.contentSearch
                    title: qsTr("搜索文档")
                    keywords: qsTr("Word;Excel;PowerPoint;PPT;PDF;WPS;docx;xlsx").split(";")
                    unlocks: sizeRows.count > 3 ? [sizeRows.itemAt(3)] : []
                    description: qsTr("Word、Excel、PowerPoint、PDF 和 WPS 文件，图片里的文字除外")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.contentDocuments
                        onToggled: (on) => window.editor.contentDocuments = on
                    }
                }

                SettingRow {
                    id: extensionsRow
                    sub: true
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
                                dpr: window.dpr
                                id: extensionInput
                                width: 240
                                placeholder: qsTr("例如 md, log")
                                onAccepted: if (window.editor.addContentExtensions(text)) clear()
                            }
                            FlatButton {
                                dpr: window.dpr
                                text: qsTr("添加")
                                enabled: extensionInput.text.trim().length > 0
                                onClicked: if (window.editor.addContentExtensions(extensionInput.text)) extensionInput.clear()
                            }
                        }
                    ]
                }

                SettingRow {
                    id: lowPriorityRow
                    sub: true
                    shown: window.editor.contentSearch
                    title: qsTr("也搜索系统和程序文件夹")
                    keywords: qsTr("系统文件夹;Program Files;AppData;Windows;node_modules").split(";")
                    description: qsTr("Windows、Program Files、AppData 等；文件很多，搜索会变慢")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.contentInLowPriority
                        onToggled: (on) => window.editor.contentInLowPriority = on
                    }
                }

                SettingRow {
                    id: contentIndexRow
                    sub: true
                    shown: window.editor.contentSearch
                    title: qsTr("建立内容索引")
                    keywords: qsTr("索引;加速;index").split(";")
                    description: qsTr("在后台记下文件里的文字，搜索快得多。只用于 NTFS 磁盘，约占一两百 MB")
                                 + (window.editor.contentIndexStatus.length > 0 ? "\n" + window.editor.contentIndexStatus : "")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.contentIndex
                        onToggled: (on) => window.editor.contentIndex = on
                    }
                }
            }

            SettingsSection {
                page: window.scopePage
                tab: 4
                shown: window.editor.contentSearch
                width: parent.width
                title: qsTr("单文件读取上限")
                glyph: "\uE74C"
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
                            dpr: window.dpr
                            glyph: "" // Remove (minus)
                            enabled: !sizeRow.inKB || sizeRow.kb > 1
                            onClicked: window.editor.setContentMaxSizeMB(sizeRow.index, sizeRow.down)
                        }
                        InputBox {
                            dpr: window.dpr
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
                            dpr: window.dpr
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
            // </page scope>
                } }
            }

            PageSlot {
                id: resultsSlot
                index: window.resultsPage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({
                        fileTypesSection: fileTypesSection,
                        quickFiltersSection: quickFiltersSection,
                        favoritesOnStartRow: favoritesOnStartRow,
                        favoriteFolderTabRow: favoriteFolderTabRow,
                        previewHotkeyRow: previewHotkeyRow,
                        openHotkeyRow: openHotkeyRow,
                        resultNumberKeysRow: resultNumberKeysRow,
                        letterJumpRow: letterJumpRow,
                        queryHistoryKeysRow: queryHistoryKeysRow
                    })
                    recordingHotkey: previewHotkeyRecorder.recording || openHotkeyRecorder.recording
                    onResetForReopen: {
                        previewHotkeyRecorder.stop()
                        openHotkeyRecorder.stop()
                        fileTypesSection.resetForReopen()
                        quickFiltersSection.resetForReopen()
                        clearHistoryButton.confirming = false
                    }
            // <page results>
            SettingsSection {
                page: window.resultsPage
                tab: 0
                width: parent.width
                title: qsTr("排序与类型优先级")
                glyph: "\uE8CB"

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
                    keywords: qsTr("排序;修改时间;修改日期;最新;最早;名称;文件名;所在文件夹;目录;类型;扩展名;大小;升序;降序;次数;打开次数;运行次数;最常用;sort;date;size;type;count").split(";")
                    description: qsTr("文件和文件夹怎么排（“全部”里应用和设置仍在前面）；底栏左边也能切换（Ctrl+S 在前三种里轮换）")

                    readonly property var sorts: ["match", "newest", "oldest", "name", "folder", "type", "largest", "smallest", "opened"]
                    readonly property int chosen: sorts.indexOf(window.editor.sort)

                    ScopeTabs {
                        labels: [qsTr("最匹配"), qsTr("最新在前"), qsTr("最早在前")]
                        current: sortRow.chosen < 3 ? Math.max(0, sortRow.chosen) : -1
                        onActivated: (index) => window.editor.sort = sortRow.sorts[index]
                    }

                    // Too many for one line (#44): the other orders under the first.
                    body: [
                        ScopeTabs {
                            labels: [qsTr("名称"), qsTr("所在文件夹"), qsTr("类型"), qsTr("大的在前"), qsTr("小的在前"), qsTr("打开最多")]
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
                        dpr: window.dpr
                        visible: !window.editor.preferredTypesDefault
                        text: qsTr("恢复默认")
                        onClicked: window.editor.resetPreferredTypes()
                    }

                    body: [
                        InputBox {
                            dpr: window.dpr
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
                page: window.resultsPage
                tab: 1
                width: parent.width
                title: qsTr("附加搜索源与匹配模式")
                glyph: "\uE721"

                SettingRow {
                    title: qsTr("搜索应用")
                    keywords: qsTr("应用;程序;软件;开始菜单;启动;全部;关闭;app").split(";")
                    description: qsTr("在“全部”里列出开始菜单里的程序和应用；关掉后只找文件和内容（收藏的应用仍会列出，在开始菜单里打字也一样）")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.searchApps
                        Accessible.name: qsTr("搜索应用")
                        onToggled: (on) => window.editor.searchApps = on
                    }
                }

                SettingRow {
                    title: qsTr("搜索系统设置")
                    keywords: qsTr("设置;系统设置;控制面板;系统开关;蓝牙;音量;全部;关闭;settings").split(";")
                    description: qsTr("在“全部”里列出 Windows 设置页、控制面板和系统开关（如“关蓝牙”“音量 30”）；关掉后只找文件和内容")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.searchSystem
                        Accessible.name: qsTr("搜索系统设置")
                        onToggled: (on) => window.editor.searchSystem = on
                    }
                }

                SettingRow {
                    title: qsTr("正则模式（Ctrl+R）")
                    keywords: qsTr("正则;正则表达式;regex;Everything;Ctrl+R;匹配").split(";")
                    description: qsTr("每个词都按正则表达式匹配：“全部”和“文件”里匹配名字，“内容”里匹配文件里的文字")
                    tip: qsTr("如 ^IMG_\\d{4}；ext:、!排除、“ | ”照常可用。在搜索框里按 Ctrl+R 开关，开着时搜索框旁显示 .*。不开时也可在单个词前加 regex:")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.regexMode
                        Accessible.name: qsTr("正则模式")
                        onToggled: (on) => window.editor.regexMode = on
                    }
                }

                SettingRow {
                    title: qsTr("模糊匹配（FZF）")
                    keywords: qsTr("模糊;跳字;fzf;fuzzy;subsequence;缩写;匹配;关闭").split(";")
                    description: qsTr("按字符顺序跳字查找（如 nsrch 找 NameSearch.cpp、vcode 找 Visual Studio Code）；与现有搜索语法独立，可随时关闭")
                    tip: qsTr("无结果时：常规搜索完全不变，仅在无匹配时自动兜底（≥3 字符），零额外开销；始终开启：常规结果优先，模糊结果排后，搜索框旁显示 fz。关闭后仍可用 fz: 前缀按需触发（如 fz:rpt）")

                    ScopeTabs {
                        objectName: "fuzzyModeTabs"
                        readonly property var modes: ["off", "fallback", "on"]
                        labels: [qsTr("关闭"), qsTr("无结果时"), qsTr("始终开启")]
                        current: Math.max(0, modes.indexOf(window.editor.fuzzyMode))
                        onActivated: (index) => window.editor.fuzzyMode = modes[index]
                    }
                }
            }

            SettingsSection {
                page: window.resultsPage
                tab: 1
                width: parent.width
                title: qsTr("未输入时的初始列表")
                glyph: "\uE81C"

                SettingRow {
                    title: qsTr("合并“全部”和“文件”")
                    description: qsTr("在“全部”下常驻文件类型筛选，应用仍优先显示；按住 Ctrl 查看类型关键词")
                    keywords: qsTr("合并;全部;文件;类型;筛选;merge;scope").split(";")
                    ToggleSwitch {
                        objectName: "mergeFileScopeToggle"
                        dpr: window.dpr
                        checked: window.editor.mergeFileScope
                        onToggled: (on) => window.editor.mergeFileScope = on
                    }
                }

                SettingRow {
                    title: qsTr("显示“应用”标签")
                    description: qsTr("在范围标签的最后加一个“应用”：应用排成图标网格，可看常用的、收藏的或全部，点星标收藏")
                    keywords: qsTr("程序;应用;图标;网格;启动台;apps;grid;launchpad").split(";")
                    ToggleSwitch {
                        objectName: "appsTabToggle"
                        dpr: window.dpr
                        checked: window.editor.appsTab
                        onToggled: (on) => window.editor.appsTab = on
                    }
                }

                SettingRow {
                    title: qsTr("不输入时显示")
                    unlocks: [recentItemsRow]
                    keywords: qsTr("最近;最近打开;最近修改;最近使用;历史;recent").split(";")
                    description: qsTr("打开搜索框还没输入时列出最近打开或最近修改的项目；底栏也能切换（Ctrl+S）。收藏由下面的开关单独控制")

                    ScopeTabs {
                        readonly property var lists: ["opened", "modified", "none"]
                        labels: [qsTr("最近打开"), qsTr("最近修改"), qsTr("不显示")]
                        current: Math.max(0, lists.indexOf(window.editor.startList))
                        onActivated: (index) => window.editor.startList = lists[index]
                    }
                }

                SettingRow {
                    id: recentItemsRow
                    sub: true
                    shown: window.editor.startList === "opened"
                    title: qsTr("最近打开的数量")
                    keywords: qsTr("最近;历史;数量;条数;上限;自定义;recent").split(";")
                    description: qsTr("可填 1–200 项，按最近打开的顺序显示；仅在上面选择“最近打开”时生效")

                    InputBox {
                        dpr: window.dpr
                        anchors.verticalCenter: parent.verticalCenter
                        width: 80
                        enabled: window.editor.startList === "opened"
                        horizontalAlignment: TextInput.AlignHCenter
                        text: String(window.editor.recentItems)
                        Accessible.name: qsTr("最近打开的数量")
                        validator: IntValidator { bottom: 1; top: 200 }
                        onEditingFinished: {
                            const count = parseInt(text, 10)
                            if (count > 0)
                                window.editor.recentItems = Math.min(200, count)
                            text = Qt.binding(() => String(window.editor.recentItems))
                        }
                    }
                }

                SettingRow {
                    id: favoritesOnStartRow
                    title: qsTr("不输入时显示收藏")
                    keywords: qsTr("收藏;命令;显示;隐藏;关键词;空白;启动;favorite").split(";")
                    description: qsTr("关闭后，收藏和自定义命令仍可通过名称或关键词找到")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.favoritesOnStart
                        onToggled: (on) => window.editor.favoritesOnStart = on
                    }
                }

                SettingRow {
                    title: qsTr("收藏的样式")
                    keywords: qsTr("收藏;图标;平铺;网格;多行;排序;拖动;favorite;icon;grid").split(";")
                    description: window.editor.favoriteTiles
                                 ? qsTr("不输入时，收藏排成一格格图标，名称在下面，一屏放得更多；按住一个图标稍等再拖动，可以换位置")
                                 : qsTr("不输入时，收藏和最近打开的一样一行一个")

                    ScopeTabs {
                        objectName: "favoriteTilesChoice"
                        enabled: window.editor.favoritesOnStart
                        labels: [qsTr("列表"), qsTr("图标")]
                        current: window.editor.favoriteTiles ? 1 : 0
                        onActivated: (index) => window.editor.favoriteTiles = index === 1
                    }
                }
            }

            SettingsSection {
                page: window.resultsPage
                tab: 1
                width: parent.width
                title: qsTr("键盘提示")

                SettingRow {
                    title: qsTr("显示快捷键帮助按钮")
                    description: qsTr("在搜索框右下角显示入口，点击查看常用快捷键")
                    keywords: qsTr("快捷键;帮助;提示;隐藏;极简").split(";")

                    ToggleSwitch {
                        dpr: window.dpr
                        objectName: "shortcutHelpToggle"
                        checked: window.editor.shortcutHelp
                        Accessible.name: qsTr("显示快捷键帮助按钮")
                        onToggled: (on) => window.editor.shortcutHelp = on
                    }
                }

                SettingRow {
                    title: qsTr("显示底栏按键提示")
                    description: qsTr("显示 Enter、Ctrl+Enter、Tab 等操作提示；未输入时隐藏 Tab 切换范围提示")
                    keywords: qsTr("快捷键;底栏;提示;隐藏;Tab;极简").split(";")

                    ToggleSwitch {
                        dpr: window.dpr
                        objectName: "footerHintsToggle"
                        checked: window.editor.footerHints
                        Accessible.name: qsTr("显示底栏按键提示")
                        onToggled: (on) => window.editor.footerHints = on
                    }
                }
            }

            SettingsSection {
                page: window.resultsPage
                tab: 1
                width: parent.width
                title: qsTr("列表信息与交互")
                glyph: "\uE8FD"

                SettingRow {
                    title: qsTr("显示修改日期")
                    keywords: qsTr("修改时间;修改日期;日期;时间;date").split(";")
                    description: qsTr("在搜索结果右侧显示修改日期；可与文件大小同时显示")
                    unlocks: [showModifiedTimeRow]

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.showModified
                        onToggled: (on) => window.editor.showModified = on
                    }
                }

                SettingRow {
                    id: showModifiedTimeRow
                    sub: true
                    shown: window.editor.showModified
                    title: qsTr("较早的日期也显示具体时间")
                    keywords: qsTr("修改时间;修改日期;具体时间;时分;time").split(";")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.showModifiedTime
                        Accessible.name: qsTr("较早的日期也显示时间")
                        onToggled: (on) => window.editor.showModifiedTime = on
                    }
                }

                SettingRow {
                    title: qsTr("显示文件大小")
                    keywords: qsTr("文件大小;体积;精简;双击Ctrl;KB;MB;GB;size").split(";")
                    description: qsTr("无需展开窗口，在搜索结果右侧显示文件大小；按大小排序时始终显示。文件夹不统计大小。")

                    ToggleSwitch {
                        dpr: window.dpr
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
                        dpr: window.dpr
                        checked: window.editor.thumbnails
                        onToggled: (on) => window.editor.thumbnails = on
                    }
                }

                SettingRow {
                    title: qsTr("显示删除按钮")
                    keywords: qsTr("删除;垃圾桶;回收站;误删;按钮;delete;trash;recycle").split(";")
                    description: qsTr("文件和文件夹的结果行末尾显示删除按钮（点两次，移到回收站）；关闭后仍可在右键菜单中移到回收站")

                    ToggleSwitch {
                        objectName: "rowDeleteButtonToggle"
                        dpr: window.dpr
                        checked: window.editor.rowDeleteButton
                        onToggled: (on) => window.editor.rowDeleteButton = on
                    }
                }

                SettingRow {
                    title: qsTr("点击结果")
                    keywords: qsTr("单击;双击;点击;鼠标;误触;打开;选中;预览;看内容;click;double;preview").split(";")
                    description: !window.editor.doubleClickToOpen
                                 ? qsTr("单击就打开；也可以单击只选中，双击或按 Enter 才打开，结果多时不怕点错")
                                 : window.editor.clickPreviews
                                   ? qsTr("单击选中并预览，不用打开就能看内容；双击或按 Enter 打开")
                                   : qsTr("单击只选中（高亮），双击或按 Enter 才打开，结果多时不怕点错")

                    ScopeTabs {
                        objectName: "clickActionChoice"
                        labels: [qsTr("单击打开"), qsTr("双击打开"), qsTr("单击预览")]
                        current: !window.editor.doubleClickToOpen ? 0 : window.editor.clickPreviews ? 2 : 1
                        onActivated: (index) => {
                            window.editor.clickPreviews = index === 2
                            window.editor.doubleClickToOpen = index !== 0
                        }
                    }
                }

                SettingRow {
                    objectName: "defaultSystemMenuRow"
                    title: qsTr("默认使用系统右键菜单")
                    keywords: qsTr("右键;系统菜单;右键菜单;比较文件;更多选项;Shift;context menu;shell").split(";")
                    description: window.editor.defaultSystemMenu
                                 ? qsTr("在文件或文件夹上右键直接打开系统菜单，Shift+右键 打开 Win顺 菜单；应用、插件等仍打开 Win顺 菜单")
                                 : qsTr("在文件或文件夹上右键打开 Win顺 菜单，Shift+右键 打开系统菜单；开启后互换")

                    ToggleSwitch {
                        objectName: "defaultSystemMenuToggle"
                        dpr: window.dpr
                        checked: window.editor.defaultSystemMenu
                        onToggled: (on) => window.editor.defaultSystemMenu = on
                    }
                }

                SettingRow {
                    id: favoriteFolderTabRow
                    objectName: "favoriteFolderTab"
                    title: qsTr("选中收藏文件夹时按 Tab")
                    keywords: qsTr("Tab;Shift+Tab;收藏;文件夹;范围;键盘;favorite;keyboard").split(";")
                    description: qsTr("在“全部”范围中，列表自动选中的收藏文件夹（如第一行）默认切换搜索范围，也可改为进入它继续搜索。用 ↑ ↓ 或单击选中的文件夹，按 Tab 都会进入。Shift+Tab 仍反向切换范围。")

                    ScopeTabs {
                        objectName: "favoriteFolderTabChoice"
                        labels: [qsTr("切换范围"), qsTr("进入文件夹")]
                        current: window.editor.tabEntersFavoriteFolder ? 1 : 0
                        onActivated: (index) => window.editor.tabEntersFavoriteFolder = index === 1
                    }
                }
            }

            SettingsSection {
                page: window.resultsPage
                tab: 1
                width: parent.width
                title: qsTr("文件快速预览")
                glyph: "\uE890"

                SettingRow {
                    title: qsTr("预览文件")
                    keywords: qsTr("预览;快速查看;看一眼;QuickLook;Seer;空格;Alt+P;preview").split(";")
                    description: window.editor.previewWith === "builtin"
                                 ? qsTr("用 Win顺 自己的预览窗口看一眼")
                                 : qsTr("QuickLook 或 Seer 在后台运行时交给它，否则用 Win顺 自己的预览")

                    ScopeTabs {
                        labels: [qsTr("自动"), qsTr("内置")]
                        current: window.editor.previewWith === "builtin" ? 1 : 0
                        onActivated: (index) => window.editor.previewWith = ["auto", "builtin"][index]
                    }
                }

                SettingRow {
                    title: qsTr("预览跟随")
                    keywords: qsTr("预览;跟随;鼠标;悬停;指向;划过;选中;大文件;卡;hover;preview").split(";")
                    description: window.editor.previewFollowsPointer
                                 ? qsTr("预览开着时，鼠标停在哪一行就预览哪一行；Word、PPT、PDF 这类文件要停稳一下才打开")
                                 : window.editor.doubleClickToOpen
                                   ? qsTr("只预览选中的行：单击或用方向键选中。鼠标划过时不切换，大文件不会被一个个打开")
                                   : qsTr("只预览选中的行：用方向键选中，或用鼠标中键点一行。鼠标划过时不切换，大文件不会被一个个打开")

                    ScopeTabs {
                        objectName: "previewFollowChoice"
                        labels: [qsTr("鼠标所指"), qsTr("选中的行")]
                        current: window.editor.previewFollowsPointer ? 0 : 1
                        onActivated: (index) => window.editor.previewFollowsPointer = index === 0
                    }
                }

                SettingRow { // whether a preview left open comes back with the launcher (GitHub #258: some want it gone)
                    title: qsTr("再次打开时恢复预览")
                    keywords: qsTr("预览;恢复;记住;保留;关闭;下次;再次;restore;preview").split(";")
                    description: window.editor.previewRestore
                                 ? qsTr("预览开着时收起搜索框，下次打开搜索框预览也跟着回来")
                                 : qsTr("搜索框收起，预览就一起关掉；下次要看再按预览快捷键")

                    ToggleSwitch {
                        objectName: "previewRestoreSwitch"
                        checked: window.editor.previewRestore
                        onToggled: (on) => window.editor.previewRestore = on
                    }
                }

                SettingRow {
                    id: previewHotkeyRow
                    title: qsTr("预览快捷键")
                    keywords: qsTr("预览;快捷键;空格;Space;Alt+P;QuickLook;preview;shortcut").split(";")
                    values: [window.editor.previewHotkey]
                    description: qsTr("仅在搜索结果中生效，再按一次关闭，鼠标中键点一行也行。可直接录制空格：刚输入完时空格照常输入，选过结果或打开预览后才开关预览；Shift+空格始终输入空格。Backspace 清除，Esc 取消。")

                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("恢复默认")
                        enabled: window.editor.previewHotkey !== "Alt+P"
                        onClicked: window.editor.previewHotkey = "Alt+P"
                    }
                    HotkeyRecorder {
                        id: previewHotkeyRecorder
                        objectName: "previewHotkeyRecorder"
                        hotkey: window.editor.previewHotkey
                        previewShortcut: true
                        onRecorded: (hotkey) => window.editor.previewHotkey = hotkey
                    }

                    bodyShown: window.editor.previewHotkeyError.length > 0
                    body: Text {
                        width: parent.width
                        text: window.editor.previewHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
            }

            SettingsSection {
                page: window.resultsPage
                tab: 2
                width: parent.width
                title: qsTr("搜索框与数字快捷键")
                glyph: "\uE765"
                note: qsTr("在搜索框按 Tab、Shift+Tab 切换“全部”“文件”“内容”；Ctrl+Tab 在“文件”里切换文件类型，在“内容”里切换文件夹。")

                SettingRow {
                    objectName: "searchLogoRow"
                    title: qsTr("搜索框显示 Logo")
                    description: qsTr("关掉后搜索框更简洁：拖顶部的小横条移动窗口，按 Ctrl+, 或点托盘图标打开设置，钉住时搜索框右边显示图钉")
                    keywords: qsTr("Logo;图标;简洁;隐藏;搜索框;logo;icon;hide;minimal").split(";")

                    ToggleSwitch {
                        objectName: "searchLogoSwitch"
                        dpr: window.dpr
                        checked: window.editor.searchLogo
                        onToggled: (on) => window.editor.searchLogo = on
                    }
                }

                SettingRow {
                    objectName: "logoRightClickRow"
                    visible: window.editor.searchLogo // nothing to click without it
                    title: qsTr("右键 Logo 的行为")
                    description: qsTr("“靠上”模式下，展开的搜索面板会居中")
                    keywords: qsTr("右键;Logo;位置;归位;靠上;居中;拖动;logo;position").split(";")

                    ScopeTabs {
                        objectName: "logoRightClickChoice"
                        readonly property var choices: ["menu", "top", "center", "restore"]
                        labels: [qsTr("位置菜单"), qsTr("靠上"), qsTr("居中"), qsTr("上次拖动位置")]
                        current: Math.max(0, choices.indexOf(window.editor.logoRightClick))
                        onActivated: (index) => window.editor.logoRightClick = choices[index]
                    }
                }

                SettingRow {
                    title: qsTr("每次打开时清空搜索")
                    keywords: qsTr("清空;清除;上次;保留;隐私;搜索框;clear;privacy").split(";")
                    description: qsTr("打开时搜索框是空的，回到“全部”；关掉时保留上次的文字并选中，直接输入就会替换")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clearOnOpen
                        onToggled: (on) => window.editor.clearOnOpen = on
                    }
                }

                SettingRow {
                    title: qsTr("回到上次的位置")
                    keywords: qsTr("上次;位置;滚动;记住;选中;恢复;scroll;position;remember").split(";")
                    description: qsTr("再打开时停在上次选中的那一行，列表也滚到那里；关掉时从第一行开始。开着“每次打开时清空搜索”时不起作用")

                    ToggleSwitch {
                        objectName: "reopenWhereLeftSwitch"
                        dpr: window.dpr
                        enabled: !window.editor.clearOnOpen
                        checked: window.editor.reopenWhereLeft
                        onToggled: (on) => window.editor.reopenWhereLeft = on
                    }
                }

                SettingRow { // feedback 2026-11-16
                    title: qsTr("钉住时在最前")
                    keywords: qsTr("钉住;置顶;最前;盖住;遮挡;Ctrl+T;pin;topmost;always on top").split(";")
                    description: qsTr("按 Ctrl+T 钉住后，搜索框一直盖在别的窗口上面；关掉时点别的窗口会把它盖住，按打开 Win顺 的快捷键再回到前面")

                    ToggleSwitch {
                        objectName: "pinnedOnTopSwitch"
                        dpr: window.dpr
                        checked: window.editor.pinnedOnTop
                        onToggled: (on) => window.editor.pinnedOnTop = on
                    }
                }

                SettingRow {
                    title: qsTr("打开时切换为英文输入")
                    keywords: qsTr("输入法;英文;字母;中文;拼音首字母;IME;English").split(";")
                    description: qsTr("中文输入法在搜索框里先用英文，直接打字母和拼音首字母；在框里切回中文照常可用，关掉后恢复原样")

                    ToggleSwitch {
                        checked: window.editor.englishOnOpen
                        onToggled: (on) => window.editor.englishOnOpen = on
                    }
                }

                SettingRow {
                    title: qsTr("打开时填入刚复制的文字")
                    keywords: qsTr("剪贴板;复制;自动;填入;粘贴;网址;clipboard;paste").split(";")
                    description: qsTr("刚在别的程序里复制了一行文字，在这么多秒内打开搜索框，就直接搜它（全选着，打字即替换）；超过时间、复制的是多行或文件时不填。装了“网页搜索”插件，复制的是网址时第一行就是打开它")

                    ScopeTabs {
                        labels: [qsTr("关闭"), qsTr("3 秒"), qsTr("5 秒"), qsTr("10 秒")]
                        current: [0, 3, 5, 10].indexOf(window.editor.clipboardFillSeconds) // another number of seconds (set in the file): none of them
                        onActivated: (index) => window.editor.clipboardFillSeconds = [0, 3, 5, 10][index]
                    }
                }

                SettingRow {
                    id: openHotkeyRow
                    title: qsTr("打开结果的备用键")
                    keywords: qsTr("打开;回车;Enter;备用;左手;快捷键;空格;open;shortcut").split(";")
                    values: [window.editor.openHotkey]
                    description: qsTr("和 Enter 一样打开选中的结果，Enter 照常可用。录一个左手顺手的键，比如 Alt+Q 或 F1；空格不行，它要用来分开几个词。Backspace 清除，Esc 取消。")

                    HotkeyRecorder {
                        id: openHotkeyRecorder
                        objectName: "openHotkeyRecorder"
                        hotkey: window.editor.openHotkey
                        previewShortcut: true
                        placeholder: qsTr("未设置")
                        onRecorded: (hotkey) => window.editor.openHotkey = hotkey
                    }

                    bodyShown: window.editor.openHotkeyError.length > 0
                    body: Text {
                        width: parent.width
                        text: window.editor.openHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    // What a folder's or file's 在终端中打开 starts (GitHub #386):
                    // only what is found here, then a command of the user's own.
                    id: terminalRow
                    readonly property bool custom: window.editor.terminal === "custom"
                    title: qsTr("在终端中打开")
                    keywords: qsTr("终端;命令行;命令提示符;PowerShell;pwsh;Windows Terminal;Git Bash;WSL;MSYS2;bash;shell;terminal").split(";")
                    description: qsTr("结果上的“在终端中打开”用哪个。选“Windows 终端”就用它里面设的默认配置")
                    bodyShown: custom

                    ChoiceBox {
                        id: terminalPicker
                        objectName: "terminalPicker"
                        implicitWidth: 240
                        implicitHeight: 36
                        model: window.editor.terminals
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: window.editor.terminals.findIndex(t => t.value === window.editor.terminal)
                        onActivated: window.editor.terminal = currentValue
                        Accessible.name: qsTr("在终端中打开")
                        dpr: window.dpr
                        leftPadding: 12
                        contentItem: Text {
                            text: terminalPicker.displayText
                            font: terminalPicker.font
                            color: terminalPicker.palette.buttonText
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        Connections {
                            target: terminalPicker.popup
                            function onOpened() { window.frame.addPopup(terminalPicker.popup.background) }
                        }
                    }

                    body: [
                        InputBox {
                            dpr: window.dpr
                            objectName: "terminalCommandBox"
                            width: Math.min(parent.width, 420)
                            text: window.editor.terminalCommand
                            placeholder: qsTr("例如 %1").arg("C:\\msys64\\msys2_shell.cmd -ucrt64 -here")
                            onEditingFinished: window.editor.terminalCommand = text
                        },
                        Text {
                            width: parent.width
                            text: qsTr("在那个文件夹里运行；要把文件夹路径当参数时写 {query}。没填命令时开命令提示符")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    ]
                }

                SettingRow {
                    id: resultNumberKeysRow
                    title: qsTr("用数字键直接打开")
                    unlocks: [resultNumberHintsRow]
                    keywords: qsTr("快捷键;数字键;序号;Ctrl+1;Alt+1;冲突;hotkey").split(";")
                    description: qsTr("按 %1+1 打开当前可见的第 1 行，依此类推到第 9 行（随列表滚动更新）")
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
                    id: letterJumpRow
                    title: qsTr("字母直达")
                    unlocks: [letterJumpKeyRow, letterJumpKeysRow, resultNumberHintsRow]
                    description: qsTr("轻按一下下面选的键再松开，每一行前面出现一个字母，按字母直接打开这一行（剪贴板里是粘贴），第九行以后也有。搜索框里按 Shift+字母是选中这一行，它的按钮（打开所在位置、在终端中打开、复制、复制路径、删除）上再出现字母，按字母就是点那个按钮，删除要按两次；剪贴板和对话框旁的搜索栏里 Shift+字母是选中那一条，剪贴板里可以选几条一起粘贴。再按一下那个键或按 Esc 退出。启动器和剪贴板共用")
                    keywords: qsTr("快捷键;字母;单手;vim;vi;提示;跳转;直达;hint").split(";")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.letterJump
                        onToggled: (on) => window.editor.letterJump = on
                    }
                }

                SettingRow {
                    id: letterJumpKeyRow
                    sub: true
                    shown: window.editor.letterJump
                    title: qsTr("字母直达用的键")
                    description: window.editor.letterJumpKey === "shift"
                                 ? qsTr("搜索框里用中文输入法时，轻按 Shift 也会切换中英文")
                                 : qsTr("跟数字键一样：搜索框和剪贴板各用数字键设置里的那个键；关闭数字键时默认用 Ctrl")
                    keywords: qsTr("字母直达;修饰键;Ctrl;Alt;Shift").split(";")

                    ScopeTabs {
                        labels: [qsTr("跟数字键一样"), "Ctrl", "Alt", "Shift"]
                        current: Math.max(0, ["auto", "ctrl", "alt", "shift"].indexOf(window.editor.letterJumpKey))
                        onActivated: (index) => window.editor.letterJumpKey = ["auto", "ctrl", "alt", "shift"][index]
                    }
                }

                SettingRow {
                    id: letterJumpKeysRow
                    sub: true
                    shown: window.editor.letterJump
                    title: qsTr("用哪些字母")
                    description: qsTr("按顺序给第一行、第二行……，可以有数字，例如左手用的 QWERTASDFGZXCVB12345")
                    keywords: qsTr("字母直达;字母;数字;顺序;左手;单手").split(";")

                    Row {
                        spacing: 8

                        InputBox {
                            id: letterJumpKeysBox
                            dpr: window.dpr
                            width: 230
                            text: window.editor.letterJumpKeys
                            placeholder: window.editor.defaultLetterJumpKeys
                            // Taken as cleaned up (upper case, A-Z and 0-9 once each): the box shows that.
                            // Anything else in it (an input method's 汉字 for the letters typed) is not
                            // taken: cleaned up, "阿斯顿a" would be the one letter A.
                            function take() {
                                error = !/^[A-Za-z0-9\s,;，；]*$/.test(text)
                                if (error)
                                    return
                                window.editor.letterJumpKeys = text
                                text = Qt.binding(() => window.editor.letterJumpKeys)
                                showStart()
                            }
                            onEditingFinished: take()
                            onAccepted: take()
                            onTextChanged: if (error && /^[A-Za-z0-9\s,;，；]*$/.test(text)) error = false
                        }
                        FlatButton {
                            dpr: window.dpr
                            visible: window.editor.letterJumpKeys !== window.editor.defaultLetterJumpKeys
                            text: qsTr("恢复默认")
                            onClicked: {
                                window.editor.letterJumpKeys = window.editor.defaultLetterJumpKeys
                                letterJumpKeysBox.error = false
                                letterJumpKeysBox.text = Qt.binding(() => window.editor.letterJumpKeys)
                                letterJumpKeysBox.showStart()
                            }
                        }
                    }
                }

                SettingRow {
                    id: resultNumberHintsRow
                    objectName: "resultNumberHintsRow"
                    shown: window.editor.numberKeys !== "off" || window.editor.letterJump
                    title: qsTr("序号提示")
                    description: window.editor.numberKeys === "off"
                                 ? qsTr("启动器、剪贴板和对话框搜索栏共用；关闭数字键时提前预览各行字母，轻按直达键后高亮触发")
                                 : qsTr("启动器、剪贴板和对话框搜索栏共用；隐藏提示不影响数字快捷键，关闭数字键时也用于预览字母直达")
                    keywords: qsTr("快捷键;数字键;字母;字母直达;图标;按住;显示;隐藏").split(";")

                    ScopeTabs {
                        labels: [qsTr("按住时显示"), qsTr("一直显示"), qsTr("不显示", "number hints")]
                        current: Math.max(0, ["held", "always", "off"].indexOf(window.editor.numberHints))
                        onActivated: (index) => window.editor.numberHints = ["held", "always", "off"][index]
                    }
                }
            }

            SettingsSection {
                page: window.resultsPage
                tab: 2
                width: parent.width
                title: qsTr("使用记录")
                glyph: "\uE81C"

                SettingRow {
                    title: qsTr("记住搜索和打开过的项目")
                    keywords: qsTr("历史;最近;记录;history;recent").split(";")
                    description: qsTr("记住打开过的项目和最近 100 条搜索词；同样的字常打开的项目排在前面")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.recordHistory
                        onToggled: (on) => window.editor.recordHistory = on
                    }
                }

                SettingRow {
                    title: qsTr("记住以管理员身份运行")
                    keywords: qsTr("管理员;提权;权限;admin;administrator;elevate").split(";")
                    description: qsTr("用 Ctrl+Shift+Enter 以管理员身份运行过的程序，之后按 Enter 也以管理员身份运行；右键选“以普通权限运行”可改回")

                    ToggleSwitch {
                        dpr: window.dpr
                        enabled: window.editor.recordHistory
                        checked: window.editor.rememberAdmin
                        onToggled: (on) => window.editor.rememberAdmin = on
                    }
                }

                SettingRow {
                    id: queryHistoryKeysRow
                    title: qsTr("搜索历史快捷键")
                    description: qsTr("在搜索框中翻阅；回到最新位置时恢复未完成的输入，也可点时钟图标浏览和删除")
                    keywords: qsTr("历史;上一个;下一个;history;Ctrl;Alt").split(";")
                    ScopeTabs {
                        objectName: "queryHistoryKeys"
                        enabled: window.editor.recordHistory
                        readonly property var choices: ["ctrl", "alt", "off"]
                        labels: ["Ctrl+↑/↓", "Alt+↑/↓", qsTr("关闭")]
                        current: choices.indexOf(window.editor.queryHistoryKeys)
                        onActivated: (index) => window.editor.queryHistoryKeys = choices[index]
                    }
                }

                SettingRow {
                    title: qsTr("清除搜索和最近使用记录")
                    keywords: qsTr("历史;最近;删除;清空;history").split(";")
                    description: window.editor.historyCount > 0
                                 ? qsTr("共 %1 项；搜索词可在搜索历史中单独删除，打开记录可右键移除").arg(window.editor.historyCount)
                                 : qsTr("没有记录")

                    FlatButton {
                        dpr: window.dpr
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

            FileTypesSection {
                id: fileTypesSection
                page: window.resultsPage
                tab: 3
                width: parent.width
                editor: window.editor
            }

            QuickFiltersSection {
                id: quickFiltersSection
                page: window.resultsPage
                tab: 3
                width: parent.width
                editor: window.editor
            }
            // </page results>
                } }
            }

            PageSlot {
                id: integrationSlot
                index: window.integrationPage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({
                        taskbarWinSRow: taskbarWinSRow,
                        explorerTypingRow: explorerTypingRow,
                        opusTypingRow: opusTypingRow,
                        oneCommanderTypingRow: oneCommanderTypingRow,
                        folderMenuRow: folderMenuRow,
                        folderMenuGroupsRow: folderMenuGroupsRow,
                        folderMenuFavoriteKindsRow: folderMenuFavoriteKindsRow,
                        folderMenuHotkeyRow: folderMenuHotkeyRow,
                        dialogBarRow: dialogBarRow,
                        trayLeftActionRow: trayLeftActionRow,
                        trayMiddleActionRow: trayMiddleActionRow
                    })
                    recordingHotkey: folderMenuHotkeyRecorder.recording
                    onResetForReopen: {
                        folderMenuHotkeyRecorder.stop()
                        taskbarButtonRow.chosen = -1
                        opusMenuRow.confirming = false
                        opusMenuRow.copied = false
                    }
            // <page integration>
            SettingsSection {
                page: window.integrationPage
                tab: 0
                width: parent.width
                title: qsTr("任务栏入口")
                glyph: "\uE75B"
                note: qsTr("任务栏搜索框可单独使用，也可与 Windows 搜索并存。")

                SettingRow {
                    id: taskbarButtonRow
                    unlocks: [taskbarStyleRow]

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
                            if (window.editor.searchBoxForm === "box")
                                return window.editor.taskbarSearchBoxSide === "tray"
                                    ? qsTr("已放到托盘图标左边，点它直接打字")
                                    : qsTr("已放到任务栏上，点它直接打字")
                            return window.editor.taskbarSearchBoxSide === "tray"
                                ? qsTr("已放到托盘图标左边，点它打开搜索")
                                : qsTr("已放到任务栏上，点它打开搜索")
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
                        dpr: window.dpr
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
                    id: taskbarStyleRow
                    sub: true
                    // Under 搜索框 chosen above: how wide it may be, as
                    // Windows' own search has it (Settings › Taskbar ›
                    // Search). Narrower ones still where that does not fit.
                    readonly property var styles: ["box", "label", "icon"]
                    readonly property int chosenRank: Math.max(0, styles.indexOf(window.editor.taskbarSearchBoxStyle))
                    readonly property int actualRank: styles.indexOf(window.editor.searchBoxForm)
                    readonly property bool narrowed: window.editor.searchBoxShown && actualRank > chosenRank
                    shown: taskbarButtonRow.boxes && taskbarButtonRow.boxOn
                    title: qsTr("搜索框样式")
                    keywords: qsTr("样式;仅图标;只显示图标;图标和标签;标签;放大镜;按钮;搜索框;icon;label").split(";")
                    description: {
                        if (narrowed) {
                            const shownName = window.editor.searchBoxForm === "icon" ? qsTr("仅图标") : qsTr("图标和标签")
                            return window.editor.taskbarSearchBoxSide === "tray"
                                ? qsTr("当前托盘旁空隙不够，已自动收窄为“%1”；可改选“自动”位置或减少任务栏图标").arg(shownName)
                                : qsTr("当前任务栏空隙不够，已自动收窄为“%1”；减少几个任务栏图标后会自动展开").arg(shownName)
                        }
                        return window.editor.taskbarSearchBoxStyle === "icon"
                               ? qsTr("一个搜索图标，像任务栏上的按钮")
                               : window.editor.taskbarSearchBoxStyle === "label"
                                 ? qsTr("图标加“搜索”两个字，点它打开搜索")
                                 : qsTr("点它直接打字；任务栏挤时自动变窄")
                    }

                    ScopeTabs {
                        dpr: window.dpr
                        labels: [qsTr("搜索框"), qsTr("图标和标签"), qsTr("仅图标")]
                        current: taskbarStyleRow.chosenRank
                        onActivated: (index) => window.editor.taskbarSearchBoxStyle = taskbarStyleRow.styles[index]
                    }
                }

                SettingRow {
                    id: taskbarSideRow
                    sub: true
                    // Which end of the taskbar (GitHub #271): Windows lays
                    // its icons out centred or at the left, and 自动 keeps
                    // out of their way; 托盘旁 for a pointer that lives at
                    // that end, with centred icons too.
                    shown: taskbarStyleRow.shown
                    title: qsTr("搜索框位置")
                    keywords: qsTr("位置;停靠;左侧;最左;托盘;右侧;居中;任务栏对齐;position;tray").split(";")
                    description: window.editor.taskbarSearchBoxSide === "tray"
                                 ? qsTr("始终在托盘图标左边，任务栏图标居中时也是")
                                 : qsTr("任务栏图标居中时在任务栏最左边，靠左时在托盘图标左边")

                    ScopeTabs {
                        dpr: window.dpr
                        readonly property var sides: ["auto", "tray"]
                        labels: [qsTr("自动"), qsTr("托盘旁")]
                        current: Math.max(0, sides.indexOf(window.editor.taskbarSearchBoxSide))
                        onActivated: (index) => window.editor.taskbarSearchBoxSide = sides[index]
                    }
                }

                SettingRow {
                    title: qsTr("隐藏任务栏上的 Windows 搜索")
                    keywords: qsTr("隐藏;任务栏;搜索框;taskbar").split(";")
                    description: window.editor.windowsSearchShown
                                 ? qsTr("可在任务栏设置里把“搜索”选成“隐藏”")
                                 : qsTr("已隐藏；可在任务栏设置中恢复")

                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("打开任务栏设置")
                        onClicked: window.editor.openUrl("ms-settings:taskbar")
                    }
                }
            }

            SettingsSection {
                page: window.integrationPage
                tab: 0
                width: parent.width
                title: qsTr("系统搜索接管")
                glyph: "\uE721"

                SettingRow {
                    id: taskbarWinSRow
                    title: qsTr("用 Win+S 打开 Win顺")
                    keywords: qsTr("Win+S;Windows 搜索;截图;Win+Shift+S").split(";")
                    description: qsTr("切换时资源管理器会重启，打开的文件夹窗口会关闭")

                    ToggleSwitch {
                        dpr: window.dpr
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
                            dpr: window.dpr
                            visible: window.editor.winSState === "blocked"
                                     || (window.editor.winSState === "failed" && !window.editor.taskbarWinS)
                            text: qsTr("还给 Windows")
                            onClicked: window.editor.giveBackToWindows("S")
                        }
                        FlatButton {
                            dpr: window.dpr
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
                        dpr: window.dpr
                        checked: window.editor.taskbarStartTyping
                        onToggled: (on) => window.editor.taskbarStartTyping = on
                    }
                }
            }

            SettingsSection {
                page: window.integrationPage
                tab: 0
                width: parent.width
                title: qsTr("托盘图标")
                glyph: "ph:cursor-click"
                note: qsTr("右键托盘图标始终打开托盘菜单；可为左键和中键分别指定快捷动作。")

                SettingRow {
                    id: trayLeftActionRow
                    title: qsTr("左键单击托盘图标")
                    keywords: qsTr("托盘;通知区域;左键;单击;点击;图标;tray;left click").split(";")
                    description: qsTr("默认打开搜索框；也可设为完整搜索面板、剪贴板、设置或已启用插件的动作")

                    ChoiceBox {
                        id: trayLeftActionPicker
                        objectName: "trayLeftActionPicker"
                        implicitWidth: 260
                        implicitHeight: 36
                        model: window.editor.trayLeftActions
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: window.editor.trayLeftActions.findIndex(action => action.value === window.editor.trayLeftAction)
                        onActivated: window.editor.trayLeftAction = currentValue
                        Accessible.name: qsTr("左键单击托盘图标")
                        dpr: window.dpr
                        leftPadding: 12
                        contentItem: Text {
                            text: trayLeftActionPicker.displayText
                            font: trayLeftActionPicker.font
                            color: trayLeftActionPicker.palette.buttonText
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        Connections {
                            target: trayLeftActionPicker.popup
                            function onOpened() { window.frame.addPopup(trayLeftActionPicker.popup.background) }
                        }
                    }
                }

                SettingRow {
                    id: trayMiddleActionRow
                    title: qsTr("中键单击托盘图标")
                    keywords: qsTr("托盘;通知区域;中键;滚轮;单击;点击;设置;截图历史;tray;middle click").split(";")
                    description: qsTr("默认直接打开设置；安装截图插件后也可选打开截图历史或触发插件动作")

                    ChoiceBox {
                        id: trayMiddleActionPicker
                        objectName: "trayMiddleActionPicker"
                        implicitWidth: 260
                        implicitHeight: 36
                        model: window.editor.trayMiddleActions
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: window.editor.trayMiddleActions.findIndex(action => action.value === window.editor.trayMiddleAction)
                        onActivated: window.editor.trayMiddleAction = currentValue
                        Accessible.name: qsTr("中键单击托盘图标")
                        dpr: window.dpr
                        leftPadding: 12
                        contentItem: Text {
                            text: trayMiddleActionPicker.displayText
                            font: trayMiddleActionPicker.font
                            color: trayMiddleActionPicker.palette.buttonText
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        Connections {
                            target: trayMiddleActionPicker.popup
                            function onOpened() { window.frame.addPopup(trayMiddleActionPicker.popup.background) }
                        }
                    }
                }
            }

            SettingsSection {
                page: window.integrationPage
                tab: 1
                width: parent.width
                title: qsTr("文件管理器整合")
                glyph: "\uEC50"

                SettingRow {
                    shown: window.editor.fileManagers.length > 1
                    title: qsTr("用哪个文件管理器打开文件夹")
                    keywords: qsTr("文件管理器;资源管理器;打开文件夹;打开所在位置;Total Commander;Directory Opus;TC;Opus;XYplorer;Pulse;OneCommander;自定义;其他;Tessoa;custom").split(";")
                    description: qsTr("打开文件夹、打开文件所在位置时用它；没装的不列")
                    unlocks: [customManagerRow]

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
                    sub: true
                    shown: window.editor.fileManager === "custom"
                    title: qsTr("其他文件管理器的命令")
                    keywords: qsTr("文件管理器;自定义;命令;命令行;其他;Tessoa;custom;command").split(";")
                    description: qsTr("命令里的 {path} 会替换为完整路径（自动加引号）")
                    tip: qsTr("自定义文件管理器不支持读取当前正在显示的文件夹：对话框 Ctrl+G 与搜索框建议仅支持资源管理器、Total Commander、Directory Opus、XYplorer、Pulse 和 OneCommander")
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
                                dpr: window.dpr
                                id: openCommandBox
                                width: parent.width - browseOpen.width - parent.spacing
                                text: window.editor.fileManagerOpenCommand
                                // The path outside qsTr(): lupdate reads backslashes in QML strings wrongly.
                                placeholder: qsTr("例如 %1").arg("\"C:\\Program Files\\Tessoa\\tessoa.exe\" {path}")
                                onEditingFinished: window.editor.fileManagerOpenCommand = text
                            }
                            FlatButton {
                                dpr: window.dpr
                                id: browseOpen
                                text: qsTr("浏览…")
                                onClicked: {
                                    const command = window.editor.pickFileManagerProgram()
                                    if (command.length > 0) {
                                        window.editor.fileManagerOpenCommand = command
                                        openCommandBox.text = Qt.binding(() => window.editor.fileManagerOpenCommand) // the setting's again, not a copy
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
                                dpr: window.dpr
                                id: revealCommandBox
                                width: parent.width - browseReveal.width - parent.spacing
                                text: window.editor.fileManagerRevealCommand
                                placeholder: qsTr("可不填：就打开文件所在的文件夹")
                                onEditingFinished: window.editor.fileManagerRevealCommand = text
                            }
                            FlatButton {
                                dpr: window.dpr
                                id: browseReveal
                                text: qsTr("浏览…")
                                onClicked: {
                                    const command = window.editor.pickFileManagerProgram()
                                    if (command.length > 0) {
                                        window.editor.fileManagerRevealCommand = command
                                        revealCommandBox.text = Qt.binding(() => window.editor.fileManagerRevealCommand)
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
                        dpr: window.dpr
                        checked: window.editor.explorerMenu
                        onToggled: (on) => window.editor.explorerMenu = on
                    }
                }

                SettingRow {
                    title: qsTr("资源管理器右键“贴到屏幕（Win顺）”")
                    keywords: qsTr("贴到屏幕;贴图;图片;右键;右键菜单;上下文菜单;资源管理器;截图;pin;picture;context menu").split(";")
                    description: window.editor.screenshotHistory
                        ? qsTr("右键图片文件（PNG、JPG、WebP 等）直接贴到屏幕上置顶；Windows 11 在“显示更多选项”里")
                        : qsTr("右键图片文件直接贴到屏幕上置顶；需先在“插件”里启用“截图”插件才会显示")

                    ToggleSwitch {
                        objectName: "explorerPinPictureSwitch"
                        dpr: window.dpr
                        checked: window.editor.explorerPinPicture
                        onToggled: (on) => window.editor.explorerPinPicture = on
                    }
                }

                SettingRow { // GitHub #358
                    title: qsTr("winshun:// 链接")
                    keywords: qsTr("链接;协议;网址;URL;网页;笔记;Obsidian;scheme;protocol;winshun").split(";")
                    description: qsTr("网页、笔记软件里的 winshun://search/关键词 链接点一下就用 Win顺 搜索")
                    tip: qsTr("winshun://search/合同 打开搜索框并填好“合同”；winshun://settings 打开设置。链接只会打字或打开设置，不会打开或运行文件")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.uriProtocol
                        onToggled: (on) => window.editor.uriProtocol = on
                    }
                }

                SettingRow {
                    id: explorerTypingRow
                    title: qsTr("在资源管理器里直接打字搜索")
                    keywords: qsTr("打字;输入;直接搜索;当前文件夹;资源管理器;Listary;type to search;explorer").split(";")
                    description: qsTr("文件列表有焦点时直接打字即可用 Win顺 搜索")
                    tip: qsTr("和 Listary 一样，代替资源管理器自带的“按首字母跳到文件”")
                    unlocks: [folderTypingRow]

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.explorerTyping
                        onToggled: (on) => window.editor.explorerTyping = on
                    }
                }

                SettingRow { // GitHub #314
                    id: desktopTypingRow
                    title: qsTr("在桌面上直接打字搜索")
                    keywords: qsTr("桌面;打字;输入;直接搜索;图标;desktop;type to search").split(";")
                    description: qsTr("点一下桌面后直接打字，即可用 Win顺 搜索整台电脑")
                    tip: qsTr("代替桌面自带的“按首字母跳到图标”；重命名图标时不受影响")

                    ToggleSwitch {
                        dpr: window.dpr
                        objectName: "desktopTypingToggle"
                        checked: window.editor.desktopTyping
                        onToggled: (on) => window.editor.desktopTyping = on
                    }
                }

                SettingRow {
                    id: opusTypingRow
                    title: qsTr("在 Directory Opus 里直接打字搜索")
                    keywords: qsTr("Opus;DOpus;直接打字;输入;当前文件夹;双栏;type to search").split(";")
                    description: qsTr("文件列表有焦点时，输入字母或数字即可搜索当前文件夹")
                    tip: qsTr("符号键保留给 Opus，重命名和地址栏输入不受影响")
                    unlocks: [opusScopeRow, folderTypingRow]

                    ToggleSwitch {
                        dpr: window.dpr
                        objectName: "opusTypingToggle"
                        checked: window.editor.opusTyping
                        Accessible.name: qsTr("在 Directory Opus 里直接打字搜索")
                        onToggled: (on) => window.editor.opusTyping = on
                    }
                }

                SettingRow {
                    id: oneCommanderTypingRow
                    title: qsTr("在 OneCommander 里直接打字搜索")
                    keywords: qsTr("OneCommander;One Commander;直接打字;输入;当前文件夹;双栏;type to search").split(";")
                    description: qsTr("文件列表有焦点时，输入字母或数字即可搜索当前文件夹")
                    tip: qsTr("代替 OneCommander 自带的“按首字母筛选”；符号键、侧栏、重命名和地址栏输入不受影响")
                    unlocks: [folderTypingRow]

                    ToggleSwitch {
                        dpr: window.dpr
                        objectName: "oneCommanderTypingToggle"
                        checked: window.editor.oneCommanderTyping
                        Accessible.name: qsTr("在 OneCommander 里直接打字搜索")
                        onToggled: (on) => window.editor.oneCommanderTyping = on
                    }
                }

                SettingRow { // GitHub #82
                    id: opusScopeRow
                    sub: true
                    shown: window.editor.opusTyping
                    title: qsTr("在 Directory Opus 里搜索的范围")
                    keywords: qsTr("Opus;DOpus;打字;范围;整个电脑;全盘;当前文件夹;type to search;scope").split(";")
                    description: qsTr("搜索框出现在文件列表的右下角")
                    tip: qsTr("整个电脑：搜索框里不带当前路径，直接全盘搜索")

                    ScopeTabs {
                        labels: [qsTr("当前文件夹"), qsTr("整个电脑")]
                        current: window.editor.opusTypingScope === "all" ? 1 : 0
                        onActivated: (index) => window.editor.opusTypingScope = index === 1 ? "all" : "folder"
                    }
                }

                SettingRow { // GitHub #110
                    id: folderTypingRow
                    sub: true
                    shown: window.editor.explorerTyping || window.editor.opusTyping || window.editor.oneCommanderTyping
                    title: qsTr("在文件夹里打字时")
                    keywords: qsTr("打字;当前文件夹;优先;只搜;其他位置;子文件夹;Listary;Ctrl+Shift+D;current folder;type to search").split(";")
                    description: qsTr("搜索时可按 Ctrl+Shift+D 临时切换范围")
                    tip: qsTr("当前文件夹优先：先列出当前文件夹及子文件夹的结果，再列出其他位置的结果，与 Listary 一致。只搜这一层：只列当前文件夹里直接放着的，不含子文件夹")

                    ScopeTabs {
                        labels: [qsTr("当前文件夹优先"), qsTr("只搜当前文件夹"), qsTr("只搜这一层")]
                        current: window.editor.folderTyping === "level" ? 2 : window.editor.folderTyping === "only" ? 1 : 0
                        onActivated: (index) => window.editor.folderTyping = index === 2 ? "level" : index === 1 ? "only" : "first"
                    }
                }

                SettingRow { // GitHub #40
                    title: qsTr("让其他软件把 Win顺 当作 Everything 搜索")
                    keywords: qsTr("Everything;兼容;Directory Opus;DOpus;Total Commander;TC;es.exe;SDK;IPC").split(";")
                    description: qsTr("Directory Opus、Total Commander 等软件里的 Everything 搜索由 Win顺 回答，不用再装 Everything")
                    tip: qsTr("Everything 自己在运行时仍由它回答。打开后，本机的程序都可以通过这个接口查到索引里的文件名；“不搜索的文件夹”也会建索引给它们查，只是不出现在 Win顺 自己的结果里")

                    ToggleSwitch {
                        dpr: window.dpr
                        objectName: "everythingCompatToggle"
                        checked: window.editor.everythingCompat
                        Accessible.name: qsTr("让其他软件把 Win顺 当作 Everything 搜索")
                        onToggled: (on) => window.editor.everythingCompat = on
                    }
                }
            }

            SettingsSection {
                page: window.integrationPage
                tab: 2
                width: parent.width
                title: qsTr("文件夹快捷菜单")
                glyph: "\uE8B7"

                SettingRow {
                    id: folderMenuRow
                    title: qsTr("在资源管理器或桌面弹出文件夹快捷菜单")
                    keywords: qsTr("双击;空白;空白处;收藏;收藏夹;最近;文件夹菜单;文件夹快捷菜单;资源管理器;桌面;Listary;double-click;folder menu").split(";")
                    unlocks: [opusMenuRow]
                    description: qsTr("在空白处双击或按中键弹出常用文件夹菜单，选中后直接在当前窗口打开")
                    tip: qsTr("Listary 正在运行时会自动交给 Listary 处理，避免弹出两个菜单")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.folderMenu
                        onToggled: (on) => window.editor.folderMenu = on
                    }
                }

                SettingRow {
                    id: folderMenuHowRow
                    sub: true
                    title: qsTr("触发方式")
                    description: qsTr("至少保留一项")
                    keywords: qsTr("双击;左键;中键;鼠标;滚轮;空白;文件夹菜单;Listary;double-click;middle").split(";")

                    ScopeTabs {
                        multiSelect: true
                        disabled: window.editor.folderMenu ? [] : [0, 1]
                        labels: [qsTr("双击鼠标左键"), qsTr("鼠标中键")]
                        checkedIndices: {
                            const list = []
                            if (window.editor.folderMenuDouble) list.push(0)
                            if (window.editor.folderMenuMiddle) list.push(1)
                            return list
                        }
                        onToggled: (index, on) => {
                            if (index === 0) window.editor.folderMenuDouble = on
                            else if (index === 1) window.editor.folderMenuMiddle = on
                        }
                    }
                }

                SettingRow {
                    id: folderMenuWhereRow
                    sub: true
                    title: qsTr("触发位置")
                    description: qsTr("至少保留一项")
                    keywords: qsTr("空白;文件夹菜单;资源管理器;桌面;位置;Listary").split(";")

                    ScopeTabs {
                        multiSelect: true
                        disabled: window.editor.folderMenu ? [] : [0, 1]
                        labels: [qsTr("资源管理器"), qsTr("桌面")]
                        checkedIndices: {
                            const list = []
                            if (window.editor.folderMenuExplorer) list.push(0)
                            if (window.editor.folderMenuDesktop) list.push(1)
                            return list
                        }
                        onToggled: (index, on) => {
                            if (index === 0) window.editor.folderMenuExplorer = on
                            else if (index === 1) window.editor.folderMenuDesktop = on
                        }
                    }
                }

                SettingRow {
                    id: folderMenuGroupsRow
                    sub: true
                    title: qsTr("菜单里列出的内容")
                    description: qsTr("至少保留一项")
                    tip: qsTr("收藏夹中包含收藏的文件夹、文件、应用和自定义命令")
                    keywords: qsTr("双击;文件夹菜单;收藏;收藏夹;自定义命令;最近;最近打开;其他窗口;别的窗口;已打开").split(";")

                    ScopeTabs {
                        multiSelect: true
                        disabled: window.editor.folderMenu ? [] : [0, 1, 2]
                        labels: [qsTr("收藏夹"), qsTr("最近文件夹"), qsTr("其他窗口")]
                        checkedIndices: {
                            const list = []
                            if (window.editor.folderMenuFavorites) list.push(0)
                            if (window.editor.folderMenuRecent) list.push(1)
                            if (window.editor.folderMenuWindows) list.push(2)
                            return list
                        }
                        onToggled: (index, on) => {
                            if (index === 0) window.editor.folderMenuFavorites = on
                            else if (index === 1) window.editor.folderMenuRecent = on
                            else if (index === 2) window.editor.folderMenuWindows = on
                        }
                    }
                }

                SettingRow {
                    id: folderMenuFavoriteKindsRow
                    sub: true
                    readonly property bool active: window.editor.folderMenu && window.editor.folderMenuFavorites
                    title: qsTr("收藏夹里显示的类型")
                    description: qsTr("至少保留一项；未勾选的类型不再出现，应用也可在菜单里直接展开或折叠")
                    keywords: qsTr("文件夹菜单;收藏夹;收藏;类型;隐藏;过滤;文件夹;文件;应用;系统设置;自定义命令;折叠;展开;favorites;apps;hide;fold;collapse").split(";")

                    FlatButton {
                        objectName: "folderMenuManageFavoritesButton"
                        dpr: window.dpr
                        flat: true
                        text: qsTr("管理收藏夹")
                        trailingGlyph: "\uE76C"
                        onClicked: window.showPage(window.favoritesPage)
                    }

                    body: Flow {
                        objectName: "folderMenuFavoriteKindChoices"
                        width: parent.width
                        spacing: 4
                        Repeater {
                            model: [
                                { title: qsTr("文件夹"), setting: "folderMenuFavoriteFolders" },
                                { title: qsTr("文件"), setting: "folderMenuFavoriteFiles" },
                                { title: qsTr("应用"), setting: "folderMenuFavoriteApps" },
                                { title: qsTr("系统设置"), setting: "folderMenuFavoritePlaces" },
                                { title: qsTr("自定义命令"), setting: "folderMenuFavoriteCommands" }
                            ]
                            delegate: ScopeTabs {
                                required property var modelData
                                required property int index
                                objectName: "folderMenuFavoriteKind" + index
                                dpr: window.dpr
                                multiSelect: true
                                disabled: folderMenuFavoriteKindsRow.active ? [] : [0]
                                labels: [modelData.title]
                                checkedIndices: window.editor[modelData.setting] ? [0] : []
                                onToggled: (index, on) => window.editor[modelData.setting] = on
                            }
                        }
                    }
                }

                Repeater { // the menu's order: favorites, recent, open
                    model: [
                        { title: qsTr("收藏夹显示方式"), words: qsTr("文件夹菜单;直接显示收藏夹;直接显示;收藏;收藏夹;一级;二级;平铺;全部;数量;更多;展开;折叠;Listary;inline;favorites"),
                          group: "folderMenuFavorites", setting: "folderMenuFavoritesInline" },
                        { title: qsTr("最近的文件夹显示方式"), words: qsTr("文件夹菜单;直接显示最近的文件夹;直接显示;最近;一级;二级;平铺;全部;数量;更多;展开;折叠;Listary;inline;recent;folders"),
                          group: "folderMenuRecent", setting: "folderMenuRecentInline" },
                        { title: qsTr("已打开的文件夹显示方式"), words: qsTr("文件夹菜单;直接显示已打开的文件夹;直接显示;已打开;其他窗口;一级;二级;平铺;全部;数量;更多;展开;折叠;Listary;inline;open;windows;folders"),
                          group: "folderMenuWindows", setting: "folderMenuWindowsInline" }
                    ]
                    delegate: SettingRow {
                        id: inlineFoldersRow
                        required property var modelData
                        readonly property int count: window.editor[modelData.setting]
                        readonly property bool active: window.editor.folderMenu && window.editor[modelData.group]
                        property int lastLimit: count > 0 ? count : 5
                        onCountChanged: if (count > 0) lastLimit = count
                        sub: true
                        title: modelData.title
                        description: count === 0
                                     ? qsTr("收进二级菜单，保持顶层菜单紧凑")
                                     : count < 0
                                       ? qsTr("全部直接列在顶层菜单，过多时可滚动")
                                       : qsTr("顶层直接显示前 %1 个，其余收进“更多…”").arg(count)
                        keywords: modelData.words.split(";")
                        ScopeTabs {
                            objectName: inlineFoldersRow.modelData.setting + "Mode"
                            anchors.verticalCenter: parent.verticalCenter
                            dpr: window.dpr
                            disabled: inlineFoldersRow.active ? [] : [0, 1, 2]
                            labels: [qsTr("二级菜单"), qsTr("前几个"), qsTr("全部")]
                            current: inlineFoldersRow.count === 0 ? 0 : inlineFoldersRow.count < 0 ? 2 : 1
                            onActivated: (index) => {
                                if (index === 0) window.editor[inlineFoldersRow.modelData.setting] = 0
                                else if (index === 2) window.editor[inlineFoldersRow.modelData.setting] = -1
                                else window.editor[inlineFoldersRow.modelData.setting] = inlineFoldersRow.lastLimit
                            }
                        }
                        InputBox {
                            objectName: inlineFoldersRow.modelData.setting
                            anchors.verticalCenter: parent.verticalCenter
                            visible: inlineFoldersRow.count > 0
                            enabled: inlineFoldersRow.active
                            width: 56
                            dpr: window.dpr
                            horizontalAlignment: TextInput.AlignHCenter
                            text: String(Math.max(1, inlineFoldersRow.count))
                            validator: IntValidator { bottom: 1; top: 20 }
                            onEditingFinished: {
                                const count = parseInt(text, 10)
                                if (!isNaN(count) && count >= 1 && count <= 20)
                                    window.editor[inlineFoldersRow.modelData.setting] = count
                                text = Qt.binding(() => String(Math.max(1, inlineFoldersRow.count)))
                            }
                        }
                    }
                }

                SettingRow { // GitHub #384
                    sub: true
                    title: qsTr("文件夹的子列也列出文件")
                    description: qsTr("文件夹在前、文件在后，点一下文件就打开；打开、保存对话框里仍只列文件夹")
                    keywords: qsTr("文件夹菜单;文件;子列;子菜单;列出文件;打开文件;Listary;files").split(";")

                    ToggleSwitch {
                        objectName: "folderMenuFilesSwitch"
                        dpr: window.dpr
                        enabled: window.editor.folderMenu
                        checked: window.editor.folderMenuFiles
                        onToggled: (on) => window.editor.folderMenuFiles = on
                    }
                }

                SettingRow { // GitHub #200
                    id: folderMenuActionsRow
                    sub: true
                    title: qsTr("菜单里的操作")
                    description: qsTr("菜单末尾的“在此搜索”“复制路径”“收藏这个文件夹”等；没勾选的不再出现")
                    keywords: qsTr("文件夹菜单;搜索;复制路径;复制地址;copy path;命令提示符;cmd;PowerShell;收藏;设置;隐藏;search;command prompt;favorites;settings").split(";")

                    body: Flow {
                        objectName: "folderMenuActionChoices"
                        width: parent.width
                        spacing: 4
                        Repeater {
                            model: [
                                { title: qsTr("搜索"), setting: "folderMenuItemSearch" },
                                { title: qsTr("复制路径"), setting: "folderMenuItemCopyPath" },
                                { title: qsTr("命令提示符"), setting: "folderMenuItemCmd" },
                                { title: qsTr("PowerShell"), setting: "folderMenuItemPowerShell" },
                                { title: qsTr("收藏"), setting: "folderMenuItemFavorite" },
                                { title: qsTr("设置"), setting: "folderMenuItemSettings" }
                            ]
                            delegate: ScopeTabs {
                                required property var modelData
                                required property int index
                                objectName: "folderMenuAction" + index
                                dpr: window.dpr
                                multiSelect: true
                                disabled: window.editor.folderMenu ? [] : [0]
                                labels: [modelData.title]
                                checkedIndices: window.editor[modelData.setting] ? [0] : []
                                onToggled: (index, on) => window.editor[modelData.setting] = on
                            }
                        }
                    }
                }
                SettingRow {
                    id: opusMenuRow
                    sub: true
                    // on | stale | off | taken | manual | busy (SettingsEditor::opusFolderMenu)
                    readonly property string setup: window.editor.opusFolderMenu
                    property bool copied: false
                    property bool confirming: false // Opus runs: it restarts after a second click
                    shown: window.editor.folderMenu && window.editor.fileManagers.indexOf("dopus") >= 0
                    title: qsTr("在 Directory Opus 的空白处也弹出")
                    keywords: qsTr("双击;中键;空白;此电脑;文件夹菜单;Directory Opus;Opus;dopus").split(";")
                    description: {
                        let text
                        switch (setup) {
                        case "on":
                            text = qsTr("已在 Opus 的设置里加好，按“触发方式”弹出")
                            break
                        case "stale":
                            text = qsTr("Opus 里设的是别处或旧版 Win顺 的命令（旧版的不支持中键，在“此电脑”里也不弹），更新一下就好")
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
                        dpr: window.dpr
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
                        dpr: window.dpr
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
                                dpr: window.dpr
                                id: opusCommand
                                width: parent.width - copyOpusCommand.width - parent.spacing
                                    - (openOpusPrefs.visible ? openOpusPrefs.width + parent.spacing : 0)
                                text: window.editor.opusFolderMenuCommand()
                                readOnly: true
                            }
                            FlatButton {
                                dpr: window.dpr
                                id: copyOpusCommand
                                text: opusMenuRow.copied ? qsTr("已复制") : qsTr("复制")
                                onClicked: {
                                    window.editor.copyText(opusCommand.text)
                                    opusMenuRow.copied = true
                                    copiedReset.restart()
                                }
                            }
                            FlatButton {
                                dpr: window.dpr
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
                    id: folderMenuHotkeyRow
                    // Shown with the double click off too: the hotkey alone
                    // brings the menu (GitHub #25).
                    title: qsTr("用快捷键弹出文件夹菜单")
                    keywords: qsTr("快捷键;热键;组合键;文件夹菜单;收藏;最近;对话框;Directory Opus;Total Commander;XYplorer;Pulse;OneCommander;hotkey;folder menu").split(";")
                    values: [window.editor.folderMenuHotkey]
                    description: qsTr("双击被占用时使用，在文件管理器或对话框中按下即可弹出菜单")
                    tip: qsTr("支持资源管理器、桌面、Directory Opus、Total Commander、XYplorer、OneCommander 及“打开/另存为”对话框（在当前窗口跳转）；在 Pulse 中交给 Pulse 处理；在其他窗口中用默认文件管理器打开。再按一次关闭")

                    FlatButton {
                        dpr: window.dpr
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
            }

            SettingsSection {
                page: window.integrationPage
                tab: 3
                width: parent.width
                title: qsTr("对话框联动")
                glyph: "\uE8E5"

                SettingRow {
                    title: qsTr("Ctrl+G 转到文件管理器的文件夹")
                    keywords: qsTr("跳转;转到;对话框;另存为;打开文件;资源管理器;Total Commander;Directory Opus;jump").split(";")
                    description: qsTr("在“打开”“另存为”里按 Ctrl+G，转到文件管理器当前的文件夹")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.dialogJump
                        onToggled: (on) => window.editor.dialogJump = on
                    }
                }

                SettingRow {
                    id: dialogBarRow
                    title: qsTr("对话框旁的搜索框")
                    keywords: qsTr("对话框;另存为;打开文件;保存").split(";")
                    unlocks: [dialogBarPlaceRow, dialogBarSuggestionsRow, dialogBarSuggestionSourcesRow]
                    description: qsTr("在“打开”“另存为”旁搜文件夹和文件，选中后直接转过去")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.dialogBar
                        onToggled: (on) => window.editor.dialogBar = on
                    }
                }

                SettingRow {
                    id: dialogBarPlaceRow
                    sub: true
                    shown: window.editor.dialogBar
                    title: qsTr("搜索框的位置")
                    keywords: qsTr("位置;下方;左边;右边;对话框").split(";")
                    description: qsTr("“自动”时放在下方，放不下就换到旁边")

                    ScopeTabs {
                        dpr: window.dpr
                        labels: [qsTr("自动"), qsTr("下方"), qsTr("左边"), qsTr("右边")]
                        current: Math.max(0, window.dialogBarPlaces.indexOf(window.editor.dialogBarPlace))
                        onActivated: (index) => window.editor.dialogBarPlace = window.dialogBarPlaces[index]
                    }
                }

                SettingRow {
                    id: dialogBarSuggestionsRow
                    sub: true
                    shown: window.editor.dialogBar
                    readonly property bool anySource: window.editor.dialogBarSuggestOpen
                                                   || window.editor.dialogBarSuggestClipboard
                                                   || window.editor.dialogBarSuggestFavorites
                                                   || window.editor.dialogBarSuggestRecent
                    title: qsTr("点进来就列出常用位置")
                    keywords: qsTr("最近;常用;隐私;建议;列表;对话框").split(";")
                    description: anySource
                                 ? qsTr("点进空白搜索框立即列出下方勾选的位置；关掉后打字才出结果，按 ↓ 仍可展开")
                                 : qsTr("下方未勾选任何常用位置来源；勾选后生效")

                    ToggleSwitch {
                        objectName: "dialogBarSuggestionsToggle"
                        dpr: window.dpr
                        checked: window.editor.dialogBarSuggestions
                        Accessible.name: qsTr("点进来就列出常用位置")
                        onToggled: (on) => window.editor.dialogBarSuggestions = on
                    }
                }

                SettingRow {
                    id: dialogBarSuggestionSourcesRow
                    sub: true
                    shown: window.editor.dialogBar
                    stackControl: true
                    title: qsTr("常用位置里列出的内容")
                    keywords: qsTr("常用位置;弹出;已打开;目录;文件夹;剪贴板;收藏夹;最近;隐私;对话框;open;clipboard;favorites;recent;privacy").split(";")
                    description: qsTr("未勾选的不会列出，按 ↓ 也不会显示，不影响搜索；全部取消则只保留搜索")

                    ScopeTabs {
                        objectName: "dialogBarSuggestionSourcesTabs"
                        multiSelect: true
                        dpr: window.dpr
                        labels: [qsTr("已打开文件夹"), qsTr("剪贴板"), qsTr("收藏夹"), qsTr("最近使用")]
                        checkedIndices: {
                            const list = []
                            if (window.editor.dialogBarSuggestOpen) list.push(0)
                            if (window.editor.dialogBarSuggestClipboard) list.push(1)
                            if (window.editor.dialogBarSuggestFavorites) list.push(2)
                            if (window.editor.dialogBarSuggestRecent) list.push(3)
                            return list
                        }
                        onToggled: (index, on) => {
                            if (index === 0) window.editor.dialogBarSuggestOpen = on
                            else if (index === 1) window.editor.dialogBarSuggestClipboard = on
                            else if (index === 2) window.editor.dialogBarSuggestFavorites = on
                            else if (index === 3) window.editor.dialogBarSuggestRecent = on
                        }
                    }
                }

                SettingRow {
                    title: qsTr("对话框自动转到文件管理器的文件夹")
                    keywords: qsTr("自动跳转;跟随;对话框;另存为;资源管理器").split(";")
                    unlocks: [dialogAutoJumpWhenRow]
                    description: qsTr("不用按 Ctrl+G，对话框自己转到文件管理器当前的文件夹")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.dialogAutoJump
                        onToggled: (on) => window.editor.dialogAutoJump = on
                    }
                }

                SettingRow {
                    id: dialogAutoJumpWhenRow
                    sub: true
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

                SettingRow {
                    id: dialogRulesRow
                    objectName: "dialogRulesRow"
                    title: qsTr("程序自己的选文件窗口")
                    keywords: qsTr("规则;自定义;对话框;识别;添加数据;ArcGIS;CAD;dialogs.json").split(";")
                    description: qsTr("有的程序用自己画的窗口选文件，上面几项认不出它。打开那个窗口，回来点“识别窗口”，它就也能搜、能转")
                                 + (window.editor.dialogRuleStatus.length > 0 ? "\n" + window.editor.dialogRuleStatus : "")

                    FlatButton {
                        objectName: "dialogRuleLearnButton"
                        dpr: window.dpr
                        text: window.editor.dialogRuleLearning ? qsTr("等待中…") : qsTr("识别窗口")
                        enabled: !window.editor.dialogRuleLearning
                        onClicked: window.editor.learnDialogRule()
                    }

                    FlatButton {
                        objectName: "dialogRuleFileButton"
                        dpr: window.dpr
                        text: qsTr("规则文件")
                        onClicked: window.editor.openDialogRules()
                    }
                }
            }
            // </page integration>
                } }
            }

            PageSlot {
                id: favoritesSlot
                index: window.favoritesPage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({ favoritesSection: favoritesSection, dropPieRow: favoritesSection.dropPieRow })
            // <page favorites>
            FavoritesSection {
                id: favoritesSection
                page: window.favoritesPage
                width: parent.width
                favorites: window.editor.favorites
                editor: window.editor
            }
            // </page favorites>
                } }
            }

            PageSlot {
                id: clipboardSlot
                index: window.clipboardPage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({
                        captureKeywordError: captureKeywordError, // the probe's (--settings-reopen-check)
                        clipboardUiSizeRow: clipboardUiSizeRow,
                        clipboardWinVRow: clipboardWinVRow,
                        clipboardHotkeyRow: clipboardHotkeyRow,
                        pastePlainHotkeyRow: pastePlainHotkeyRow,
                        clipboardHoldRow: clipboardHoldRow,
                        clipboardNumberKeysRow: clipboardNumberKeysRow,
                        clipboardDuplicatesRow: clipboardDuplicatesRow,
                        clipEntryHotkeysSection: clipEntryHotkeysSection
                    })
                    recordingHotkey: clipHotkeyRecorder.recording || pastePlainHotkeyRecorder.recording
                                     || clipEntryHotkeysSection.recordingId !== 0
                    onResetForReopen: {
                        clipHotkeyRecorder.stop()
                        pastePlainHotkeyRecorder.stop()
                        clipEntryHotkeysSection.stopRecording()
                        appInput.clear()
                        captureKeywordBox.text = Qt.binding(() => window.editor.captureKeyword)
                        captureKeywordError.text = ""
                        clipImport.path = ""
                        clipImport.split = 1
                        clearClipboardButton.confirming = false
                    }
                    onOpened: clipImport.ditto = window.editor.dittoDatabase()
            // <page clipboard>
            SettingsSection {
                page: window.clipboardPage
                tab: 0
                width: parent.width
                title: qsTr("记录范围与图文识别")
                glyph: "\uE77F"
                note: qsTr("只存在这台电脑上，不会上传。按住 Ctrl 或 Shift 点击可以多选，合在一起粘贴。")

                SettingRow {
                    title: qsTr("记录剪贴板历史")
                    keywords: qsTr("剪切板;复制;历史;clipboard").split(";")
                    description: qsTr("关闭后停止监听并释放历史缓存，已有记录保留在本机。密码管理器复制的密码不会被记录")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboard
                        onToggled: (on) => window.editor.clipboard = on
                    }
                }

                SettingRow {
                    sub: true
                    title: qsTr("记录图片")
                    keywords: qsTr("截图;图片;照片;image").split(";")
                    description: window.editor.screenshotHistory
                                 ? qsTr("复制的图片也记下来；Win顺 截图在下方单独控制")
                                 : qsTr("截图和复制的图片也记下来")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardImages
                        onToggled: (on) => window.editor.clipboardImages = on
                    }
                }

                SettingRow {
                    sub: true
                    objectName: "clipboardScreenshotsRow"
                    title: qsTr("记录截图")
                    keywords: qsTr("截图;截屏;贴图;保存;复制;历史;screenshot;pin").split(";")
                    description: window.editor.clipboardScreenshots === "all"
                                 ? qsTr("复制、贴图、保存的 Win顺 截图都记进剪贴板历史")
                                 : window.editor.clipboardScreenshots === "copied"
                                 ? qsTr("只有点“复制”的 Win顺 截图记进剪贴板历史，只贴图或保存的不记")
                                 : qsTr("Win顺 截图不记进剪贴板历史；复制仍写入系统剪贴板，可直接粘贴")
                    shown: window.editor.screenshotHistory

                    ScopeTabs {
                        objectName: "clipboardScreenshotsTabs"
                        readonly property var modes: ["all", "copied", "off"]
                        labels: [qsTr("全部"), qsTr("仅复制时"), qsTr("不记录")]
                        current: Math.max(0, modes.indexOf(window.editor.clipboardScreenshots))
                        onActivated: (index) => window.editor.clipboardScreenshots = modes[index]
                    }
                }

                OcrPackageRow {
                    ocrPackage: window.editor.ocrPackage
                }

                SettingRow {
                    // The screenshots the screenshot plugin (winshun.capture)
                    // put in the history, listed by a word in the search box
                    // (App::screenshotKeyword); while the plugin is on.
                    sub: true
                    title: qsTr("截图历史关键词")
                    keywords: qsTr("截图;截图历史;截屏;关键词;screenshot").split(";")
                    values: [window.editor.captureKeyword]
                    description: window.editor.clipboardScreenshots === "off"
                                 ? qsTr("在搜索框输入“%1”加空格，列出已有截图（“记录截图”已关闭，新截图不再加入）；Enter 复制并粘贴，Ctrl+Enter 贴到屏幕，Delete 删除").arg(window.editor.captureKeyword)
                                 : qsTr("在搜索框输入“%1”加空格，列出截过的图，再输入文字按图里识别出的文字筛选；Enter 复制并粘贴，Ctrl+Enter 贴到屏幕，Delete 删除").arg(window.editor.captureKeyword)
                    shown: window.editor.screenshotHistory
                    bodyShown: captureKeywordError.text.length > 0

                    InputBox {
                        dpr: window.dpr
                        id: captureKeywordBox
                        width: 96
                        text: window.editor.captureKeyword
                        maximumLength: 16
                        function commit() {
                            captureKeywordError.text = window.editor.saveCaptureKeyword(text)
                            if (captureKeywordError.text.length === 0)
                                text = Qt.binding(() => window.editor.captureKeyword)
                        }
                        onAccepted: commit()
                        onEditingFinished: commit()
                    }

                    body: Text {
                        id: captureKeywordError
                        width: parent.width
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    id: clipboardDuplicatesRow
                    objectName: "clipboardDuplicatesRow"
                    title: qsTr("保留重复项")
                    keywords: qsTr("重复;去重;合并;多次复制;再次复制;允许重复;Ditto;duplicate").split(";")
                    description: qsTr("同样的内容再复制一次，也记成新的一条；关闭时只留一条，移到最前")
                    tip: qsTr("紧接着复制同样的内容（比如连按两次 Ctrl+C）仍只记一条。从剪贴板里粘贴出去不算复制，不会多出一条")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardDuplicates
                        onToggled: (on) => window.editor.clipboardDuplicates = on
                    }
                }

                SettingRow {
                    id: clipboardAppsRow
                    title: qsTr("不记录这些程序复制的内容")
                    keywords: qsTr("密码;隐私;排除;程序;截图;exe;KeePass").split(";")
                    values: window.editor.clipboardExcludedApps
                    description: qsTr("给密码管理器、外部截图工具等再加一层保险")

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
                                dpr: window.dpr
                                id: appInput
                                width: 240
                                placeholder: qsTr("输入程序文件名，例如 KeePass.exe")
                                onAccepted: if (window.editor.addClipboardExcludedApp(text)) clear()
                            }
                            FlatButton {
                                dpr: window.dpr
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
                tab: 0
                width: parent.width
                title: qsTr("粘贴动作与列表显示")
                glyph: "\uE8C8"

                SettingRow {
                    title: qsTr("单击粘贴")
                    keywords: qsTr("单击;点击;点一下;直接粘贴;双击;远程;手机;触屏").split(";")
                    description: qsTr("点一下就粘贴，和 Windows 自带的剪贴板一样，手机远程操作时也方便。按住 Ctrl 或 Shift 点击仍是多选；关掉时单击选中、双击粘贴")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardClickToPaste
                        onToggled: (on) => window.editor.clipboardClickToPaste = on
                    }
                }

                SettingRow {
                    title: qsTr("默认粘贴为纯文本")
                    keywords: qsTr("纯文本;格式;去格式;无格式;去掉格式;富文本;样式;字体;保留格式;Word;WPS").split(";")
                    description: qsTr("粘贴文字时去掉字体、颜色、链接等格式，贴进 Word、WPS 也和打字一样。要保留格式时按 Shift+Enter，或在右键菜单里选“保留格式粘贴”")
                    tip: qsTr("单击、双击、Enter、Alt+数字和快速粘贴快捷键都按纯文本粘贴；文件和图片照常粘贴")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardPlainByDefault
                        onToggled: (on) => window.editor.clipboardPlainByDefault = on
                    }
                }

                SettingRow {
                    title: qsTr("粘贴后保持位置")
                    keywords: qsTr("位置;顺序;排序;跳到最前;置顶;不动;固定顺序;粘贴后").split(";")
                    description: qsTr("从列表里粘贴一条后留在原来的位置，不跳到最前面")
                    tip: qsTr("重新复制同样的内容仍会排到最前。粘贴过的条目右下角带蓝色对勾，也不会因“保留天数”到期而被删")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardKeepPlace
                        onToggled: (on) => window.editor.clipboardKeepPlace = on
                    }
                }

                SettingRow { // a side preview left open comes back with the window (GitHub #9; off since #174)
                    title: qsTr("再次打开时恢复预览")
                    keywords: qsTr("预览;恢复;记住;保留;保持;下次;再次;侧边;restore;preview").split(";")
                    description: window.editor.clipboardPreviewRestore
                                 ? qsTr("预览开着时关闭剪贴板，下次打开预览也跟着回来")
                                 : qsTr("关闭剪贴板时预览一起收起，下次打开从列表开始")

                    ToggleSwitch {
                        objectName: "clipboardPreviewRestoreSwitch"
                        dpr: window.dpr
                        checked: window.editor.clipboardPreviewRestore
                        onToggled: (on) => window.editor.clipboardPreviewRestore = on
                    }
                }

                SettingRow { // the tab it opens on (GitHub #404); also a tab's own menu, 打开时显示这里
                    title: qsTr("打开时显示")
                    keywords: qsTr("分组;默认;打开;标签;记住;上次;固定;文本;链接;图片;文件;tab;group").split(";")
                    description: qsTr("每次打开剪贴板先显示哪一栏；在分组上点右键也能设")

                    ChoiceBox {
                        id: clipboardOpenTabPicker
                        objectName: "clipboardOpenTabPicker"
                        implicitWidth: 180
                        implicitHeight: 36
                        model: window.editor.clipboardOpenTabs
                        textRole: "text"
                        valueRole: "value"
                        // A group since removed opens on 全部, the first.
                        currentIndex: Math.max(0, window.editor.clipboardOpenTabs.findIndex(t => t.value === window.editor.clipboardOpenTab))
                        onActivated: window.editor.clipboardOpenTab = currentValue
                        Accessible.name: qsTr("打开时显示")
                        dpr: window.dpr
                        leftPadding: 12
                        contentItem: Text {
                            text: clipboardOpenTabPicker.displayText
                            font: clipboardOpenTabPicker.font
                            color: clipboardOpenTabPicker.palette.buttonText
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        Connections {
                            target: clipboardOpenTabPicker.popup
                            function onOpened() { window.frame.addPopup(clipboardOpenTabPicker.popup.background) }
                        }
                    }
                }

                SettingRow {
                    id: clipboardUiSizeRow
                    title: qsTr("剪贴板大小")
                    keywords: qsTr("字体;字号;文字大小;行高;密集;紧凑;宽松;ditto;Ctrl++;Ctrl+-;Ctrl+0;zoom;font size;dense").split(";")
                    description: qsTr("剪贴板文字和行高，可以和搜索框不同；在剪贴板里按 Ctrl++ / Ctrl+- 可快速调整，Ctrl+0 恢复跟随。密集是每条一行的小字")

                    ScopeTabs {
                        readonly property var sizes: ["follow", "dense", "compact", "standard", "roomy"]
                        labels: [qsTr("跟随界面大小"), qsTr("密集"), qsTr("紧凑"), qsTr("标准"), qsTr("宽松")]
                        current: Math.max(0, sizes.indexOf(window.editor.clipboardUiSize))
                        onActivated: (index) => window.editor.clipboardUiSize = sizes[index]
                    }
                }

                SettingRow {
                    title: qsTr("图片缩略图")
                    keywords: qsTr("缩略图;图片;截图;放大;看清;细节;等高;一样高;行高;整齐").split(";")
                    description: qsTr("小：图片那一行和别的一样高；大：缩略图能看清细节，不用打开预览")

                    ScopeTabs {
                        readonly property var sizes: ["small", "standard", "large"]
                        labels: [qsTr("小"), qsTr("标准"), qsTr("大")]
                        current: Math.max(0, sizes.indexOf(window.editor.clipboardPictureSize))
                        onActivated: (index) => window.editor.clipboardPictureSize = sizes[index]
                    }
                }
            }

            SettingsSection {
                page: window.clipboardPage
                tab: 0
                width: parent.width
                title: qsTr("弹出位置与窗口布局")
                glyph: "\uE740"

                SettingRow {
                    title: qsTr("剪贴板打开位置")
                    id: clipboardOpenAtRow
                    readonly property var places: ["caret", "pointer", "last"]
                    keywords: qsTr("位置;鼠标;指针;光标;跟随;弹出;输入框;固定;上次;记住").split(";")
                    description: window.editor.clipboardOpenAt === "last"
                                 ? qsTr("开在上次拖到的地方，大小也不变；有鼠标的那块屏幕上")
                                 : qsTr("每次打开时放在所选位置旁边，空间不足时自动调整到屏幕内")

                    ScopeTabs {
                        labels: [qsTr("输入光标旁"), qsTr("鼠标指针旁"), qsTr("上次的位置")]
                        current: Math.max(0, clipboardOpenAtRow.places.indexOf(window.editor.clipboardOpenAt))
                        onActivated: (index) => window.editor.clipboardOpenAt = clipboardOpenAtRow.places[index]
                    }
                }

                SettingRow {
                    // By the caret it opens under it, or over it where more
                    // rows fit there (GitHub #38, Placement.above). Never
                    // where it was last put.
                    sub: true
                    shown: window.editor.clipboardOpenAt !== "last"
                    title: qsTr("开在光标上方时翻转")
                    keywords: qsTr("布局;上方;下方;翻转;颠倒;倒序;顺序").split(";")
                    description: window.editor.clipboardSearchBoxSide === "far"
                                 ? qsTr("最新的条目挨着光标，越往上越早")
                                 : qsTr("搜索框和最新的条目都挨着光标，越往上越早")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardFlipAbove
                        onToggled: (on) => window.editor.clipboardFlipAbove = on
                    }
                }

                SettingRow {
                    // Off: the row keeps its buttons (搜索框位置 still places
                    // them); typed letters search nothing (ClipboardPage.fieldShown).
                    sub: true
                    title: qsTr("显示搜索框")
                    keywords: qsTr("搜索框;搜索;隐藏;关闭;输入框;打字").split(";")
                    description: qsTr("关掉后窗口顶上只留按钮，打字不再搜索；方向键、Enter 和快捷键照常")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardSearchBox
                        onToggled: (on) => window.editor.clipboardSearchBox = on
                    }
                }

                SettingRow {
                    // Not flipped, the window over the caret is laid out as
                    // under it, so "away" is then only "under the entries".
                    sub: true
                    title: qsTr("搜索框位置")
                    keywords: qsTr("布局;上方;下方;搜索框;位置;输入框;顶部;底部").split(";")
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
                    title: qsTr("全屏时不响应剪贴板快捷键")
                    keywords: qsTr("全屏;游戏;快捷键;屏蔽;暂停;clipboard;fullscreen").split(";")
                    description: qsTr("其他程序全屏时，快捷键不弹出剪贴板；仍然记录复制内容，也能从菜单手动打开")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardPauseInFullScreen
                        onToggled: (on) => window.editor.clipboardPauseInFullScreen = on
                    }
                }
            }

            SettingsSection {
                page: window.clipboardPage
                tab: 1
                width: parent.width
                title: qsTr("呼出剪贴板")
                glyph: "\uE765"

                SettingRow {
                    id: clipboardWinVRow
                    title: qsTr("用 Win+V 打开，代替 Windows 自带的剪贴板")
                    keywords: qsTr("Win+V;剪切板;代替;替换").split(";")
                    description: qsTr("Windows 的表情面板改用 Win+. 打开")

                    ToggleSwitch {
                        dpr: window.dpr
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
                                dpr: window.dpr
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
                                dpr: window.dpr
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
                    id: clipboardHotkeyRow
                    title: qsTr("另设快捷键")
                    keywords: qsTr("快捷键;热键;组合键;hotkey").split(";")
                    values: [window.editor.clipboardHotkey]
                    description: qsTr("不想换掉 Win+V 时用，例如 Win+Alt+V")

                    FlatButton {
                        dpr: window.dpr
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

                SettingRow { // GitHub #408
                    id: pastePlainHotkeyRow
                    title: qsTr("粘贴为纯文本的快捷键")
                    keywords: qsTr("纯文本;去格式;无格式;清除格式;去掉格式;PureText;快捷键;热键;plain text;paste;hotkey").split(";")
                    values: [window.editor.pastePlainHotkey]
                    description: qsTr("在任何程序里按下，把剪贴板里的文字去掉字体、颜色和链接后粘贴，例如 Ctrl+Alt+V")
                    tip: qsTr("剪贴板里也换成纯文本。不记录剪贴板历史时也能用")

                    FlatButton {
                        dpr: window.dpr
                        visible: window.editor.pastePlainHotkey.length > 0 && !pastePlainHotkeyRecorder.recording
                        flat: true
                        text: qsTr("清除")
                        onClicked: window.editor.pastePlainHotkey = ""
                    }
                    HotkeyRecorder {
                        id: pastePlainHotkeyRecorder
                        objectName: "pastePlainHotkeyRecorder"
                        hotkey: window.editor.pastePlainHotkey
                        onRecorded: (hotkey) => window.editor.pastePlainHotkey = hotkey
                    }

                    body: Text {
                        visible: window.editor.pastePlainHotkeyError.length > 0
                        width: parent.width
                        text: window.editor.pastePlainHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }

                SettingRow {
                    id: clipboardHoldRow
                    title: qsTr("按住连按快捷键，松开粘贴")
                    keywords: qsTr("快捷键;按住;连按;松开;选择;Ditto;hotkey").split(";")
                    description: qsTr("按住修饰键每按一次往下选一条，松开粘贴")
                    tip: qsTr("按住快捷键里的 Ctrl、Alt、Shift 或 Win，每再按一下就往下选一条，松开就粘贴；按住约一秒再松开，粘贴第一条；按住时按 Esc 不粘贴。开启后按住再按快捷键不再关闭窗口")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardHoldToStep
                        onToggled: (on) => window.editor.clipboardHoldToStep = on
                    }
                }

                SettingRow {
                    title: qsTr("粘贴到以管理员身份运行的程序")
                    keywords: qsTr("管理员;权限;提权;粘贴不上;索引服务;admin;elevated").split(";")
                    description: qsTr("由索引服务代按粘贴键；关闭后只复制，要自己按 Ctrl+V")
                    tip: qsTr("Win顺 以普通权限运行，Windows 不让它向管理员程序发按键。开启后，选中的内容先放上剪贴板，再由索引服务在那个窗口里按一次 Ctrl+V（终端里是 Shift+Insert）；它只按这一组键，只对正在最前面的管理员窗口。没有安装索引服务时，仍然要自己按 Ctrl+V")

                    ToggleSwitch {
                        dpr: window.dpr
                        checked: window.editor.clipboardPasteElevated
                        onToggled: (on) => window.editor.clipboardPasteElevated = on
                    }
                }
            }

            SettingsSection {
                page: window.clipboardPage
                tab: 1
                width: parent.width
                title: qsTr("数字键快速粘贴")
                glyph: "\uE92E"

                SettingRow {
                    id: clipboardNumberKeysRow
                    title: qsTr("面板内数字键粘贴")
                    unlocks: [clipNumberHintsRow]
                    keywords: qsTr("快捷键;数字键;序号;Alt+1;Ctrl+1;冲突;hotkey").split(";")
                    description: (window.editor.clipboardPlainByDefault
                                  ? qsTr("打开剪贴板面板后，按 %1+1 粘贴当前可见的第 1 条，依此类推到第 9 条（随列表滚动更新）；加按 Shift 保留格式粘贴")
                                  : qsTr("打开剪贴板面板后，按 %1+1 粘贴当前可见的第 1 条，依此类推到第 9 条（随列表滚动更新）；加按 Shift 粘贴为纯文本"))
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
                    objectName: "clipNumberHintsRow"
                    sub: window.editor.clipboardNumberKeys !== "off"
                    shown: window.editor.clipboardNumberKeys !== "off" || window.editor.letterJump
                    title: qsTr("序号提示")
                    description: window.editor.clipboardNumberKeys === "off"
                                 ? qsTr("启动器、剪贴板和对话框搜索栏共用；关闭数字键时提前预览各行字母，轻按直达键后高亮触发")
                                 : qsTr("启动器、剪贴板和对话框搜索栏共用；隐藏提示不影响数字快捷键，关闭数字键时也用于预览字母直达")
                    keywords: qsTr("快捷键;数字键;字母;字母直达;图标;按住;显示;隐藏").split(";")

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

                SettingsLinkRow {
                    objectName: "clipboardLetterJumpLink"
                    iconName: "keyboard"
                    title: qsTr("字母直达")
                    description: {
                        if (!window.editor.letterJump)
                            return qsTr("未开启 · 轻按修饰键后按字母直接粘贴（含第九条以后），启动器和剪贴板共用")
                        const key = window.editor.letterJumpKey === "ctrl" ? "Ctrl"
                                  : window.editor.letterJumpKey === "alt" ? "Alt"
                                  : window.editor.letterJumpKey === "shift" ? "Shift"
                                  : window.editor.clipboardNumberKeys === "off" ? "Ctrl"
                                  : qsTr("跟随数字键")
                        return qsTr("已开启（%1）· 启动器和剪贴板共用").arg(key)
                    }
                    destination: qsTr("结果列表 › 操作习惯")
                    targetPage: "results"
                    targetPart: "letterJumpRow"
                }
            }

            SettingsSection {
                id: clipEntryHotkeysSection
                objectName: "clipEntryHotkeysSection"
                page: window.clipboardPage
                tab: 1
                width: parent.width
                title: qsTr("单条内容的快捷键")
                glyph: "\uE765" // Keyboard
                note: qsTr("给常用的一条设一个快捷键，在任何程序里按下就直接粘贴它。设了快捷键的会放进“固定”，不会过期。")

                // The entry whose keys are being recorded, 0 for none (the page's recordingHotkey).
                property real recordingId: 0
                readonly property list<string> words: qsTr("快捷键;热键;单条;条目;一键粘贴;常用语;片段;Ditto;hotkey;snippet").split(";")
                // Its row, to show it and record its keys (showClipboardEntryHotkey); null if not listed.
                function rowOf(id) {
                    for (let i = 0; i < entryHotkeyRows.count; ++i) {
                        const row = entryHotkeyRows.itemAt(i)
                        if (row && row.clipId === id)
                            return row
                    }
                    return null
                }
                function stopRecording() {
                    for (let i = 0; i < entryHotkeyRows.count; ++i)
                        entryHotkeyRows.itemAt(i)?.recorder.stop()
                    recordingId = 0
                }

                SettingRow {
                    shown: window.editor.clipboardEntryHotkeys.length === 0
                    title: qsTr("还没有设置")
                    description: qsTr("打开剪贴板，在常用的一条上点右键，选“设置快捷键…”")
                    keywords: clipEntryHotkeysSection.words
                }

                Repeater {
                    id: entryHotkeyRows
                    model: window.editor.clipboardEntryHotkeys

                    delegate: SettingRow {
                        id: entryRow

                        required property var modelData
                        readonly property real clipId: modelData.id
                        readonly property HotkeyRecorder recorder: entryRecorder

                        title: modelData.title
                        description: modelData.detail
                        keywords: clipEntryHotkeysSection.words
                        values: [modelData.hotkey, modelData.title]

                        FlatButton {
                            dpr: window.dpr
                            visible: entryRow.modelData.hotkey.length > 0 && !entryRecorder.recording
                            flat: true
                            text: qsTr("清除")
                            onClicked: window.editor.setClipboardEntryHotkey(entryRow.clipId, "")
                        }
                        HotkeyRecorder {
                            id: entryRecorder
                            objectName: "entryHotkeyRecorder"
                            hotkey: entryRow.modelData.hotkey
                            onRecorded: (hotkey) => window.editor.setClipboardEntryHotkey(entryRow.clipId, hotkey)
                            onRecordingChanged: {
                                if (recording)
                                    clipEntryHotkeysSection.recordingId = entryRow.clipId
                                else if (clipEntryHotkeysSection.recordingId === entryRow.clipId)
                                    clipEntryHotkeysSection.recordingId = 0
                            }
                        }

                        body: Text {
                            visible: entryRow.modelData.error.length > 0
                            width: parent.width
                            text: entryRow.modelData.error
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }

                        // Made again with the list (keys set, another entry's
                        // changed): not left recording on the way out.
                        Component.onDestruction: {
                            if (clipEntryHotkeysSection.recordingId === clipId)
                                clipEntryHotkeysSection.recordingId = 0
                        }
                    }
                }
            }

            SettingsSection {
                page: window.clipboardPage
                tab: 2
                width: parent.width
                title: qsTr("保留容量与自动清理")
                glyph: "\uE74D"

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
                        dpr: window.dpr
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
            }

            SettingsSection {
                page: window.clipboardPage
                tab: 2
                width: parent.width
                title: qsTr("数据导入、备份与清空")
                glyph: "\uE8B5"

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
                        dpr: window.dpr
                        visible: clipImport.ditto.length > 0
                        enabled: !window.editor.clipboardImporting && !window.editor.clipboardBackupBusy
                        text: qsTr("从 Ditto 导入")
                        glyph: "\uE8B5" // Import
                        onClicked: {
                            clipImport.path = ""
                            window.editor.importClipboard(clipImport.ditto, 1)
                        }
                    }
                    FlatButton {
                        dpr: window.dpr
                        enabled: !window.editor.clipboardBackupBusy
                        text: window.editor.clipboardImporting ? qsTr("取消") : qsTr("导入…")
                        glyph: window.editor.clipboardImporting ? "\uE711" : "\uE8B5" // Cancel, Import
                        onClicked: window.editor.clipboardImporting ? window.editor.cancelClipboardImport() : clipImport.pick()
                    }
                }

                SettingRow {
                    sub: true
                    shown: clipImport.path.length > 0 && !window.editor.clipboardImporting
                    title: qsTr("拆分方式")
                    description: clipImport.path.split(/[\\/]/).pop()

                    ScopeTabs {
                        labels: [qsTr("每行一条"), qsTr("空行分段"), qsTr("整个文件一条")]
                        current: clipImport.split
                        onActivated: (index) => clipImport.split = index
                    }
                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("取消")
                        flat: true
                        onClicked: clipImport.path = ""
                    }
                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("开始导入")
                        highlighted: true
                        onClicked: clipImport.start()
                    }
                }

                SettingRow {
                    id: clipBackupRow

                    // A backup picked, waiting for the word to replace the history with it.
                    property string picked

                    readonly property bool busy: window.editor.clipboardBackupBusy || window.editor.clipboardImporting

                    title: qsTr("备份与还原")
                    keywords: qsTr("备份;还原;恢复;导出;迁移;换电脑;重装;ZIP;backup;restore;export").split(";")
                    description: window.editor.clipboardBackupMessage.length > 0
                                 ? window.editor.clipboardBackupMessage
                                 : qsTr("把全部历史、分组、备注和图片存成一个 ZIP 文件；换电脑或重装系统后从它还原")

                    FlatButton {
                        objectName: "clipboardBackupButton"
                        dpr: window.dpr
                        enabled: !clipBackupRow.busy
                        text: qsTr("备份…")
                        glyph: "\uEDE1" // Export
                        onClicked: {
                            clipBackupRow.picked = ""
                            window.editor.backUpClipboard()
                        }
                    }
                    FlatButton {
                        objectName: "clipboardRestoreButton"
                        dpr: window.dpr
                        enabled: !clipBackupRow.busy
                        text: qsTr("还原…")
                        glyph: "\uE81C" // History
                        onClicked: clipBackupRow.picked = window.editor.pickClipboardBackup()
                    }
                }

                SettingRow {
                    objectName: "clipboardRestoreConfirmRow"
                    sub: true
                    shown: clipBackupRow.picked.length > 0 && !clipBackupRow.busy
                    title: qsTr("用这份备份替换现在的剪贴板历史？")
                    description: qsTr("%1。现在的历史和分组会被它替换，Win顺 会重启一次").arg(clipBackupRow.picked.split(/[\\/]/).pop())

                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("取消")
                        flat: true
                        onClicked: clipBackupRow.picked = ""
                    }
                    FlatButton {
                        dpr: window.dpr
                        text: qsTr("还原并重启")
                        highlighted: true
                        onClicked: {
                            const picked = clipBackupRow.picked
                            clipBackupRow.picked = ""
                            window.editor.restoreClipboard(picked)
                        }
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
                        dpr: window.dpr
                        id: clearClipboardButton

                        property bool confirming: false

                        text: confirming ? qsTr("确定清除？再点一次") : qsTr("清除")
                        glyph: confirming ? "" : "" // Delete
                        enabled: window.editor.clipboardCount !== 0 && !window.editor.clipboardImporting && !window.editor.clipboardBackupBusy
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
            // </page clipboard>
                } }
            }

            PageSlot {
                id: pluginsSlot
                index: window.pluginsPage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({ pluginsSection: pluginsSection, pluginGallerySection: pluginGallerySection, pluginDevSection: pluginDevSection })
                    onResetForReopen: {
                        pluginsSection.resetForReopen()
                        pluginGallerySection.resetForReopen()
                        pluginDevSection.resetForReopen()
                    }
            // <page plugins>
            PluginsSection {
                id: pluginsSection
                onEditWebShortcut: (index, button) => window.editWebShortcut(index, button)
                page: window.pluginsPage
                width: parent.width
                plugins: window.editor.plugins
                editor: window.editor
                lockedElsewhere: pluginDevSection.lockedRows
                onDetailToggled: (open) => window.pluginDetailToggled(open, 0)
                onSettingRequested: (which) => {
                    if (which === "dropPie")
                        window.showPart(window.favoritesPage, "dropPieRow")
                    else
                        window.showPart(window.integrationPage, which === "folderMenu" ? "folderMenuRow"
                                        : which === "dialogBar" ? "dialogBarRow"
                                        : which === "opusTyping" ? "opusTypingRow"
                                        : which === "oneCommanderTyping" ? "oneCommanderTypingRow"
                                        : "explorerTypingRow")
                }
            }

            PluginGallerySection {
                id: pluginGallerySection
                page: window.pluginsPage
                tab: 1
                width: parent.width
                plugins: window.editor.plugins
                editor: window.editor
                onEditWebShortcut: (index, button) => window.editWebShortcut(index, button)
                onDetailToggled: (open) => window.pluginDetailToggled(open, 1)
                onShowProxySettings: window.goTo(window.part(window.generalPage, "proxyRow"))
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
            // </page plugins>
                } }
            }

            PageSlot {
                id: aiSlot
                index: window.aiPage
                content: Component { SettingsPage {
                    host: settingsContext
                    parts: ({ cliRow: aiCommandLineSection.cliRow })
            // <page ai>
            AiAccessSection {
                page: window.aiPage
                width: parent.width
                editor: window.editor
                lockedElsewhere: aiPrivacySection.lockedRows
            }

            AiCommandLineSection {
                id: aiCommandLineSection
                page: window.aiPage
                width: parent.width
                editor: window.editor
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
            // </page ai>
                } }
            }

            PageSlot {
                id: healthSlot
                index: window.healthPage
                content: Component { HealthPage {
                    host: settingsContext
                    editor: window.editor
                    pageIndex: window.healthPage
                    // Minimized it is still "visible": no readings then.
                    shown: window.visible && window.visibility !== Window.Minimized
                        && window.currentPage === window.healthPage && !window.searching
                } }
            }

        }
    }

    ListScrollBar { // the page's: widens under the mouse, drag the thumb or click the track (GitHub #229)
        objectName: "settingsScrollBar"
        dpr: window.dpr
        flickable: flick
        visible: flick.visible && flick.contentHeight > flick.height
        anchors.right: flick.right
        anchors.rightMargin: window.frame.resizeMargin(window.dpr) // clear of the resize edge, where a press sizes the window
        anchors.top: flick.top
        anchors.bottom: flick.bottom
    }

    Item { // bounds the edge-overscroll cue to the right-hand content pane
        anchors.left: nav.right
        anchors.right: parent.right
        anchors.top: window.warm ? titleBar.bottom : pageHeader.bottom
        anchors.bottom: parent.bottom
        z: 0.5
        Accessible.role: Accessible.Pane
        Accessible.ignored: !edgeScrollHint.visible

        EdgeScrollHint {
            id: edgeScrollHint
            objectName: "settingsEdgeScrollHint"
            anchors.fill: parent
            direction: edgeGate.direction
            progress: edgeGate.progress
            ready: edgeGate.readyToSwitch
            targetLabel: window.adjacentLabel(window.adjacentView(edgeGate.direction))
            warm: window.warm
            dpr: window.dpr
            onClicked: {
                const dir = edgeGate.direction
                if (dir !== 0)
                    window.stepAdjacentView(dir)
            }
        }
    }

    Loader { // outside the page's Flickable: always in the visible window
        id: webDialog
        z: 1
        anchors.fill: parent
        active: window.webEditing >= -1
        visible: active
        Component.onCompleted: window.frame.addPopup(webDialog)
        sourceComponent: WebShortcutForm {
            editor: window.editor
            index: window.webEditing
            // Copy the draft once. Saving rebuilds the list and its delegates.
            Component.onCompleted: {
                if (index >= 0)
                    shortcut = window.editor.webShortcuts[index]
            }
            onFinished: window.closeWebForm()
        }
    }

    UpdateDialog {
        id: updateDialog
        z: 1
        Accessible.role: Accessible.Dialog // closed: one element skipped whole (see nav)
        anchors.fill: parent
        updater: window.updater
        icon: window.frame.icon
        Component.onCompleted: window.frame.addPopup(updateDialog)
    }

    PluginReadmeDialog {
        id: readmeDialog
        z: 1
        Accessible.role: Accessible.Dialog
        anchors.fill: parent
        plugins: window.editor.plugins
        frame: window.frame
    }

    PluginNotesDialog {
        id: notesDialog
        z: 1
        Accessible.role: Accessible.Dialog
        anchors.fill: parent
        plugins: window.editor.plugins
        frame: window.frame
    }

    PluginDialog { // the 插件 page's questions: permissions, versions, removal
        id: pluginDialog
        z: 1
        Accessible.role: Accessible.Dialog
        anchors.fill: parent
        plugins: window.editor.plugins
        Component.onCompleted: window.frame.addPopup(pluginDialog)
    }

    DonateMoveDialog { // before the 捐赠 button goes to the 拾穗计划 page (GitHub #402)
        id: donateDialog
        z: 1
        Accessible.role: Accessible.Dialog
        anchors.fill: parent
        onDonate: window.editor.openUrl(Gleaning.donateUrl)
        onVisitSite: window.editor.openUrl(Gleaning.siteUrl)
        onConfirmed: window.editor.donateOnGleaningPage = true
        Component.onCompleted: window.frame.addPopup(donateDialog)
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

    UiSizeToast {
        id: uiSizeToast
        objectName: "settingsUiSizeToast"
        x: Theme.snap(nav.width + (window.width - nav.width - width) / 2, window.dpr)
        y: Theme.snap(window.height - height - 20, window.dpr)
        z: 500
        dpr: window.dpr
    }

    WindowEdge {
        id: windowEdge
        fill: Theme.page
    }
}
