pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A result row's buttons (ResultRow): open its settings page (a switch's),
// run as administrator, reveal, open in a terminal, copy, copy the path,
// delete (click twice). Not made in every row: the list keeps a few sets
// (RowActionsPool) and lends one to a row while it shows them. Lent to
// another row it only takes that row's state: nothing is made, the style's
// surfaces and the drawn copy-path icon included.
Item {
    id: bar

    property ResultRow host: null // the row it is lent to; null: put away
    property real dpr: 1
    property bool deleteButton: true // the settings': off, no row has one (GitHub #316)

    // For the row: the room the buttons take, how many show, and the one the
    // mouse rests on (the row's HoverTip, one for its buttons and its date).
    readonly property real actionsWidth: actions.width
    readonly property int count: actions.visibleChildren.length
    readonly property RowAction hovered: switchPageAction.tipWanted ? switchPageAction
                                       : adminAction.tipWanted ? adminAction
                                       : revealAction.tipWanted ? revealAction
                                       : terminalAction.tipWanted ? terminalAction
                                       : copyAction.tipWanted ? copyAction
                                       : copyPathAction.tipWanted ? copyPathAction
                                       : deleteAction.tipWanted ? deleteAction : null

    // Put in the row's mouse area by the pool, so that hovering a button still hovers the row.
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    visible: host !== null

    // The style's capsule round the buttons (Theme.actionCapsule; none in fluent).
    Loader {
        anchors.fill: actions
        anchors.margins: -Theme.capsulePadding
        active: Theme.actionCapsule !== null
        visible: bar.count > 0
        sourceComponent: Hook {
            part: Theme.actionCapsule
            count: bar.count
            role: "rowAction"
            dpr: bar.dpr
        }
    }

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: bar.host ? bar.host.actionsRightMargin : 0
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        RowAction {
            id: switchPageAction
            dpr: bar.dpr
            shared: true
            visible: bar.host !== null && bar.host.switchReady
            glyph: "" // Settings (gear)
            tip: qsTranslate("ResultRow", "打开设置")
            tipShortcut: "Ctrl+Enter"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.Reveal)
        }
        RowAction {
            id: adminAction
            objectName: "adminAction"
            dpr: bar.dpr
            shared: true
            visible: bar.host !== null && bar.host.elevatable && !bar.host.switchReady
            glyph: "" // Admin (shield)
            tip: qsTranslate("ResultRow", "以管理员身份运行")
            tipShortcut: "Ctrl+Shift+Enter"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.RunAsAdmin)
        }
        RowAction {
            id: revealAction
            dpr: bar.dpr
            shared: true
            visible: bar.host !== null && !bar.host.switchReady && (!bar.host.isApp || bar.host.revealable)
            glyph: "" // OpenFolder
            tip: bar.host !== null && bar.host.packagedApp ? qsTranslate("ResultRow", "打开安装文件夹") : qsTranslate("ResultRow", "打开所在位置")
            tipShortcut: "Ctrl+Enter"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.Reveal)
        }
        RowAction {
            id: terminalAction
            dpr: bar.dpr
            shared: true
            visible: bar.host !== null && !bar.host.isApp // a file's or folder's: its folder in cmd
            glyph: "" // CommandPrompt
            tip: qsTranslate("ResultRow", "在终端中打开")
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.OpenTerminal)
        }
        RowAction {
            id: copyAction
            dpr: bar.dpr
            shared: true
            // A desktop app copies its program; a packaged one has none.
            visible: bar.host !== null && bar.host.copyable && !bar.host.switchReady
            glyph: "" // Copy
            tip: qsTranslate("ResultRow", "复制")
            tipShortcut: "Ctrl+C"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.CopyItem)
        }
        RowAction {
            id: copyPathAction
            objectName: "copyPathAction"
            readonly property ResultRow host: bar.host
            dpr: bar.dpr
            shared: true
            visible: host !== null && !host.settingsAction && !host.switchReady && (!host.plugin || host.pluginCopies)
            // Copy; else drawn by Glyph: a box holding "\\..".
            glyph: host !== null && (host.plugin || host.windowResult) ? "" : "copyPath"
            tip: host === null ? ""
                : host.windowResult ? qsTranslate("ResultRow", "复制窗口标题")
                : host.plugin ? qsTranslate("ResultRow", "复制")
                : host.web ? qsTranslate("ResultRow", "复制网址")
                : host.place ? qsTranslate("ResultRow", "复制打开命令")
                : qsTranslate("ResultRow", "复制完整路径")
            tipShortcut: host !== null && host.plugin ? "Ctrl+C" : "Ctrl+Shift+C"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.CopyPath)
        }
        RowAction {
            id: deleteAction
            objectName: "deleteAction"
            readonly property ResultRow host: bar.host
            readonly property bool armed: host !== null && host.deleteArmed
            dpr: bar.dpr
            shared: true
            visible: bar.deleteButton && host !== null && !host.isApp // uninstalling is for Windows Settings
            glyph: "" // Delete
            tip: armed ? "" : qsTranslate("ResultRow", "删除（点两次，移到回收站）")
            text: !armed ? ""
                  : host.selected && host.selectedCount > 1 ? qsTranslate("ResultRow", "确认删除 %1 项").arg(host.selectedCount)
                  : qsTranslate("ResultRow", "确认删除")
            danger: armed
            onClicked: bar.host.deleteClicked()
        }
    }
}
