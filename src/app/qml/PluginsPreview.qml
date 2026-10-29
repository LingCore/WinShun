pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The 插件 page on its own (or, with `aiPage`, the AI 助手 page), for the
// offscreen rendering Preview.cpp does (--preview-plugins): the sections
// (its three tabs one under another) in a column shaped like the settings
// window's page column (what SettingsSection reads from its parent), over
// the page's colour, and the permission dialog over it when asked. Never
// shown to anyone: a window only so that it can be grabbed.
Window {
    id: preview

    required property PluginSettings plugins
    required property SettingsEditor editor
    property bool dialogShown: false
    property bool aiPage: false

    width: 900
    height: page.implicitHeight + 48
    color: Theme.page
    flags: Qt.Tool | Qt.FramelessWindowHint

    SettingsSearch {
        id: settingsSearch
    }

    Column {
        id: page

        readonly property SettingsSearch search: settingsSearch
        readonly property int currentPage: 0
        readonly property int currentTab: 0
        readonly property var pages: [{ title: preview.aiPage ? qsTr("AI 助手") : qsTr("插件"), glyph: preview.aiPage ? "" : "" }]
        readonly property var pageKeywords: [""]
        function openPage(index, tab) {}

        x: 24
        y: 24
        width: parent.width - 48
        spacing: 20

        Text {
            text: preview.aiPage ? qsTr("AI 助手") : qsTr("插件")
            color: Theme.text
            font.pixelSize: Theme.fontDisplay
            font.weight: Font.DemiBold
        }

        AiAccessSection {
            page: 0
            width: parent.width
            visible: preview.aiPage
            editor: preview.editor
        }

        PluginsSection {
            id: pluginsSection
            page: 0
            width: parent.width
            visible: !preview.aiPage
            plugins: preview.plugins
        }

        PluginGallerySection {
            page: 0
            width: parent.width
            visible: !preview.aiPage
            plugins: preview.plugins
        }

        PluginDevSection {
            page: 0
            width: parent.width
            visible: !preview.aiPage
            plugins: preview.plugins
        }
    }

    PluginDialog {
        id: dialog
        z: 1
        anchors.fill: parent
        plugins: preview.plugins
        animated: false
    }

    Component.onCompleted: {
        if (!preview.aiPage) {
            pluginsSection.expandAt(1) // one card unfolded, its permissions with it
            pluginsSection.expandAt(4) // ... and the strict-sandbox one: its first permission wraps
        }
        if (preview.dialogShown && !preview.aiPage) {
            dialog.ask("permissions", {
                id: "lingcore.kill", name: qsTr("结束进程"), version: "1.2.0", author: "LingCore",
                homepage: "https://github.com/LingCore/WinShun", source: qsTr("你的插件文件夹"),
                permissions: [ // the strict sandbox's sentence: the longest, wrapping onto a second line
                    { name: "", text: qsTr("在严格沙箱里运行：低完整性，不是管理员，写不了你的文件（只能写它自己的数据文件夹）；内存上限 %1 MB").arg(64), enforced: true, note: "" },
                    { name: "processes", text: qsTr("启动程序（起子进程，或打开程序、非网页链接）"), enforced: true, note: "" },
                    { name: "network", text: qsTr("联网"), enforced: false, note: qsTr("Win顺 不能阻止它联网") }
                ],
                more: true, newNames: ["network"]
            })
        }
    }
}
