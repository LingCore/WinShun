pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// One result: icon, highlighted name, folder (or the matching line for
// content search) and a small trailing label. The current and the hovered row
// show buttons instead of the label: reveal, copy, copy path, delete (click
// twice). An installed app shows where it comes from instead of a folder, an
// "应用" tag, and its own buttons: run as administrator, reveal, copy (its
// program file, if it has one), copy path. A place in Windows (a page of
// Settings, a Control Panel task, a system tool) is tagged "系统", says where
// in Windows it is, and has only the copy button, which copies its command.
// A web shortcut's site or search is tagged "网页" and copies its address; a
// search on one of its suggestions (under the search's own row) reads just
// the suggestion and is tagged "建议".
// A system switch (深色模式, 蓝牙: switchReady) shows its state in place of the
// "系统" tag, a switch at a width of its own whatever the state, and a gear
// that opens its page; its second line says what there is to know of it, or
// (in red) why the last try failed. A click on it, or on the row, is Enter.
// File metadata stays beside the text, also on the current row where the
// buttons take the right end. Size and date can be shown independently;
// together they align with the name and path, sharing one narrow column.
// A picture, a video, a PDF shows its thumbnail for an icon, once loaded.
// A file or folder on a network share that cannot be reached now is dimmed
// and tagged "离线".
// A plugin's row (an app in the list's terms) is tagged with the plugin's
// name, shows what the plugin said under its title, and has one button,
// which copies its text, when it has text of its own to copy or its Enter
// copies (its actions are the plugin's: Enter, Ctrl+Enter, Shift+Enter, and
// the menu).
// The rows shown carry their shortcut on the icon (Ctrl+1 opens the first one
// shown); one another program holds is struck through, its tip says why.
// A file's or folder's row can be dragged out with the mouse, into a chat or
// a folder (with the selection, when it is in it).
Item {
    id: row

    required property int index
    required property string name
    required property string nameHtml
    required property var nameMatches // where the terms are in the name: start, length, ...
    required property string folder
    required property var folderMatches // where in it the terms the name lacks are: start, length, ...
    required property string path
    required property bool isDir
    required property string icon
    required property string thumbnail // a picture's, video's, PDF's; "" for none
    required property string snippetHtml
    required property int line
    required property string location // of a content match: "第 3 行", "第 2 页"
    required property bool recent
    required property bool isApp // or a place, or a plugin's row
    required property bool place // or a web page
    required property bool web
    required property bool suggestion // a web search on one of the site's suggestions: tagged 建议
    required property bool packagedApp
    required property bool elevatable
    required property bool revealable
    required property bool copyable
    required property bool selected // one of several picked with Ctrl / Shift
    required property string modified // "昨天 14:32"; empty for an app, or not known
    required property string modifiedFull
    required property string modifiedDate // "2026-10-15 14:32", for the tip
    required property string size // "1.2 MB", for the tip
    required property bool offline // on a network share that cannot be reached now: dimmed, tagged
    required property bool plugin // a plugin's row
    required property string source // ... the plugin's name
    required property bool pluginCopies // ... with its own text to copy, or Enter copies: its copy button
    required property string folderAway // ... its second line while the keyboard is on another row; "": the folder line
    required property bool favorite // one of its user's favorites: tagged 收藏 (with its keyword)
    required property string favoriteKeyword
    required property bool missing // a favorite no longer there: dimmed, tagged
    required property string switchId // a system switch's row (SearchResult::switchId)
    required property int switchOn // -1 not known, 0 off, 1 on, 2 mixed
    required property int switchLevel // the volume, 0–100; -1 not known, or another switch
    required property bool switchBusy // being written
    required property string switchError // why the last try failed; the second line in red
    required property bool switchReady // draws its switch (else an ordinary place, with its tag)
    property bool showModified: true
    property bool showSize: false
    readonly property bool sizeShown: showSize && size.length > 0
    readonly property bool modifiedShown: showModified && modified.length > 0
    property int hint: 0 // its number for the number keys (Ctrl+1…), on its icon; 0 for none
    property string hintModifier: "Ctrl" // ... with this held
    property bool hintTaken: false // another program holds that chord as a global hotkey: shown struck through
    property real hintOpacity: 1
    // The window's tip for a name or folder cut short (one for all its rows).
    property NameTip nameTip: null

    readonly property bool contentMode: line > 0 // a content match (also mixed into 全部)
    readonly property bool current: ListView.isCurrentItem
    // With rows selected, the background marks them; the current row keeps only its accent pill.
    readonly property int selectedCount: ListView.view ? ListView.view.model.selectedCount : 0
    readonly property var results: ListView.view ? ListView.view.model : null // the ResultModel
    readonly property bool showActions: current || area.containsMouse
    // ... and it has some: a plugin's row may have none, its badge then stays (GitHub #113).
    readonly property bool actionsShown: showActions && actions.visibleChildren.length > 0
    // The volume's number and bar, left of its switch (their room kept before the level is read).
    readonly property bool levelShown: switchReady && switchId === "volume"
    readonly property real levelSpace: levelShown ? levelControl.width + levelControl.anchors.rightMargin : 0
    // Room kept on the right for the buttons, the badge or the label.
    readonly property real trailingSpace: (switchReady ? switchControl.width + switchControl.anchors.rightMargin + levelSpace
                                                         + (showActions ? actions.width + 2 : 0)
                                           : actionsShown ? actions.width + actions.anchors.rightMargin
                                           : isApp ? badge.width + badge.anchors.rightMargin
                                           : trailing.width + trailing.anchors.rightMargin) + 10
    property bool deleteArmed: false // first click on delete; the second one deletes

    signal clicked(int index, int modifiers)
    signal doubleClicked(int index, int modifiers) // opens, where a click only selects (Launcher.doubleClickToOpen, #44)
    signal menuRequested(int index, point globalPos, int modifiers) // with Shift: the system's menu
    signal actionRequested(int index, int action)
    signal switchClicked(int index) // its switch clicked
    signal dragRequested(int index) // its file or folder dragged out of the window

    // Text on whole device pixels. Qt rounds each corner of a glyph's quad to
    // the pixel grid on the GPU, in single precision: on a half pixel (where
    // centring often puts it at 150 %) the rounding goes either way from one
    // scroll position to the next, and the text jumps or stretches by a pixel.
    function onPixel(y) { return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio }
    function upToPixel(y) { return Math.ceil(y * Screen.devicePixelRatio - 1e-6) / Screen.devicePixelRatio }

    onShowActionsChanged: if (!showActions) deleteArmed = false
    // Taken for another switch, the new one is there from the first frame: nothing slides between two.
    onSwitchIdChanged: {
        switchControl.animated = false
        levelControl.animated = false
        Qt.callLater(() => {
            switchControl.animated = true
            levelControl.animated = true
        })
    }
    Connections {
        target: row.results
        function onSwitchNudged(id: string) {
            if (id === row.switchId)
                switchControl.pulse()
        }
    }
    onPathChanged: { // delegate reused for another result
        deleteArmed = false
        dropTip()
    }

    // Shortened: more "…" shown than the text has (elidedHtml puts its own in).
    function cut(shown: string, whole: string): bool {
        return shown.split("…").length > whole.split("…").length
    }
    readonly property bool nameCut: title.truncated || cut(title.text, name)
    readonly property bool folderCut: contentMode ? contentFolder.truncated
                                    : folderLine.truncated || (folderLine.marked && cut(folderLine.text, folder))
    // Over its icon or text (not the date, the buttons) with something cut:
    // the tip with all of it (NameTip), once the mouse rests there.
    readonly property bool wantsTip: nameTip !== null && (nameCut || folderCut) && area.containsMouse && !area.pressed
                                     && area.mouseX >= iconSlot.x && area.mouseX < textColumn.x + textColumn.width
    onWantsTipChanged: offerTip()
    function offerTip() {
        if (!nameTip)
            return
        if (!wantsTip) {
            nameTip.withdraw(row)
            return
        }
        nameTip.offer(row, title, {
            name: name, folder: folderLine.away ? folderAway : folder, path: path, size: size, modified: modifiedDate,
            onDisk: !isApp && !offline && !missing, at: area.mapToGlobal(area.mouseX, area.mouseY)
        })
    }
    function dropTip() {
        if (nameTip && nameTip.source === row)
            nameTip.dismiss()
    }
    ListView.onPooled: dropTip()
    Component.onDestruction: dropTip()

    Timer {
        id: disarmTimer
        interval: 3000
        onTriggered: row.deleteArmed = false
    }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: Theme.largeControlRadius
        color: row.selected || (row.current && row.selectedCount === 0) ? Theme.selection
               : area.containsMouse ? Theme.hover : "transparent"
    }

    Rectangle { // accent pill on the current row, as in Windows 11 lists
        visible: row.current
        x: 7
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: 18
        radius: Theme.smallRadius
        color: Theme.accent
    }

    Item {
        id: iconSlot
        opacity: row.offline || row.missing ? 0.6 : 1
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 32 // 48 device pixels at 150 %, 64 at 200 %: sizes icons are drawn for
        height: 32

        Image {
            id: iconImage
            visible: !thumbnailImage.ready

            readonly property real dpr: Screen.devicePixelRatio
            readonly property int pixels: Math.round(iconSlot.width * dpr)

            // Exactly as many device pixels as the icon has, sampled
            // unsmoothed: smoothly it would blur wherever it sits on a half
            // pixel (at 150 % most rows do). Nudged off the exact half pixel,
            // where the nearest sample is a tie that can take one column twice.
            // sourceSize is in logical pixels: Image asks the provider for it
            // times the device pixel ratio.
            x: 1 / 64
            y: 1 / 64
            width: pixels / dpr
            height: pixels / dpr
            source: row.icon
            sourceSize.width: pixels / dpr
            sourceSize.height: pixels / dpr
            smooth: false
            asynchronous: true
            fillMode: Image.PreserveAspectFit
        }

        FileThumbnail {
            id: thumbnailImage
            source: row.thumbnail
        }
    }

    Column {
        id: textColumn
        objectName: "resultText"
        opacity: row.offline || row.missing ? 0.6 : 1
        y: row.onPixel((row.height - height) / 2)
        anchors.left: iconSlot.right
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: row.trailingSpace + (metadata.visible ? metadata.width + 16 : 0)
        spacing: 4

        Item {
            width: parent.width
            height: row.upToPixel(title.implicitHeight) // the line below on a whole pixel too

            Text {
                id: title
                width: Math.min(implicitWidth, parent.width)
                // A long name is shortened around its matches, not past them,
                // and keeps its end (the extension, "(2)", "v3") with or
                // without any. An app's (and a web search's) is short, its
                // highlights its own.
                text: row.results && !row.isApp
                      ? row.results.elidedHtml(row.name, row.nameMatches, parent.width, font, row.results.highlightColor)
                      : row.nameHtml
                textFormat: Text.StyledText
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: Theme.uiTitle
            }

            Text { // content search: where the file lives, next to its name
                id: contentFolder
                visible: row.contentMode
                y: row.onPixel(title.baselineOffset - baselineOffset) // on the name's baseline
                anchors.left: title.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                text: row.folder
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.faint
                font.pixelSize: Theme.uiCaption
            }
        }

        Text {
            id: folderLine
            // A plugin's row with a line for while the keyboard is on another
            // row (how to get there and what Enter then does: 传's request
            // that came in above it, GitHub #113): plain, kept from its start.
            readonly property bool away: !row.current && !row.contentMode && row.folderAway.length > 0
            // A term found in a folder rather than the name stands out there,
            // and stays in sight when the path is shortened.
            readonly property bool marked: !away && !row.contentMode && row.folderMatches.length > 0 && row.results !== null
            width: parent.width
            text: row.contentMode ? row.snippetHtml
                : away ? row.folderAway
                : marked ? row.results.elidedHtml(row.folder, row.folderMatches, width, font, row.results.highlightColor)
                : row.folder
            textFormat: row.contentMode || marked ? Text.StyledText : Text.PlainText
            elide: row.contentMode || marked || away ? Text.ElideRight : Text.ElideMiddle
            maximumLineCount: 1
            color: row.switchError.length > 0 ? Theme.danger : Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
    }

    // One value is centred as before; two use the text's baselines. Both
    // lines on the left stop short of the wider value, including on hover.
    Item {
        id: metadata
        objectName: "resultMetadata"
        visible: row.sizeShown || row.modifiedShown
        anchors.right: parent.right
        anchors.rightMargin: row.trailingSpace
        height: row.height
        width: Math.ceil(Math.max(row.sizeShown ? fileSize.implicitWidth : 0,
                                  row.modifiedShown ? date.implicitWidth : 0))

        Text {
            id: fileSize
            objectName: "fileSizeLabel"
            visible: row.sizeShown
            anchors.right: parent.right
            y: row.modifiedShown ? row.onPixel(textColumn.y + title.baselineOffset - baselineOffset)
                                : row.onPixel((row.height - height) / 2)
            text: row.size
            textFormat: Text.PlainText
            color: Theme.faint
            font.pixelSize: Theme.uiCaption
        }

        Text {
            id: date
            objectName: "modifiedLabel"
            visible: row.modifiedShown
            anchors.right: parent.right
            y: row.sizeShown ? row.onPixel(textColumn.y + folderLine.y + folderLine.baselineOffset - baselineOffset)
                            : row.onPixel((row.height - height) / 2)
            text: row.modified
            textFormat: Text.PlainText
            color: Theme.faint
            font.pixelSize: Theme.uiCaption
        }
    }

    Text {
        id: trailing
        visible: !row.showActions && !row.isApp
        anchors.right: parent.right
        anchors.rightMargin: 18
        y: row.onPixel((row.height - height) / 2)
        width: Math.min(implicitWidth, 200)
        elide: Text.ElideMiddle
        text: row.contentMode ? row.location
            : row.offline ? qsTr("离线")
            : row.missing ? qsTr("已不存在")
            : row.favorite ? (row.favoriteKeyword.length > 0 ? qsTr("收藏 · %1").arg(row.favoriteKeyword) : qsTr("收藏"))
            : row.recent ? qsTr("最近") : ""
        color: Theme.faint
        font.pixelSize: Theme.uiCaption
    }

    Rectangle {
        id: badge
        visible: row.isApp && !row.actionsShown && !row.switchReady
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: badgeText.implicitWidth + 16
        height: Theme.badgeHeight
        radius: Theme.pillRadius(height)
        color: Theme.badge

        Text {
            id: badgeText
            anchors.horizontalCenter: parent.horizontalCenter
            y: row.onPixel((badge.height - height) / 2)
            text: row.plugin ? row.source : row.missing ? qsTr("已不存在")
                : row.favorite ? (row.favoriteKeyword.length > 0 ? qsTr("收藏 · %1").arg(row.favoriteKeyword) : qsTr("收藏"))
                : row.recent ? qsTr("最近") : row.suggestion ? qsTr("建议") : row.web ? qsTr("网页")
                : row.place ? qsTr("系统") : qsTr("应用")
            color: Theme.accent
            font.pixelSize: Theme.uiCaption
        }
    }

    IconNumber { // "Ctrl+1" (or "Alt+1"): opens this row
        id: iconNumber
        opacity: row.hintOpacity
        keys: row.hint > 0 ? row.hintModifier + "+" + row.hint : ""
        taken: row.hintTaken
        // From the icon's top left corner rightwards, clear of the title. Not
        // moved onto a wide thumbnail's corner: it would cover half of it.
        x: row.onPixel(iconSlot.x - 5)
        y: row.onPixel(iconSlot.y - 4)
    }

    MouseArea {
        id: area

        // A file's or folder's row, held with the mouse and moved past the
        // system's drag distance, drags it out (into a chat, a folder). The
        // list then does not take the drag for scrolling (the wheel, the
        // scroll bar and touch still scroll it); the other rows scroll as before.
        property point pressedAt: Qt.point(0, 0)
        property bool draggedOut: false

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: (mouse) => {
            if (row.nameTip)
                row.nameTip.dismiss()
            draggedOut = false
            pressedAt = Qt.point(mouse.x, mouse.y)
            preventStealing = mouse.button === Qt.LeftButton && !row.isApp && !row.offline && !row.missing
                              && mouse.source === Qt.MouseEventNotSynthesized
        }
        // The wheel hides the tip even where the list does not scroll (Windows' own tips do);
        // passed on, so the list still scrolls.
        onWheel: (wheel) => {
            if (row.nameTip)
                row.nameTip.dismiss()
            wheel.accepted = false
        }
        onPositionChanged: (mouse) => {
            if (!pressed) { // moved: a tip dismissed (a key, a scroll) may come again
                row.offerTip()
                return
            }
            if (!preventStealing || draggedOut || !(mouse.buttons & Qt.LeftButton))
                return
            if (Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y) < Application.styleHints.startDragDistance)
                return
            draggedOut = true
            row.dragRequested(row.index) // returns once it was let go
        }
        onClicked: (mouse) => {
            if (draggedOut) // the release that ended a drag out
                return
            if (mouse.button === Qt.RightButton)
                row.menuRequested(row.index, area.mapToGlobal(mouse.x, mouse.y), mouse.modifiers)
            else
                row.clicked(row.index, mouse.modifiers)
        }
        onDoubleClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton)
                row.doubleClicked(row.index, mouse.modifiers)
        }

        // Inside the MouseArea so that hovering a button still hovers the row.
        // A system switch, always there, at the same width in every state.
        ToggleSwitch {
            id: switchControl
            visible: row.switchReady
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            // The volume is never "自定义": its word ("开", "关") right against
            // the bar's gap, not 自定义's width further.
            mixable: !row.levelShown
            wordAlign: Text.AlignRight
            activeFocusOnTab: false
            checked: row.switchOn === 1
            mixed: row.switchOn === 2
            unknown: row.switchOn < 0
            busy: row.switchBusy
            onToggled: row.switchClicked(row.index)
        }

        SwitchLevel { // the volume: its number and bar
            id: levelControl
            visible: row.levelShown
            anchors.right: switchControl.left
            anchors.rightMargin: 12
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            value: row.switchLevel
            muted: row.switchOn === 0
            opacity: switchControl.opacity // dimmed with the switch while a write takes long
        }

        Row {
            id: actions
            visible: row.showActions
            anchors.right: parent.right
            anchors.rightMargin: row.switchReady ? switchControl.width + 14 + 4 + row.levelSpace : 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            RowAction {
                id: switchPageAction
                visible: row.switchReady
                glyph: "" // Settings (gear)
                tip: qsTr("打开设置")
                tipShortcut: "Ctrl+Enter"
                onClicked: row.actionRequested(row.index, Launcher.Reveal)
            }

            RowAction {
                id: adminAction
                visible: row.isApp && row.elevatable && !row.switchReady
                glyph: "" // Admin (shield)
                tip: qsTr("以管理员身份运行")
                tipShortcut: "Ctrl+Shift+Enter"
                onClicked: row.actionRequested(row.index, Launcher.RunAsAdmin)
            }
            RowAction {
                id: revealAction
                visible: !row.switchReady && (!row.isApp || row.revealable)
                glyph: "" // OpenFolder
                tip: row.packagedApp ? qsTr("打开安装文件夹") : qsTr("打开所在位置")
                tipShortcut: "Ctrl+Enter"
                onClicked: row.actionRequested(row.index, Launcher.Reveal)
            }
            RowAction {
                id: terminalAction
                visible: !row.isApp // a file's or folder's: its folder in cmd
                glyph: "" // CommandPrompt
                tip: qsTr("在终端中打开")
                onClicked: row.actionRequested(row.index, Launcher.OpenTerminal)
            }
            RowAction {
                id: copyAction
                visible: row.copyable && !row.switchReady // a desktop app copies its program; a packaged one has none
                glyph: "" // Copy
                tip: qsTr("复制")
                tipShortcut: "Ctrl+C"
                onClicked: row.actionRequested(row.index, Launcher.CopyItem)
            }
            RowAction {
                id: copyPathAction
                visible: !row.switchReady && (!row.plugin || row.pluginCopies)
                glyph: row.plugin ? "" : "copyPath" // Copy; else drawn by Glyph: a box holding "\\.."
                tip: row.plugin ? qsTr("复制") : row.web ? qsTr("复制网址") : row.place ? qsTr("复制打开命令") : qsTr("复制完整路径")
                tipShortcut: row.plugin ? "Ctrl+C" : "Ctrl+Shift+C"
                onClicked: row.actionRequested(row.index, Launcher.CopyPath)
            }
            RowAction {
                id: deleteAction
                visible: !row.isApp // uninstalling is for Windows Settings
                glyph: "" // Delete
                tip: row.deleteArmed ? "" : qsTr("删除（点两次，移到回收站）")
                text: !row.deleteArmed ? ""
                      : row.selected && row.selectedCount > 1 ? qsTr("确认删除 %1 项").arg(row.selectedCount)
                      : qsTr("确认删除")
                danger: row.deleteArmed
                onClicked: {
                    if (row.deleteArmed) {
                        row.deleteArmed = false
                        row.actionRequested(row.index, Launcher.Recycle)
                    } else {
                        row.deleteArmed = true
                        disarmTimer.restart()
                    }
                }
            }
        }
    }

    // One tip for all the buttons: it glides from one to the next. The date
    // has one too: the time in full.
    HoverTip {
        readonly property RowAction hovered: switchPageAction.tipWanted ? switchPageAction
                                           : adminAction.tipWanted ? adminAction
                                           : revealAction.tipWanted ? revealAction
                                           : terminalAction.tipWanted ? terminalAction
                                           : copyAction.tipWanted ? copyAction
                                           : copyPathAction.tipWanted ? copyPathAction
                                           : deleteAction.tipWanted ? deleteAction : null
        // Hover handlers on items under the row's mouse area get no hover: asked of the area instead.
        function under(item: Item): bool {
            return area.containsMouse && item.contains(item.mapFromItem(area, area.mouseX, area.mouseY))
        }
        readonly property bool onDate: !hovered && date.visible && under(date)
        readonly property bool onKeys: !hovered && !onDate && iconNumber.visible && iconNumber.taken && under(iconNumber)
        target: hovered ? hovered : onDate ? date : onKeys ? iconNumber : null
        text: hovered ? hovered.tip : onDate ? row.modifiedFull
            : onKeys ? qsTr("%1 被别的程序占用了，在这里按也会交给那个程序。可在设置的“结果列表”里改用 %2")
                       .arg(iconNumber.keys).arg(row.hintModifier === "Alt" ? "Ctrl" : "Alt")
            : ""
        shortcut: hovered ? hovered.tipShortcut : ""
    }
}
