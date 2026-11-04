pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → AI 助手 › 连接客户端: the AI clients found on this PC, each connected to
// Win顺 with one click (McpClients writes WinShunMcp.exe into its
// configuration), and the configuration to paste into any other. Claude Code
// can also get a Skill that teaches it the command line ws (AiCommandLineSection).
SettingsSection {
    id: clientsSection

    required property SettingsEditor editor

    readonly property bool relayFound: editor.mcpRelayPath.length > 0
    // ws, for the Skill: its exe is there, and its folder on the PATH.
    readonly property bool cliFound: editor.cli.state.exeFound === true
    readonly property bool cliOnPath: editor.cli.state.onPath === true
    // Whether the PATH has been looked at yet: not "off it" before that.
    readonly property bool cliChecked: editor.cli.state.checked === true

    // The Skill file may have been deleted (or made) meanwhile: looked at when this shows, the
    // one path only, and not while the settings are searched (it shows and hides with each letter).
    onVisibleChanged: if (visible && !clientsSection.searching) clientsSection.editor.refreshSkillState()

    note: relayFound ? qsTr("点“连接”就写进那个客户端的配置（原文件先备份成 .bak）。装了的客户端才列出来。Claude Code 还可以“装 Skill”：让它在终端里用命令行 ws，比 MCP 更省上下文。")
                     : qsTr("没找到 WinShunMcp.exe，重新安装 Win顺 就有了。")

    Repeater {
        model: clientsSection.editor.mcpClients

        delegate: SettingRow {
            id: clientRow

            required property var modelData
            readonly property bool isClaudeCode: modelData.id === "claude-code"
            // Claude Code's Skill: below the row, across all of it (three buttons leave the
            // text beside them a few characters wide in a narrow window, and a path in it
            // broke anywhere).
            readonly property string skillText: {
                if (!isClaudeCode)
                    return ""
                const lines = []
                if (modelData.skillInstalled)
                    lines.push(qsTr("已装 Skill · %1").arg(modelData.skillPath))
                if (!clientsSection.cliFound)
                    lines.push(qsTr("没找到 ws.exe，装不了 Skill；重新安装 Win顺 就有了"))
                else if (clientsSection.cliChecked && !clientsSection.cliOnPath)
                    lines.push(qsTr("ws 还没加进 PATH：Skill 里写着 ws.exe 的完整路径，照样能用；建议先在“权限”页的“命令行”里启用"))
                return lines.join("\n")
            }

            title: modelData.name
            keywords: qsTr("连接;配置;MCP;AI;Skill;%1").arg(modelData.name).split(";")
            description: modelData.connected ? qsTr("已连接 · %1").arg(modelData.configPath) : modelData.note
            shown: modelData.installed && clientsSection.editor.mcp
            bodyShown: skillText.length > 0
            body: Text {
                width: parent.width
                text: clientRow.skillText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }

            Glyph {
                visible: clientRow.modelData.connected
                anchors.verticalCenter: parent.verticalCenter
                glyph: "" // CheckMark
                size: 20
                color: Theme.accent
            }
            FlatButton {
                dpr: clientsSection.dpr
                text: clientRow.modelData.connected ? qsTr("重新连接") : qsTr("连接")
                glyph: "" // Link
                highlighted: !clientRow.modelData.connected
                enabled: clientsSection.relayFound
                onClicked: clientsSection.editor.connectMcpClient(clientRow.modelData.id)
            }
            FlatButton { // no icon, like 复制 JSON: three buttons with icons leave the text beside them a column at the window's narrowest
                visible: clientRow.isClaudeCode
                text: clientRow.modelData.skillInstalled ? qsTr("重装 Skill") : qsTr("装 Skill")
                enabled: clientsSection.cliFound && !clientsSection.editor.mcpSkillBusy // one ws at a time
                onClicked: clientsSection.editor.installMcpSkill(clientRow.modelData.id)
            }
            FlatButton {
                dpr: clientsSection.dpr
                text: clientRow.isClaudeCode ? qsTr("复制命令") : qsTr("复制配置")
                glyph: "" // Copy
                enabled: clientsSection.relayFound
                onClicked: clientsSection.editor.copyMcpSnippet(clientRow.modelData.id)
            }
        }
    }

    SettingRow {
        title: qsTr("其他支持 MCP 的客户端")
        keywords: qsTr("其他;配置;stdio;MCP;AI;JSON").split(";")
        description: qsTr("把 WinShunMcp.exe 加为 stdio 类型的 MCP 服务器，不需要参数：") + "\n" + clientsSection.editor.mcpRelayPath
        shown: clientsSection.editor.mcp

        FlatButton {
            dpr: clientsSection.dpr
            text: qsTr("复制路径")
            glyph: "" // Copy
            enabled: clientsSection.relayFound
            onClicked: clientsSection.editor.copyText(clientsSection.editor.mcpRelayPath)
        }
        FlatButton {
            dpr: clientsSection.dpr
            text: qsTr("复制 JSON")
            enabled: clientsSection.relayFound
            onClicked: clientsSection.editor.copyMcpSnippet("")
        }
    }

    SettingRow {
        title: qsTr("刚才的结果")
        description: clientsSection.editor.mcpMessage
        shown: clientsSection.editor.mcp && clientsSection.editor.mcpMessage.length > 0

        FlatButton {
            dpr: clientsSection.dpr
            text: qsTr("知道了")
            flat: true
            onClicked: clientsSection.editor.clearMcpMessage()
        }
    }
}
