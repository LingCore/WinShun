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

    note: qsTr("Win顺 插件库里的插件，下载后先核对文件再安装。装好的插件默认关着，打开时会先列出它要的权限。")

    SettingRow { // what the install said: not seen on the 已安装 tab from here
        title: qsTr("刚才的结果")
        description: gallerySection.plugins.message
        shown: gallerySection.plugins.message.length > 0

        FlatButton {
            text: qsTr("知道了")
            flat: true
            onClicked: gallerySection.plugins.clearMessage()
        }
    }

    SettingRow {
        id: listRow

        title: qsTr("插件库")
        keywords: qsTr("下载插件;安装插件;更新插件;商店;市场;store;gallery;download").split(";")
        description: gallerySection.gallery.status
        bodyShown: gallerySection.gallery.count > 0 || gallerySection.gallery.problem.length > 0

        FlatButton {
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
                visible: gallerySection.gallery.problemIsConnection
                text: qsTr("设置网络代理…")
                onClicked: gallerySection.showProxySettings()
            },
            InputBox {
                id: filterBox
                visible: gallerySection.gallery.count > 0
                width: Math.min(parent.width, 320)
                implicitHeight: 32
                placeholder: qsTr("筛选：名称、关键字、说明")
                onTextChanged: gallerySection.gallery.filter = text
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
                text: qsTr("没有找到“%1”").arg(filterBox.text.trim())
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        ]
    }

    SettingRow {
        title: qsTr("分享你的插件")
        keywords: qsTr("投稿;发布;提交;开发;share;publish;submit").split(";")
        description: qsTr("做了好用的插件？可以投稿到插件库，给大家用")

        FlatButton {
            text: qsTr("看看怎么投稿")
            glyph: "" // Link
            onClicked: gallerySection.gallery.openRepository()
        }
    }

    SettingRow {
        title: qsTr("做一个自己的插件")
        keywords: qsTr("AI;开发;模板;创建;贡献;Python;PowerShell").split(";")
        description: qsTr("从能运行的模板开始，也可以把开发提示和模板一起交给 AI。创建后仍由你决定是否启用插件。")
        FlatButton {
            text: qsTr("复制 AI 开发提示")
            onClicked: gallerySection.plugins.copyDevelopmentPrompt()
        }
        FlatButton {
            text: qsTr("创建插件")
            highlighted: true
            onClicked: gallerySection.showDeveloper()
        }
    }
}
