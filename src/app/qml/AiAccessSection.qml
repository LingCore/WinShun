pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → AI 助手 › 权限: what AI clients may do through Win顺 (the MCP
// server, src/mcp): the switch, opening things, the clipboard history, the
// plugins. The folders they never see and the log of their calls are on the
// 隐私与记录 tab (AiPrivacySection).
SettingsSection {
    id: aiSection

    title: qsTr("MCP 与命令行访问权限")
    glyph: "\uE8D7"
    required property SettingsEditor editor
    // Rows on other tabs shown only while AI may connect (AiPrivacySection.lockedRows).
    property var lockedElsewhere: []

    note: qsTr("Claude、Cursor、VS Code 等 AI 助手通过 MCP（Model Context Protocol）连接 Win顺，就能搜文件名、文件里的文字和最近改过的文件：和你在搜索框里一样快，拼音也行。能运行命令的 AI 助手和你自己也可以在终端里用命令行 ws。默认只读，不改动任何文件。")

    SettingRow {
        id: enableRow

        title: qsTr("允许 AI 助手和命令行连接")
        keywords: qsTr("MCP;AI;Claude;Cursor;智能;助手;大模型;人工智能;命令行;终端;ws;command line;cli;terminal;shell;powershell;cmd").split(";")
        description: !aiSection.editor.mcp ? qsTr("关着时 AI 客户端和命令行 ws 都连不上 Win顺")
                   : aiSection.editor.mcpStatus.length > 0 ? aiSection.editor.mcpStatus
                   : qsTr("AI 客户端启动 WinShunMcp.exe 就能连上；在终端里输入 ws 也行")
        unlocks: [openRow, clipboardRow, screenshotRow, settingsRow, pluginsRow, actionsRow, ...aiSection.lockedElsewhere]

        ToggleSwitch {
            checked: aiSection.editor.mcp
            onToggled: (on) => aiSection.editor.mcp = on
        }
    }

    SettingRow {
        id: openRow
        sub: true

        title: qsTr("允许 AI 打开文件和文件夹")
        keywords: qsTr("打开;文件夹;显示;open;AI").split(";")
        description: qsTr("允许 AI 用默认程序打开文件、在文件管理器中定位显示")
        tip: qsTr("只管通过 MCP 连接的 AI 客户端：用默认程序打开文件、在文件管理器里显示；以普通权限打开，客户端每次会先问你。命令行的 ws open 按你自己的权限打开，不受这个开关管")
        shown: aiSection.editor.mcp

        ToggleSwitch {
            checked: aiSection.editor.mcpAllowOpen
            onToggled: (on) => aiSection.editor.mcpAllowOpen = on
        }
    }

    SettingRow {
        id: clipboardRow
        sub: true

        title: qsTr("允许 AI 读取剪贴板历史")
        keywords: qsTr("剪贴板;剪切板;隐私;clipboard;AI").split(";")
        description: qsTr("允许 AI 读取复制过的文本、链接与文件路径历史")
        tip: qsTr("AI 能看到复制过的文字、链接和文件名，图片只告诉它大小；Win顺 的截图还能拿到识别出的文字和图片（截图开着时）。密码管理器复制的本来就不记录")
        shown: aiSection.editor.mcp

        ToggleSwitch {
            checked: aiSection.editor.mcpClipboard
            onToggled: (on) => aiSection.editor.mcpClipboard = on
        }
    }

    SettingRow { // take_screenshot (App::confirmScreenshot)
        id: screenshotRow
        sub: true

        title: qsTr("允许 AI 请求截屏")
        keywords: qsTr("截图;截屏;屏幕;screenshot;AI").split(";")
        description: qsTr("允许 AI 请求截取指定屏幕区域并返回图像")
        tip: qsTr("AI 每次要截屏，屏幕上都会弹出确认，写明是谁、截哪块屏幕；你点“允许”才截，拒绝或 30 秒不理就不截。截下的图不进剪贴板历史")
        shown: aiSection.editor.mcp

        ToggleSwitch {
            checked: aiSection.editor.mcpScreenshot
            onToggled: (on) => aiSection.editor.mcpScreenshot = on
        }
    }

    SettingRow { // get_settings / change_settings (App::confirmSettingsChange)
        id: settingsRow
        sub: true

        readonly property var modes: ["ask", "auto", "off"]

        title: qsTr("允许 AI 修改设置")
        keywords: qsTr("设置;配置;修改;改设置;帮我设置;setting;config;change;AI").split(";")
        description: {
            switch (aiSection.editor.mcpSettings) {
            case "auto":
                return qsTr("AI 改完会在屏幕右下角告诉你，几秒内能撤销。涉及安全的几项（开机自启、索引方式、网站搜索）仍会先问你")
            case "off":
                return qsTr("AI 看不到也改不了 Win顺 的设置，只能带你打开设置页")
            default:
                return qsTr("AI 可以帮你开关功能、调整参数。每次修改前都会弹出确认，写明改什么、为什么；涉及安全的几项总会先问你")
            }
        }
        shown: aiSection.editor.mcp

        ScopeTabs {
            labels: [qsTr("每次先问我"), qsTr("直接修改"), qsTr("不允许")]
            current: Math.max(0, settingsRow.modes.indexOf(aiSection.editor.mcpSettings))
            onActivated: (index) => aiSection.editor.mcpSettings = settingsRow.modes[index]
        }
    }

    SettingRow { // the plugins' commands as tools (plugin::AiBridge)
        id: pluginsRow
        sub: true

        title: qsTr("AI 可以使用插件")
        keywords: qsTr("插件;工具;plugin;tool;AI").split(";")
        description: qsTr("将支持 AI 的插件命令作为只读工具开放给 AI 查询")
        tip: qsTr("标明可给 AI 用的插件，每个命令成为一个工具（plugin_插件_命令）：AI 只能查询，像你在搜索框里输入那样拿到结果；每个插件的卡片上还有一个开关")
        shown: aiSection.editor.mcp
        unlocks: [actionsRow]

        ToggleSwitch {
            checked: aiSection.editor.mcpPlugins
            onToggled: (on) => aiSection.editor.mcpPlugins = on
        }
    }

    SettingRow {
        id: actionsRow
        sub: true

        title: qsTr("允许 AI 执行插件动作")
        keywords: qsTr("插件;动作;执行;结束;plugin;action;AI").split(";")
        description: qsTr("允许 AI 执行结束进程、复制等具有实际副作用的插件动作")
        tip: qsTr("再多一个工具（plugin_插件_命令_run）做 Enter 做的事：结束进程、打开、复制这类会改动东西的操作；AI 客户端每次会先问你")
        shown: aiSection.editor.mcp && aiSection.editor.mcpPlugins

        ToggleSwitch {
            checked: aiSection.editor.mcpPluginActions
            onToggled: (on) => aiSection.editor.mcpPluginActions = on
        }
    }
}
