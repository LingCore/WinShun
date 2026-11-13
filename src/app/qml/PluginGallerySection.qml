pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → 插件 › 插件库: the plugins published in the WinShun-plugins
// repository (PluginGallery), each a PluginGalleryEntry with what it is here
// and a button to install or update it; a filter; where to share one's own.
// The list is fetched while the tab is on show (SettingsWindow binds
// `gallery.active`), never while the settings are searched. What the page's
// install says of the last one shows over the list, as on the 已安装 tab.
SettingsSection {
    id: gallerySection

    required property PluginSettings plugins
    readonly property PluginGallery gallery: plugins.gallery

    signal showInstalled(string pluginId) // 查看: its card on the 已安装 tab
    signal showDeveloper()
    signal showProxySettings() // 设置网络代理: the row on 高级


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
        description: gallerySection.gallery.status
        bodyShown: gallerySection.gallery.count > 0 || gallerySection.gallery.problem.length > 0

        FlatButton {
            dpr: gallerySection.dpr
            objectName: "updateAllPlugins"
            visible: gallerySection.gallery.updatableCount > 0 || gallerySection.gallery.updatingAll
            highlighted: true
            text: qsTr("全部更新")
            enabled: !gallerySection.gallery.downloading && !gallerySection.gallery.updatingAll
            onClicked: gallerySection.gallery.updateAll()
        }
        FlatButton {
            dpr: gallerySection.dpr
            text: qsTr("刷新")
            glyph: "" // Refresh
            enabled: !gallerySection.gallery.fetching
            onClicked: gallerySection.gallery.refresh()
        }

        body: [
            Text { // why the list could not be had (the one kept from before, if any, shows below)
                visible: gallerySection.gallery.problem.length > 0
                width: parent.width
                height: Theme.snapUp(implicitHeight, Screen.devicePixelRatio) // the filter box below on whole pixels
                text: gallerySection.gallery.problem
                textFormat: Text.PlainText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            },
            FlatButton { // GitHub could not be reached: the way round it (GitHub #84)
                dpr: gallerySection.dpr
                visible: gallerySection.gallery.problemIsConnection
                text: qsTr("设置网络代理…")
                onClicked: gallerySection.showProxySettings()
            },
            Text { // 全部更新: how far it is, then what came of it
                visible: gallerySection.gallery.updateStatus.length > 0
                width: parent.width
                height: Theme.snapUp(implicitHeight, Screen.devicePixelRatio)
                text: gallerySection.gallery.updateStatus
                textFormat: Text.PlainText
                color: gallerySection.gallery.updateFailed ? Theme.danger : Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            },
            Row {
                visible: gallerySection.gallery.count > 0
                width: parent.width
                height: 32
                spacing: 8

                InputBox {
                    id: filterBox
                    width: Math.min(parent.width - (updatesChip.visible ? updatesChip.width + parent.spacing : 0), 320)
                    implicitHeight: 32
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
            },
            Column {
                width: parent.width
                spacing: 8

                Repeater {
                    model: gallerySection.gallery.list

                    delegate: PluginGalleryEntry {
                        width: parent.width
                        gallery: gallerySection.gallery
                        onShowInstalled: gallerySection.showInstalled(pluginId)
                    }
                }
            },
            Text {
                visible: gallerySection.gallery.count > 0 && gallerySection.gallery.shownCount === 0
                width: parent.width
                text: gallerySection.gallery.updatesOnly && filterBox.text.trim().length === 0 ? qsTr("插件都是最新的")
                    : qsTr("没有找到“%1”").arg(filterBox.text.trim())
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        ]
    }

    SettingRow { // one row for making a plugin and sharing it
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
