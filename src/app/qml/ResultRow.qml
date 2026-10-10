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
// A web shortcut's site or search is tagged "网页" and copies its address.
// A file or folder says when it was last written, at the end of its name's
// line (also on the current row, where the buttons take the right end).
// While the modifier of the number keys is held, the rows show their number
// there instead (Ctrl+1 opens the first one shown).
Item {
    id: row

    required property int index
    required property string name
    required property string nameHtml
    required property string folder
    required property string path
    required property bool isDir
    required property string icon
    required property string snippetHtml
    required property int line
    required property string location // of a content match: "第 3 行", "第 2 页"
    required property bool recent
    required property bool isApp // or a place
    required property bool place // or a web page
    required property bool web
    required property bool packagedApp
    required property bool elevatable
    required property bool revealable
    required property bool copyable
    required property bool selected // one of several picked with Ctrl / Shift
    required property string modified // "昨天 14:32"; empty for an app, or not known
    required property string modifiedFull
    property bool showModified: true
    property int hint: 0 // the number keys' modifier held: this row's number, 0 for none

    readonly property bool contentMode: line > 0 // a content match (also mixed into 全部)
    readonly property bool current: ListView.isCurrentItem
    // With rows selected, the background marks them; the current row keeps only its accent pill.
    readonly property int selectedCount: ListView.view ? ListView.view.model.selectedCount : 0
    readonly property bool showActions: (current || area.containsMouse) && hint === 0
    // Room kept on the right for the buttons, the keycap, the badge or the label.
    readonly property real trailingSpace: (showActions ? actions.width + actions.anchors.rightMargin
                                           : keycap.visible ? keycap.width + keycap.anchors.rightMargin
                                           : isApp ? badge.width + badge.anchors.rightMargin
                                           : trailing.width + trailing.anchors.rightMargin) + 10
    property bool deleteArmed: false // first click on delete; the second one deletes

    signal clicked(int index, int modifiers)
    signal menuRequested(int index, point globalPos)
    signal actionRequested(int index, int action)

    // Text on whole device pixels. Qt rounds each corner of a glyph's quad to
    // the pixel grid on the GPU, in single precision: on a half pixel (where
    // centring often puts it at 150 %) the rounding goes either way from one
    // scroll position to the next, and the text jumps or stretches by a pixel.
    function onPixel(y) { return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio }
    function upToPixel(y) { return Math.ceil(y * Screen.devicePixelRatio - 1e-6) / Screen.devicePixelRatio }

    onShowActionsChanged: if (!showActions) deleteArmed = false
    onPathChanged: deleteArmed = false // delegate reused for another result

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
        radius: 6
        color: row.selected || (row.current && row.selectedCount === 0) ? Theme.selection
               : area.containsMouse ? Theme.hover : "transparent"
    }

    Rectangle { // accent pill on the current row, as in Windows 11 lists
        visible: row.current
        x: 7
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: 18
        radius: 1.5
        color: Theme.accent
    }

    Item {
        id: iconSlot
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 32 // 48 device pixels at 150 %, 64 at 200 %: sizes icons are drawn for
        height: 32

        Image {
            id: iconImage

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
    }

    Column {
        y: row.onPixel((row.height - height) / 2)
        anchors.left: iconSlot.right
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: row.trailingSpace + (date.visible ? date.width + 16 : 0)
        spacing: 4

        Item {
            width: parent.width
            height: row.upToPixel(title.implicitHeight) // the line below on a whole pixel too

            Text {
                id: title
                width: Math.min(implicitWidth, parent.width)
                text: row.nameHtml
                textFormat: Text.StyledText
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: Theme.fontTitle
            }

            Text { // content search: where the file lives, next to its name
                visible: row.contentMode
                y: row.onPixel(title.baselineOffset - baselineOffset) // on the name's baseline
                anchors.left: title.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                text: row.folder
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.faint
                font.pixelSize: Theme.fontBody
            }
        }

        Text {
            width: parent.width
            text: row.contentMode ? row.snippetHtml : row.folder
            textFormat: row.contentMode ? Text.StyledText : Text.PlainText
            elide: row.contentMode ? Text.ElideRight : Text.ElideMiddle
            maximumLineCount: 1
            color: Theme.subtext
            font.pixelSize: Theme.fontBody
        }
    }

    // When it was last written: centred on the row, and both lines on the left
    // stop short of it, so the path below the name never runs under it.
    Text {
        id: date
        visible: row.showModified && row.modified.length > 0
        anchors.right: parent.right
        anchors.rightMargin: row.trailingSpace
        y: row.onPixel((row.height - height) / 2)
        width: Math.ceil(implicitWidth)
        text: row.modified
        textFormat: Text.PlainText
        color: Theme.faint
        font.pixelSize: Theme.fontBody

        HoverHandler { id: dateHover }
    }

    Text {
        id: trailing
        visible: !row.showActions && !row.isApp && row.hint === 0
        anchors.right: parent.right
        anchors.rightMargin: 18
        y: row.onPixel((row.height - height) / 2)
        width: Math.min(implicitWidth, 200)
        elide: Text.ElideMiddle
        text: row.contentMode ? row.location : row.recent ? qsTr("最近") : ""
        color: Theme.faint
        font.pixelSize: Theme.fontBody
    }

    Rectangle {
        id: badge
        visible: row.isApp && !row.showActions && row.hint === 0
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: badgeText.implicitWidth + 16
        height: 24
        radius: 12
        color: Theme.badge

        Text {
            id: badgeText
            anchors.horizontalCenter: parent.horizontalCenter
            y: row.onPixel((badge.height - height) / 2)
            text: row.recent ? qsTr("最近") : row.web ? qsTr("网页") : row.place ? qsTr("系统") : qsTr("应用")
            color: Theme.accent
            font.pixelSize: Theme.fontCaption
        }
    }

    KeyCap { // the number keys' modifier held: it and this number open the row
        id: keycap
        visible: row.hint > 0
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        number: row.hint
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                row.menuRequested(row.index, area.mapToGlobal(mouse.x, mouse.y))
            else
                row.clicked(row.index, mouse.modifiers)
        }

        // Inside the MouseArea so that hovering a button still hovers the row.
        Row {
            id: actions
            visible: row.showActions
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            RowAction {
                id: adminAction
                visible: row.isApp && row.elevatable
                glyph: "" // Admin (shield)
                tip: qsTr("以管理员身份运行")
                tipShortcut: "Ctrl+Shift+Enter"
                onClicked: row.actionRequested(row.index, Launcher.RunAsAdmin)
            }
            RowAction {
                id: revealAction
                visible: !row.isApp || row.revealable
                glyph: "" // OpenFolder
                tip: row.packagedApp ? qsTr("打开安装文件夹") : qsTr("打开所在位置")
                tipShortcut: "Ctrl+Enter"
                onClicked: row.actionRequested(row.index, Launcher.Reveal)
            }
            RowAction {
                id: copyAction
                visible: row.copyable // a desktop app copies its program; a packaged one has none
                glyph: "" // Copy
                tip: qsTr("复制")
                tipShortcut: "Ctrl+C"
                onClicked: row.actionRequested(row.index, Launcher.CopyItem)
            }
            RowAction {
                id: copyPathAction
                glyph: "copyPath" // drawn by Glyph: a box holding "\\.."
                tip: row.web ? qsTr("复制网址") : row.place ? qsTr("复制打开命令") : qsTr("复制完整路径")
                tipShortcut: "Ctrl+Shift+C"
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
        readonly property RowAction hovered: adminAction.tipWanted ? adminAction
                                           : revealAction.tipWanted ? revealAction
                                           : copyAction.tipWanted ? copyAction
                                           : copyPathAction.tipWanted ? copyPathAction
                                           : deleteAction.tipWanted ? deleteAction : null
        readonly property bool onDate: !hovered && date.visible && dateHover.hovered
        target: hovered ? hovered : onDate ? date : null
        text: hovered ? hovered.tip : onDate ? row.modifiedFull : ""
        shortcut: hovered ? hovered.tipShortcut : ""
    }
}
