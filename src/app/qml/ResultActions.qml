pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A result row's buttons (ResultRow): open its settings page (a switch's),
// run as administrator, reveal, open in a terminal, copy, copy the path,
// delete (click twice), and the ones chosen in the settings besides
// (preview, copy the name, favorite): which show, and in which order, is
// `order`. Not made in every row: the list keeps a few sets
// (RowActionsPool) and lends one to a row while it shows them. Lent to
// another row it only takes that row's state: nothing is made, the style's
// surfaces and the drawn copy-path icon included.
Item {
    id: bar

    property ResultRow host: null // the row it is lent to; null: put away
    property real dpr: 1
    // The settings' (Launcher.rowActions, RowActions.h): the buttons that
    // show, in their order, left to right. One not named is on no row
    // (no "delete": GitHub #316); a switch's own button is always there.
    property var order: ["admin", "reveal", "terminal", "copy", "copyPath", "delete"]
    property string previewKey // the preview's shortcut, for its button's tip
    function shows(id: string): bool { return order.indexOf(id) >= 0 }

    readonly property var buttons: ({
        "admin": adminAction, "reveal": revealAction, "terminal": terminalAction, "copy": copyAction,
        "copyPath": copyPathAction, "delete": deleteAction, "preview": previewAction, "copyName": copyNameAction,
        "favorite": favoriteAction
    })
    // The buttons as they stand, left to right: the switch's own, then as
    // `order` says. Each is placed by what shows before it (leftOf); the
    // letters (letterOf, clickLettered) go by the same. Not by the order of
    // the children: a button taken out of its parent and put back at the end
    // was laid out in its new place and no longer drawn there (seen in
    // --shortcut-panel's row-buttons-chosen.png).
    readonly property var sequence: {
        const all = [switchPageAction]
        for (const id of order) {
            const button = buttons[id]
            if (button && all.indexOf(button) < 0)
                all.push(button)
        }
        return all
    }
    function leftOf(button: RowAction): real {
        let x = 0
        for (const other of sequence) {
            if (other === button)
                return x
            if (other.visible)
                x += other.width + actions.spacing
        }
        return 0
    }
    readonly property real span: {
        let x = 0
        for (const button of sequence) {
            if (button.visible)
                x += button.width + actions.spacing
        }
        return Math.max(0, x - actions.spacing)
    }

    // For the row: the room the buttons take, how many show, and the one the
    // mouse rests on (the row's HoverTip, one for its buttons and its date).
    readonly property real actionsWidth: actions.width
    readonly property bool small: host !== null && host.dense // 密集 rows: the smaller buttons (RowAction.small)
    readonly property int count: actions.visibleChildren.length
    readonly property RowAction hovered: switchPageAction.tipWanted ? switchPageAction
                                       : adminAction.tipWanted ? adminAction
                                       : revealAction.tipWanted ? revealAction
                                       : terminalAction.tipWanted ? terminalAction
                                       : copyAction.tipWanted ? copyAction
                                       : copyPathAction.tipWanted ? copyPathAction
                                       : deleteAction.tipWanted ? deleteAction
                                       : previewAction.tipWanted ? previewAction
                                       : copyNameAction.tipWanted ? copyNameAction
                                       : favoriteAction.tipWanted ? favoriteAction : null

    // 字母直达 (GitHub #357): the host's letters on its buttons as they show,
    // left to right; the nth letter clicks the nth button shown.
    readonly property string letters: host !== null ? host.actionLetters : ""
    function letterOf(button: RowAction): string {
        if (letters.length === 0 || !button.visible)
            return ""
        let n = 0
        for (const child of sequence) {
            if (child === button)
                return n < letters.length ? letters[n] : ""
            if (child.visible)
                ++n
        }
        return ""
    }
    // False: no button shown has that place.
    function clickLettered(n: int, modifiers): bool {
        const mods = modifiers ?? Qt.NoModifier
        let shown = 0
        for (const child of sequence) {
            if (!child.visible)
                continue
            if (shown === n) {
                child.lastModifiers = mods
                child.clicked()
                child.lastModifiers = Qt.NoModifier
                return true
            }
            ++shown
        }
        return false
    }

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

    Item {
        id: actions
        readonly property real spacing: 2
        anchors.right: parent.right
        anchors.rightMargin: bar.host ? bar.host.actionsRightMargin : 0
        anchors.verticalCenter: parent.verticalCenter
        width: bar.span
        height: bar.count > 0 ? (bar.small ? 24 : 32) : 0

        RowAction {
            id: switchPageAction
            x: bar.leftOf(switchPageAction)
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(switchPageAction)
            visible: bar.host !== null && bar.host.switchReady
            glyph: "" // Settings (gear)
            tip: qsTranslate("ResultRow", "打开设置")
            tipShortcut: "Ctrl+Enter"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.Reveal)
        }
        RowAction {
            id: adminAction
            x: bar.leftOf(adminAction)
            objectName: "adminAction"
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(adminAction)
            visible: bar.shows("admin") && bar.host !== null && bar.host.elevatable && !bar.host.switchReady
            glyph: "" // Admin (shield)
            tip: qsTranslate("ResultRow", "以管理员身份运行")
            tipShortcut: "Ctrl+Shift+Enter"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.RunAsAdmin)
        }
        RowAction {
            id: revealAction
            x: bar.leftOf(revealAction)
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(revealAction)
            visible: bar.shows("reveal") && bar.host !== null && !bar.host.switchReady && (!bar.host.isApp || bar.host.revealable)
            glyph: "" // OpenFolder
            tip: bar.host !== null && bar.host.packagedApp ? qsTranslate("ResultRow", "打开安装文件夹") : qsTranslate("ResultRow", "打开所在位置")
            tipShortcut: "Ctrl+Enter"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.Reveal)
        }
        RowAction {
            id: terminalAction
            x: bar.leftOf(terminalAction)
            objectName: "terminalAction"
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(terminalAction)
            visible: bar.shows("terminal") && bar.host !== null && !bar.host.isApp // a file's or folder's: its folder in cmd
            glyph: "" // CommandPrompt
            tip: qsTranslate("ResultRow", "在终端中打开（Shift+点击以管理员身份）")
            onClicked: bar.host.actionRequested(bar.host.index,
                (lastModifiers & Qt.ShiftModifier) ? Launcher.OpenTerminalAdmin : Launcher.OpenTerminal)
        }
        RowAction {
            id: copyAction
            x: bar.leftOf(copyAction)
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(copyAction)
            // A desktop app copies its program; a packaged one has none.
            visible: bar.shows("copy") && bar.host !== null && bar.host.copyable && !bar.host.switchReady
            glyph: "" // Copy
            tip: qsTranslate("ResultRow", "复制")
            tipShortcut: "Ctrl+C"
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.CopyItem)
        }
        RowAction {
            id: copyPathAction
            x: bar.leftOf(copyPathAction)
            objectName: "copyPathAction"
            readonly property ResultRow host: bar.host
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(copyPathAction)
            visible: bar.shows("copyPath") && host !== null && !host.settingsAction && !host.switchReady
                     && (!host.plugin || host.pluginCopies)
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
            x: bar.leftOf(deleteAction)
            objectName: "deleteAction"
            readonly property ResultRow host: bar.host
            readonly property bool armed: host !== null && host.deleteArmed
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(deleteAction)
            visible: bar.shows("delete") && host !== null && !host.isApp // uninstalling is for Windows Settings
            glyph: "" // Delete
            tip: armed ? "" : qsTranslate("ResultRow", "删除（点两次，移到回收站）")
            text: !armed ? ""
                  : host.selected && host.selectedCount > 1 ? qsTranslate("ResultRow", "确认删除 %1 项").arg(host.selectedCount)
                  : qsTranslate("ResultRow", "确认删除")
            danger: armed
            onClicked: bar.host.deleteClicked()
        }
        // Not among the buttons as they come: chosen in the settings.
        RowAction {
            id: previewAction
            x: bar.leftOf(previewAction)
            objectName: "previewAction"
            readonly property ResultRow host: bar.host
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(previewAction)
            // A file or a folder that is there to be read (as 预览 in its menu).
            visible: bar.shows("preview") && host !== null && !host.isApp && !host.missing && !host.offline
            glyph: "ph:eye"
            tip: qsTranslate("ResultRow", "预览")
            tipShortcut: bar.previewKey
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.Preview)
        }
        RowAction {
            id: copyNameAction
            x: bar.leftOf(copyNameAction)
            objectName: "copyNameAction"
            readonly property ResultRow host: bar.host
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(copyNameAction)
            visible: bar.shows("copyName") && host !== null && !host.switchReady && !host.settingsAction
            glyph: "ph:textbox"
            tip: host !== null && host.windowResult ? qsTranslate("ResultRow", "复制窗口标题") : qsTranslate("ResultRow", "复制名称")
            onClicked: bar.host.actionRequested(bar.host.index, Launcher.CopyName)
        }
        RowAction {
            id: favoriteAction
            x: bar.leftOf(favoriteAction)
            objectName: "favoriteAction"
            readonly property ResultRow host: bar.host
            readonly property bool kept: host !== null && host.favorite
            dpr: bar.dpr
            shared: true
            small: bar.small
            keyLetter: bar.letterOf(favoriteAction)
            // A file or a folder; anything else only to take it out again
            // (what else can be kept is the menu's to say).
            visible: bar.shows("favorite") && host !== null && !host.missing && (favoriteAction.kept || !host.isApp)
            glyph: favoriteAction.kept ? "ph-fill:star" : "ph:star"
            tip: favoriteAction.kept ? qsTranslate("ResultRow", "从收藏夹移除") : qsTranslate("ResultRow", "加入收藏夹")
            onClicked: bar.host.actionRequested(bar.host.index,
                favoriteAction.kept ? Launcher.RemoveFavorite : Launcher.AddFavorite)
        }
    }
}
