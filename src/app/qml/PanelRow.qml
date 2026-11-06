pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// One row of the search panel (Panel.qml): a line in columns, as a file
// manager lists files: icon and name, the folder it is in (an app: where it
// comes from; a content match: the matching line), its size, when it was
// last written. The columns' places come from the panel (its header shows
// them). Clicks, the menu, dragging out and the selection work as on a
// ResultRow; the buttons a ResultRow shows are the menu's here.
Item {
    id: row

    required property int index
    required property string name
    required property string nameHtml
    required property var nameMatches
    required property string folder
    required property var folderMatches
    required property string path
    required property bool isDir
    required property string icon
    required property string snippetHtml
    required property int line
    required property string location
    required property bool isApp
    required property bool place
    required property bool web
    required property bool plugin
    required property string source
    required property bool selected
    required property string size // "1.2 MB"; empty for a folder, an app, or not known
    required property string modifiedDate // "2026-10-14 14:32"; empty when not known
    required property string createdDate
    required property string typeName
    required property string folderName
    required property bool offline
    required property bool missing
    required property bool favorite
    required property bool recent
    // Where the columns start and how wide they are (Panel.qml's header).
    property var columns: []
    property var columnLayout: ({})
    function columnX(id) { return columnLayout[id] ? columnLayout[id].x + 12 : 0 }
    function columnWidth(id) { return columnLayout[id] ? columnLayout[id].width - 24 : 0 }
    readonly property real nameX: columnX("name")
    readonly property real pathX: columnX("path")
    readonly property real nameWidth: columnWidth("name")
    readonly property real pathWidth: columnWidth("path")
    property NameTip nameTip: null // the window's tip for a name or folder cut short

    readonly property bool contentMode: line > 0
    readonly property bool current: ListView.isCurrentItem
    readonly property int selectedCount: ListView.view && ListView.view.model ? ListView.view.model.selectedCount : 0
    readonly property var results: ListView.view ? ListView.view.model : null
    // The window's device pixel ratio, passed by the panel: one Screen
    // attached object for all the rows (GitHub #111).
    property real dpr: 1
    // The style's own icon for a file's kind (Theme.typeIcon; null: the
    // system's). Apps, programs and links always keep the system's.
    readonly property string typeKind: Theme.typeIcon === null || isApp ? "" : isDir ? "folder" : SearchKeys.fileKind(path)
    readonly property bool styledIcon: typeKind.length > 0

    signal clicked(int index, int modifiers)
    signal doubleClicked(int index, int modifiers) // as ResultRow's
    signal menuRequested(int index, point globalPos, int modifiers)
    signal dragRequested(int index)
    signal previewRequested(int index) // as ResultRow's
    signal hoverMoved(int index, point globalPos)

    function onPixel(y) { return Math.round(y * row.dpr) / row.dpr }

    // As ResultRow's: the whole name and folder (NameTip) when either is cut,
    // or the folder is not shown (a content match shows its line there).
    function cut(shown: string, whole: string): bool {
        return shown.split("…").length > whole.split("…").length
    }
    readonly property bool nameCut: title.truncated || cut(title.text, name)
    readonly property bool folderCut: pathWidth > 0 && (contentMode ? folder.length > 0
                                    : folderText.truncated || (folderText.marked && cut(folderText.text, folder)))
    readonly property bool wantsTip: nameTip !== null && (nameCut || folderCut) && area.containsMouse && !area.pressed
                                     && ((area.mouseX >= nameX && area.mouseX < nameX + nameWidth)
                                         || (area.mouseX >= pathX && area.mouseX < pathX + pathWidth))
    onWantsTipChanged: offerTip()
    function offerTip() {
        if (!nameTip)
            return
        if (!wantsTip) {
            nameTip.withdraw(row)
            return
        }
        nameTip.offer(row, title, {
            name: name, folder: folder, path: path, size: size, modified: modifiedDate,
            onDisk: !isApp && !offline && !missing, at: area.mapToGlobal(area.mouseX, area.mouseY)
        })
    }
    function dropTip() {
        if (nameTip && nameTip.source === row)
            nameTip.dismiss()
    }
    onPathChanged: dropTip() // delegate reused for another result
    ListView.onPooled: dropTip()
    Component.onDestruction: dropTip()

    // The current row's highlight is the panel's list's own (one for all the
    // rows, Panel.qml); this one is under the others the mouse is on or that
    // are chosen: made the first time it shows, then only hidden (rows pass
    // under a still mouse while the list scrolls, docs/pitfalls.md).
    Hook {
        anchors.fill: parent
        anchors.leftMargin: Theme.rowInset
        anchors.rightMargin: Theme.rowInset
        visible: !row.current && (row.selected || area.containsMouse)
        lazy: true
        part: Theme.rowHighlight
        selected: row.selected
        hovered: area.containsMouse
        role: "panel"
        dpr: row.dpr
    }

    Image { // 20 logical pixels: 30 device pixels at 150 %, its own pixels unsmoothed as the box's icons
        id: iconImage

        readonly property real dpr: row.dpr
        readonly property int pixels: Math.round(20 * dpr)

        visible: !row.styledIcon
        x: row.nameX + 1 / 64
        y: row.onPixel((row.height - height) / 2) + 1 / 64
        width: pixels / dpr
        height: pixels / dpr
        opacity: row.offline || row.missing ? 0.6 : 1
        source: row.styledIcon ? "" : row.icon
        sourceSize.width: pixels / dpr
        sourceSize.height: pixels / dpr
        smooth: false
        asynchronous: true
        fillMode: Image.PreserveAspectFit
    }
    Loader { // the style's own file icon (none in fluent: the system's above)
        x: iconImage.x
        y: iconImage.y
        width: iconImage.width
        height: iconImage.height
        opacity: iconImage.opacity
        active: row.styledIcon
        sourceComponent: Hook {
            part: Theme.typeIcon
            kind: row.typeKind
            size: iconImage.width
            hueIndex: row.typeKind === "folder" ? -2 : -1
            dpr: row.dpr
        }
    }

    Text { // the name, shortened around its matches
        id: title
        x: row.nameX + 28
        width: Math.max(0, row.nameWidth - 28)
        y: row.onPixel((row.height - height) / 2)
        opacity: row.offline || row.missing ? 0.6 : 1
        text: row.results && !row.isApp // the end kept, matches or none (see ResultRow)
              ? row.results.elidedHtml(row.name, row.nameMatches, width, font, row.results.highlightColor, row.results.highlightWeight)
              : row.nameHtml
        textFormat: Text.StyledText
        elide: Text.ElideRight
        maximumLineCount: 1
        color: Theme.text
        font.pixelSize: Theme.uiTitle
    }

    Text { // the folder (or the matching line; an app's origin), its matches in sight
        id: folderText
        readonly property bool marked: !row.contentMode && row.folderMatches.length > 0 && row.results !== null
        visible: row.pathWidth > 0
        x: row.pathX
        width: Math.max(0, row.pathWidth)
        y: row.onPixel((row.height - height) / 2)
        text: row.contentMode ? row.snippetHtml
            : marked ? row.results.elidedHtml(row.folder, row.folderMatches, width, font, row.results.highlightColor, row.results.highlightWeight)
            : row.folder
        textFormat: row.contentMode || marked ? Text.StyledText : Text.PlainText
        elide: row.contentMode || marked ? Text.ElideRight : Text.ElideMiddle
        maximumLineCount: 1
        color: Theme.subtext
        font.pixelSize: Theme.uiBody
    }

    Repeater {
        model: row.columns
        delegate: Text {
            required property var modelData
            readonly property string columnId: modelData.id
            visible: columnId !== "name" && columnId !== "path"
            x: row.columnX(columnId)
            width: Math.max(0, row.columnWidth(columnId))
            y: row.onPixel((row.height - height) / 2)
            horizontalAlignment: columnId === "size" ? Text.AlignRight : Text.AlignLeft
            text: {
                switch (columnId) {
                case "size":
                    return row.plugin ? row.size
                        : row.contentMode ? row.location : row.isApp ? ""
                        : row.offline ? qsTr("离线") : row.missing ? qsTr("已不存在") : row.size
                case "modified": return row.modifiedDate
                case "created": return row.createdDate
                case "type": return row.typeName
                case "folder": return row.folderName
                case "source": return row.source
                default: return ""
                }
            }
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.subtext
            font.pixelSize: Theme.uiBody
        }
    }

    MouseArea {
        id: area

        // As ResultRow: held and moved past the drag distance, the file is
        // dragged out of the window.
        property point pressedAt: Qt.point(0, 0)
        property bool draggedOut: false

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
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
                row.hoverMoved(row.index, area.mapToGlobal(mouse.x, mouse.y))
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
            if (draggedOut)
                return
            if (mouse.button === Qt.RightButton)
                row.menuRequested(row.index, area.mapToGlobal(mouse.x, mouse.y), mouse.modifiers)
            else if (mouse.button === Qt.MiddleButton)
                row.previewRequested(row.index)
            else
                row.clicked(row.index, mouse.modifiers)
        }
        onDoubleClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton)
                row.doubleClicked(row.index, mouse.modifiers)
            else if (mouse.button === Qt.MiddleButton) // a second middle click, as quick: closes again
                row.previewRequested(row.index)
        }
    }
}
