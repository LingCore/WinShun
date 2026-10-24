pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → AI 助手 › 权限 › 命令行: ws, the search command for a terminal
// (src/cli), which AI assistants that can run commands use too. It shares the
// connection switch above; here is whether it can be run (ws.exe there, its
// folder on the PATH, no other ws in front) and the one button that sets it
// up (CliSetup). On the first tab, not next to the clients: the other tabs
// are gone while the switch is off, and this is where it gets turned on.
SettingsSection {
    id: cliSection

    required property SettingsEditor editor

    readonly property var cli: editor.cli
    // Not `state`: that is the Item's own.
    readonly property var cliState: cli.state
    readonly property bool exeFound: cliState.exeFound === true
    readonly property bool needsAction: !editor.mcp || (exeFound && !cliState.onPath)
    readonly property bool ready: editor.mcp && exeFound && cliState.onPath === true && cliState.shadow.length === 0
    readonly property string stateText: {
        const s = cliSection.cliState
        if (!s.checked)
            return qsTr("正在检查…")
        const lines = []
        if (!s.exeFound)
            lines.push(qsTr("没找到 ws.exe（应在 %1），重新安装 Win顺 就有了").arg(s.bin))
        else if (s.where === "system")
            lines.push(qsTr("已在系统的 PATH 里（安装程序添加，由它管理）：%1").arg(s.bin))
        else if (s.where === "user")
            lines.push((s.removable ? qsTr("已在你的 PATH 里（Win顺 加的）：%1") : qsTr("已在你的 PATH 里：%1")).arg(s.bin))
        else
            lines.push(qsTr("ws.exe 在 %1，这个文件夹还不在 PATH 里：终端里输入 ws 会找不到").arg(s.bin))
        if (s.exeFound && s.shadow.length > 0) // its path on a line of its own: it broke in the middle of "C:\Users"
            lines.push(qsTr("注意：PATH 里已有另一个叫 ws 的程序，终端里会先运行它：") + "\n" + s.shadow)
        if (!cliSection.editor.mcp)
            lines.push(qsTr("上面的开关关着：ws 连不上 Win顺"))
        return lines.join("\n")
    }

    title: qsTr("命令行")
    note: qsTr("在终端里输入 ws，就能搜文件名、搜文件里的文字、读出 Word 和 PDF 的文字。能运行命令的 AI 助手（Claude Code 这类）用它比 MCP 更省上下文。和 MCP 共用上面的开关。")

    SettingRow {
        title: qsTr("ws 命令")
        keywords: qsTr("命令行;终端;ws;command line;cli;terminal;shell;PowerShell;cmd;PATH;环境变量;启用").split(";")
        description: cliSection.stateText

        Glyph {
            visible: cliSection.ready
            anchors.verticalCenter: parent.verticalCenter
            glyph: "" // CheckMark
            size: 20
            color: Theme.accent
        }
        FlatButton {
            visible: cliSection.needsAction
            enabled: !cliSection.cli.busy
            text: qsTr("启用命令行")
            glyph: "" // CommandPrompt
            highlighted: true
            onClicked: cliSection.cli.enable()
        }
        FlatButton {
            visible: cliSection.cliState.removable === true
            enabled: !cliSection.cli.busy
            text: qsTr("从 PATH 移除")
            flat: true
            onClicked: cliSection.cli.removeFromPath()
        }
    }

    SettingRow {
        title: qsTr("试一试")
        keywords: qsTr("示例;例子;试试;命令行;ws;example;try;terminal;终端").split(";")
        description: qsTr("新打开的终端才认得新的 PATH，已经开着的不行（VS Code 的终端要重开 VS Code）。在终端里输入：")
                     + "\n" + cliSection.cli.example

        FlatButton {
            text: qsTr("复制命令")
            glyph: "" // Copy
            onClicked: cliSection.editor.copyText(cliSection.cli.example)
        }
    }

    SettingRow {
        title: qsTr("刚才的结果")
        description: cliSection.cli.message
        shown: cliSection.cli.message.length > 0

        FlatButton {
            text: qsTr("知道了")
            flat: true
            onClicked: cliSection.cli.clearMessage()
        }
    }
}
