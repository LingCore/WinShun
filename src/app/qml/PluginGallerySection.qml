pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → 插件 › 插件库: the plugins published in the WinShun-plugins
// repository (PluginGallery), each a PluginGalleryEntry with what it is here,
// a button to install or update it, and 设置 for any installed plugin (opening
// its own settings window or its PluginCard right here in place of the grid,
// with 返回插件库 to go back); a filter; where to share one's own.
// The list is fetched while the tab is on show (SettingsWindow binds
// `gallery.active`), never while the settings are searched. What the page's
// install says of the last one shows over the list, as on the 已安装 tab.
SettingsSection {
    id: gallerySection
    objectName: "pluginGallerySection"

    required property PluginSettings plugins
    property SettingsEditor editor: null
    readonly property PluginGallery gallery: plugins.gallery

    // An installed plugin opened right here from the 插件库 (in place of the
    // tile grid, as PluginsSection opens one on the 已安装 tab): empty for the grid.
    property string detailPluginId
    readonly property bool detailOpen: !listRow.searching && detailPluginId.length > 0
    signal detailToggled(bool open) // SettingsWindow: to the top, and back where the grid was
    onDetailOpenChanged: detailToggled(detailOpen)

    signal editWebShortcut(int index, Item button)
    signal showDeveloper()
    signal showProxySettings() // 设置网络代理: the row on 高级

    function open(pluginId) {
        if (plugins.hasPlugin(pluginId))
            detailPluginId = pluginId
    }
    function closeDetail() {
        detailPluginId = ""
    }
    function resetForReopen() {
        detailPluginId = ""
    }
    function pruneDetail() {
        if (detailPluginId.length > 0 && !plugins.hasPlugin(detailPluginId))
            detailPluginId = ""
    }
    function cardOf(pluginId) {
        const card = detailCardLoader.item ? detailCardLoader.item.activeCard : null
        return card && card.pluginId === pluginId ? card : null
    }

    Connections {
        target: gallerySection.plugins.list
        function onModelReset() { Qt.callLater(gallerySection.pruneDetail) }
        function onRowsRemoved() { Qt.callLater(gallerySection.pruneDetail) }
    }
    Connections {
        target: gallerySection.gallery
        function onUpdatesOnlyChanged() {
            if (gallerySection.gallery.updatesOnly)
                gallerySection.closeDetail()
        }
        function onHiddenOnlyChanged() {
            if (gallerySection.gallery.hiddenOnly)
                gallerySection.closeDetail()
        }
    }

    SettingRow { // what the install said: not seen on the 已安装 tab from here
        title: qsTr("刚才的结果")
        description: gallerySection.plugins.message
        shown: gallerySection.plugins.message.length > 0

        FlatButton {
            dpr: gallerySection.dpr
            text: qsTr("知道了")
            flat: true
            onClicked: gallerySection.plugins.clearMessage()
        }
    }

    SettingRow {
        id: listRow

        title: qsTr("插件库")
        tip: qsTr("只安装你需要的功能。下载后先核对文件，安装并启用时会先列出它要的权限，同意后即可使用。")
        keywords: qsTr("下载插件;安装插件;更新插件;商店;市场;store;gallery;download").split(";")
        // Filtered, how many of them are listed: "44 个插件" over one tile read as a fault.
        description: gallerySection.gallery.status.length > 0 && gallerySection.gallery.filter.trim().length > 0
                         && gallerySection.gallery.shownCount < gallerySection.gallery.count
                     ? qsTr("%1，筛选后显示 %2 个").arg(gallerySection.gallery.status).arg(gallerySection.gallery.shownCount)
                     : gallerySection.gallery.status
        bodyShown: gallerySection.detailOpen || gallerySection.gallery.count > 0 || gallerySection.gallery.problem.length > 0

        FlatButton {
            dpr: gallerySection.dpr
            objectName: "updateAllPlugins"
            visible: !gallerySection.detailOpen && (gallerySection.gallery.updatableCount > 0 || gallerySection.gallery.updatingAll)
            highlighted: true
            text: qsTr("全部更新")
            enabled: !gallerySection.gallery.downloading && !gallerySection.gallery.updatingAll
            onClicked: gallerySection.gallery.updateAll()
        }
        FlatButton {
            dpr: gallerySection.dpr
            visible: !gallerySection.detailOpen
            text: qsTr("刷新")
            glyph: "" // Refresh
            enabled: !gallerySection.gallery.fetching
            onClicked: gallerySection.gallery.refresh()
        }

        body: [
            FlatButton { // back from an opened plugin's card to the gallery grid
                objectName: "galleryPluginBack"
                visible: gallerySection.detailOpen
                dpr: gallerySection.dpr
                flat: true
                glyph: String.fromCharCode(0xE76B) // ChevronLeft
                text: qsTr("返回插件库")
                onClicked: gallerySection.closeDetail()
            },
            Loader { // the single installed plugin opened from here, loaded only while open
                id: detailCardLoader
                objectName: "galleryPluginDetail"
                visible: gallerySection.detailOpen
                active: gallerySection.detailOpen
                width: parent.width
                height: visible ? implicitHeight : 0

                sourceComponent: Item {
                    id: detailHost

                    property PluginCard activeCard: null
                    function refreshActiveCard() {
                        let found = null
                        for (let i = 0; i < detailRepeater.count; ++i) {
                            const s = detailRepeater.itemAt(i)
                            if (s && s.chosen && s.cardItem) {
                                found = s.cardItem
                                break
                            }
                        }
                        if (activeCard !== found)
                            activeCard = found
                    }

                    width: detailCardLoader.width
                    implicitHeight: activeCard ? activeCard.implicitHeight : 0

                    Repeater {
                        id: detailRepeater
                        model: gallerySection.plugins.list
                        onCountChanged: detailHost.refreshActiveCard()

                        delegate: Item {
                            id: slot

                            required property string pluginId
                            required property string name
                            required property string version
                            required property string author
                            required property string homepage
                            required property string feedback
                            required property string description
                            required property bool bundled
                            required property string icon
                            required property bool pluginEnabled
                            required property bool usable
                            required property string status
                            required property string tone
                            required property string error
                            required property bool resident
                            required property var keywords
                            required property var permissions
                            required property var settings
                            required property bool hasReadme
                            required property bool settingsWindow
                            required property bool webSearch
                            required property bool hasData
                            required property bool aiExpose
                            required property bool aiEnabled
                            required property string aiNote
                            required property string elevation
                            required property string elevationNote
                            required property string staged
                            required property string report
                            required property string reportTone
                            required property real progress
                            required property var reportActions
                            required property string elevationProblem

                            readonly property bool chosen: pluginId === gallerySection.detailPluginId
                            readonly property PluginCard cardItem: cardLoader.item as PluginCard

                            onChosenChanged: detailHost.refreshActiveCard()
                            onCardItemChanged: detailHost.refreshActiveCard()
                            Component.onDestruction: detailHost.refreshActiveCard()

                            visible: chosen
                            width: chosen ? detailHost.width : 0
                            height: chosen && cardItem ? cardItem.implicitHeight : 0

                            Loader {
                                id: cardLoader
                                width: slot.width
                                active: slot.chosen

                                sourceComponent: PluginCard {
                                    width: cardLoader.width
                                    editor: gallerySection.editor
                                    plugins: gallerySection.plugins
                                    animated: false
                                    loadedOnce: true
                                    expanded: true
                                    pluginId: slot.pluginId
                                    name: slot.name
                                    version: slot.version
                                    author: slot.author
                                    homepage: slot.homepage
                                    feedback: slot.feedback
                                    description: slot.description
                                    bundled: slot.bundled
                                    icon: slot.icon
                                    pluginEnabled: slot.pluginEnabled
                                    usable: slot.usable
                                    status: slot.status
                                    tone: slot.tone
                                    error: slot.error
                                    resident: slot.resident
                                    keywords: slot.keywords
                                    permissions: slot.permissions
                                    settings: slot.settings
                                    hasReadme: slot.hasReadme
                                    settingsWindow: slot.settingsWindow
                                    webSearch: slot.webSearch
                                    hasData: slot.hasData
                                    aiExpose: slot.aiExpose
                                    aiEnabled: slot.aiEnabled
                                    aiNote: slot.aiNote
                                    elevation: slot.elevation
                                    elevationNote: slot.elevationNote
                                    staged: slot.staged
                                    report: slot.report
                                    reportTone: slot.reportTone
                                    progress: slot.progress
                                    reportActions: slot.reportActions
                                    elevationProblem: slot.elevationProblem
                                    onEditWebShortcut: (index, button) => gallerySection.editWebShortcut(index, button)
                                    onExpandRequested: (on) => { if (!on) gallerySection.closeDetail() }
                                }
                            }
                        }
                    }
                }
            },
            Text { // why the list could not be had (the one kept from before, if any, shows below)
                visible: !gallerySection.detailOpen && gallerySection.gallery.problem.length > 0
                width: parent.width
                height: Theme.snapUp(implicitHeight, gallerySection.dpr) // the filter box below on whole pixels
                text: gallerySection.gallery.problem
                textFormat: Text.PlainText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            },
            FlatButton { // GitHub could not be reached: the way round it (GitHub #84)
                dpr: gallerySection.dpr
                visible: !gallerySection.detailOpen && gallerySection.gallery.problemIsConnection
                text: qsTr("设置网络代理…")
                onClicked: gallerySection.showProxySettings()
            },
            Text { // 全部更新: how far it is, then what came of it
                visible: !gallerySection.detailOpen && gallerySection.gallery.updateStatus.length > 0
                width: parent.width
                height: Theme.snapUp(implicitHeight, gallerySection.dpr)
                text: gallerySection.gallery.updateStatus
                textFormat: Text.PlainText
                color: gallerySection.gallery.updateFailed ? Theme.danger : Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            },
            Row {
                visible: !gallerySection.detailOpen && gallerySection.gallery.count > 0
                width: parent.width
                height: 32
                spacing: 8

                InputBox {
                    id: filterBox
                    width: Math.min(parent.width - (updatesChip.visible ? updatesChip.width + parent.spacing : 0)
                                    - (hiddenChip.visible ? hiddenChip.width + parent.spacing : 0)
                                    - (hiddenButton.visible ? hiddenButton.width + parent.spacing : 0), 320)
                    implicitHeight: 32
                    clearable: true
                    placeholder: qsTr("筛选：名称、关键字、说明")
                    text: gallerySection.gallery.filter
                    onTextChanged: gallerySection.gallery.filter = text
                }
                Chip {
                    id: updatesChip
                    visible: gallerySection.gallery.updatesOnly
                    text: qsTr("只看可更新")
                    onRemoveClicked: gallerySection.gallery.updatesOnly = false
                }
                // 不感兴趣 (GitHub #442): how many were put away; a click lists
                // those alone, where each can be brought back.
                Chip {
                    id: hiddenChip
                    objectName: "galleryHiddenChip"
                    visible: gallerySection.gallery.hiddenOnly
                    text: qsTr("只看不感兴趣的")
                    onRemoveClicked: gallerySection.gallery.hiddenOnly = false
                }
                FlatButton {
                    id: hiddenButton
                    objectName: "galleryHiddenButton"
                    dpr: gallerySection.dpr
                    visible: gallerySection.gallery.hiddenCount > 0 && !gallerySection.gallery.hiddenOnly
                    implicitHeight: 32
                    flat: true
                    glyph: "ph:eye-slash"
                    text: qsTr("不感兴趣的 %1 个").arg(gallerySection.gallery.hiddenCount)
                    onClicked: gallerySection.gallery.hiddenOnly = true
                }
            },
            // The entries, some 4 ms each: made a few at a time between frames
            // while the tab is away (SettingsWindow reads the list from 已安装);
            // the rest at once when the tab comes on show meanwhile, or when the
            // list first comes with the tab on show. Kept alive while a plugin's
            // card is open so closing it returns to the grid without rebuilding.
            Loader {
                width: parent.width
                active: gallerySection.gallery.count > 0
                visible: !gallerySection.detailOpen && active
                height: visible ? implicitHeight : 0
                asynchronous: !gallerySection.visible

                sourceComponent: TileFlow {
                    id: galleryTiles
                    dpr: gallerySection.dpr

                    Repeater {
                        model: gallerySection.gallery.list

                        delegate: PluginGalleryEntry {
                            dpr: gallerySection.dpr
                            gallery: gallerySection.gallery
                            onShowInstalled: gallerySection.open(pluginId)
                        }
                    }
                }
            },
            Text {
                visible: !gallerySection.detailOpen && gallerySection.gallery.count > 0 && gallerySection.gallery.shownCount === 0
                width: parent.width
                text: filterBox.text.trim().length > 0 ? qsTr("没有找到“%1”").arg(filterBox.text.trim())
                    : gallerySection.gallery.updatesOnly ? qsTr("插件都是最新的")
                    : gallerySection.gallery.hiddenOnly ? qsTr("这里没有可更新的") // both views at once
                    : gallerySection.gallery.hiddenCount > 0 ? qsTr("剩下的插件都标了不感兴趣")
                    : qsTr("没有找到“%1”").arg(filterBox.text.trim())
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        ]
    }

    SettingRow { // one row for making a plugin and sharing it
        shown: !gallerySection.detailOpen
        title: qsTr("自己做插件")
        tip: qsTr("从能运行的模板开始，也可以把开发提示和模板一起交给 AI。创建后仍由你决定是否启用插件。做了好用的插件，可以投稿到插件库，给大家用。")
        keywords: qsTr("投稿;发布;提交;开发;share;publish;submit;AI;模板;创建;贡献;Python;PowerShell").split(";")
        description: qsTr("从模板开始，做好可以投稿")

        FlatButton {
            dpr: gallerySection.dpr
            flat: true
            text: qsTr("怎么投稿")
            glyph: "" // Link
            onClicked: gallerySection.gallery.openRepository()
        }
        FlatButton {
            dpr: gallerySection.dpr
            text: qsTr("复制 AI 开发提示")
            onClicked: gallerySection.plugins.copyDevelopmentPrompt()
        }
        FlatButton {
            dpr: gallerySection.dpr
            text: qsTr("创建插件")
            onClicked: gallerySection.showDeveloper()
        }
    }
}
