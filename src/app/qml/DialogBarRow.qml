pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A row of the dialog bar's list (DialogBarWindow.qml): icon, highlighted name, the
// folder it is in, and on the right where it comes from ("资源管理器", "固定",
// "最近"…), or a key to press. Where there is room, when it was last written
// at the end of its name's line. On its left, as in the launcher and the
// clipboard: its number (Ctrl+1 goes to the first row shown), or its letter
// (字母直达). A long name keeps its end; cut, the whole of it shows as the
// mouse rests on it (NameTip).
Item {
    id: row

    required property int index
    required property string name
    required property string nameHtml
    required property var nameMatches // where the terms are in the name: start, length, ...
    required property string folder
    required property string path
    required property string icon
    required property string thumbnail // a picture's, video's, PDF's (see ResultRow)
    required property bool isDir
    required property string modified // "昨天 14:32"; empty when not known
    required property string modifiedDate // "2026-10-15 14:32", for the tip
    required property string size // "1.2 MB", for the tip
    required property bool offline
    property string tag
    property bool showModified: true
    property int hint: 0 // the number that goes to this row (Ctrl+1...); 0 for none
    property string hintLetter // 字母直达: the letter that does, while the letters show
    property bool hintArmed: false // true after tapping the jump key (renders keycap); false while previewing passively
    property string hintJumpKey: "Ctrl" // modifier tapped to arm letter jump, for the HoverTip
    property real hintOpacity: 1 // the number's: shown as the settings say (NumberHints)
    property real hintOffsetY: 0 // counter-scrolls numberSlot during smooth wheel gliding so 1–9 stay visually steady
    property bool reserveHintSpace: false // every row keeps the room, numbered or not: one left edge
    property string hintModifier: "Ctrl" // ... with this held
    property bool hintTaken: false // one of Win顺's own shortcuts has that chord: shown struck through
    property bool canOpen: false // dialog opens files: Enter / Ctrl+n opens a file row
    property bool copyOnly: false // elevated dialog without service: Enter / Ctrl+n copies its path
    property NameTip nameTip: null // the window's tip for a name or folder cut short
    property QtObject tipHost: null // permits a detached HoverTip when the bar is short

    readonly property var results: ListView.view ? ListView.view.model : null // the bar's ResultModel

    // As ResultRow's: the whole name and folder (NameTip) when either is cut.
    function cut(shown: string, whole: string): bool {
        return shown.split("…").length > whole.split("…").length
    }
    readonly property bool nameCut: nameText.truncated || cut(nameText.text, name)
    readonly property bool folderCut: folderText.truncated
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
        nameTip.offer(row, nameText, {
            name: name, folder: folder, path: path, size: size, modified: modifiedDate, onDisk: !offline,
            at: area.mapToGlobal(area.mouseX, area.mouseY)
        })
    }
    function dropTip() {
        if (nameTip && nameTip.source === row)
            nameTip.dismiss()
        rowTip.dismiss()
    }
    onPathChanged: dropTip() // delegate reused for another entry
    Component.onDestruction: dropTip()

    readonly property bool current: ListView.isCurrentItem

    signal clicked(int index)
    signal menuRequested(int index, point globalPos)

    Accessible.role: Accessible.ListItem
    Accessible.name: tag.length > 0 ? name + ", " + folder + ", " + tag : name + ", " + folder
    Accessible.selected: current
    Accessible.onPressAction: clicked(index)

    // The window's, passed by the list: no Screen attached object in every
    // row (GitHub #111, see ClipRow). Text on whole device pixels (see ResultRow).
    property real dpr: 1
    function onPixel(y) { return Math.round(y * dpr) / dpr }
    function upToPixel(y) { return Math.ceil(y * dpr - 1e-6) / dpr }

    // The style's highlight (Theme.rowHighlight). The current row's is the
    // list's own, one for all its rows (DialogBarWindow); here only the hover.
    // Made the first time it shows, then only hidden (docs/pitfalls.md, ClipRow).
    Hook {
        anchors.fill: parent
        anchors.leftMargin: Theme.rowInset
        anchors.rightMargin: Theme.rowInset
        anchors.topMargin: Theme.edge(row.dpr)
        anchors.bottomMargin: Theme.edge(row.dpr)
        visible: !row.current && area.containsMouse
        lazy: true
        part: Theme.rowHighlight
        hovered: true
        role: "list"
        dpr: row.dpr
    }

    RowHintSlot {
        id: numberSlot
        objectName: "dialogResultNumber"
        glyphObjectName: "dialogResultNumberGlyph"
        visible: row.reserveHintSpace && (row.hint > 0 || row.hintLetter.length > 0)
        opacity: row.hintArmed && row.hintLetter.length > 0 ? 1 : row.hintOpacity
        x: row.onPixel(Theme.tok("clipHintX", 14)) // the icon sits where the clipboard's does
        y: row.hintOffsetY !== 0 ? -Math.round(-row.hintOffsetY * row.dpr) / row.dpr : 0
        height: row.height
        hint: row.hint
        hintLetter: row.hintLetter
        armed: row.hintArmed
        taken: row.hintTaken
        current: row.current
        dpr: row.dpr
    }

    Item {
        id: iconSlot
        x: 16 + (row.reserveHintSpace ? 28 : 0)
        anchors.verticalCenter: parent.verticalCenter
        width: 32 // 48 device pixels at 150 %: a size icons are drawn for
        height: 32

        Image { // exactly as many device pixels as the icon has (see ResultRow)
            visible: !thumbnailImage.ready
            readonly property real dpr: row.dpr
            readonly property int pixels: Math.round(iconSlot.width * dpr)

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
        objectName: "dialogResultText"
        y: row.onPixel((row.height - height) / 2)
        anchors.left: iconSlot.right
        anchors.leftMargin: 12
        anchors.right: tagText.left
        anchors.rightMargin: 10
        spacing: 2

        Item {
            width: parent.width
            height: row.upToPixel(nameText.implicitHeight) // the folder below on a whole pixel too

            Text {
                id: nameText
                objectName: "dialogResultName"
                readonly property real room: parent.width - (date.visible ? date.width + 10 : 0)
                width: Math.min(implicitWidth, room)
                // Shortened around its matches, its end kept (see ResultRow).
                text: row.results ? row.results.elidedHtml(row.name, row.nameMatches, room, font, row.results.highlightColor, row.results.highlightWeight)
                                  : row.nameHtml
                textFormat: Text.StyledText
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: Theme.uiBody
                // Keep paired lines close without reducing result hit targets.
                font.preferTypoLineMetrics: true
            }

            Text { // when it was last written; left out where the bar is narrow
                id: date
                visible: row.showModified && row.modified.length > 0 && row.width >= 420
                anchors.right: parent.right
                y: row.onPixel(nameText.baselineOffset - baselineOffset) // on the name's baseline
                width: Math.ceil(implicitWidth)
                text: row.modified
                textFormat: Text.PlainText
                color: Theme.faint
                font.pixelSize: Theme.uiCaption
            }
        }
        Text {
            id: folderText
            objectName: "dialogResultFolder"
            width: parent.width
            text: row.folder
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            maximumLineCount: 1
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
            font.preferTypoLineMetrics: true
        }
    }

    Text {
        id: tagText
        anchors.right: parent.right
        anchors.rightMargin: 18
        y: row.onPixel((row.height - height) / 2)
        width: Math.min(implicitWidth, 200)
        text: row.tag
        textFormat: Text.PlainText
        elide: Text.ElideMiddle
        color: Theme.faint
        font.pixelSize: Theme.uiCaption
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: {
            if (row.nameTip)
                row.nameTip.dismiss()
            rowTip.dismiss()
        }
        // The wheel hides the tip even where the list does not scroll (Windows' own tips do);
        // passed on, so the list still scrolls.
        onWheel: (wheel) => {
            if (row.nameTip)
                row.nameTip.dismiss()
            rowTip.dismiss()
            wheel.accepted = false
        }
        onPositionChanged: if (!pressed) row.offerTip() // moved: a tip dismissed (a key, a scroll) may come again
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                row.menuRequested(row.index, area.mapToGlobal(mouse.x, mouse.y))
            else
                row.clicked(row.index)
        }
    }

    HoverTip {
        id: rowTip
        objectName: "dialogRowTip"
        dpr: row.dpr
        host: row.tipHost
        readonly property bool onKeys: numberSlot.visible && numberSlot.opacity > 0
                                       && (row.hint > 0 || row.hintLetter.length > 0)
                                       && area.containsMouse && !area.pressed
                                       && numberSlot.contains(numberSlot.mapFromItem(area, area.mouseX, area.mouseY))
        target: onKeys ? numberSlot : null
        text: !onKeys ? ""
            : row.hintLetter.length === 0 && row.hintTaken
                ? qsTr("%1 是你给 Win顺 设的快捷键，在这里按也是那个快捷键。可在设置的“结果列表”里改用 %2")
                  .arg(row.hintModifier + "+" + row.hint).arg(row.hintModifier === "Alt" ? "Ctrl" : "Alt")
            : row.copyOnly ? qsTr("复制这一行路径")
            : !row.isDir && row.canOpen ? qsTr("打开这一行") : qsTr("转到这一行")
        shortcut: !onKeys ? ""
                : row.hintLetter.length > 0
                    ? (row.hintArmed ? row.hintLetter : qsTr("轻按 %1 后按 %2").arg(row.hintJumpKey).arg(row.hintLetter))
                : !row.hintTaken ? row.hintModifier + "+" + row.hint : ""
    }
}
