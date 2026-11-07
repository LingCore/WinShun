import QtQuick
import WinShun

// One clipboard history entry: what it is (a picture's thumbnail, a file's
// icon, a colour's swatch, or a sign for text and links), its first line
// (or where the query matched), where it came from and when. A click
// shows it in the preview; its paste button or a double click pastes it
// (ClipboardPage.qml). With clickToPaste a click pastes it, as in Windows'
// own history, and that button shows the preview instead. The first nine have a separate number column that
// pastes them (Alt+1…); picked rows show the order they will be pasted in;
// the current and the hovered row show buttons: paste, pin, delete.
Item {
    id: row

    required property int index
    required property int clipId
    required property int kind // ClipKind: 0 text, 1 link, 2 path, 3 files, 4 image
    required property string title
    required property string detail
    required property string icon
    required property string image
    required property string swatch
    required property int order // picked: 1, 2, ...
    required property string groupName
    required property bool pinned
    required property bool missing
    required property bool movable // in its group's own order: a handle to drag, Ctrl+Up/Down
    required property bool afterPinned // the first after those in "固定" listed first
    required property bool screenshot // a picture Win顺's capture took: tagged 截图 (when in no group)
    required property bool pasted // pasted from the list before, with 粘贴后保持位置 on (else the last pasted is on top): a check mark
    property int hint: 0 // the number that pastes this row (Alt+1...); 0 for none
    property bool reserveHintSpace: false // keep one shared gutter, including rows beyond the nine numbered entries
    property string hintModifier: "Alt" // ... with this held
    property bool hintTaken: false // one of Win顺's own shortcuts has that chord: shown struck through
    property real hintOpacity: 1
    property string pasteTip: qsTr("粘贴") // where the paste button pastes ("粘贴到搜索框")
    property bool clickToPaste: false // a click pastes (Clipboard::clickToPaste)
    property bool largePictures: false // a picture's thumbnail larger (Clipboard::largePictures)
    property UiScale metrics: Theme.metrics

    // Thumbnail layout is independent of the clipboard kind (and its paste/drag actions).
    readonly property int iconSize: metrics.dense ? 24 : 32 // 密集: one line, a smaller icon
    readonly property bool picture: kind === 4 || image.length > 0
    readonly property bool current: ListView.isCurrentItem
    readonly property int selectedCount: ListView.view ? ListView.view.model.selectedCount : 0
    // ... also while its handle is held: dragged, the row keeps them. Told by
    // the lent buttons (not read from them: which set the row has follows this).
    property bool handleHeld: false
    readonly property bool showActions: current || area.containsMouse || handleHeld

    // The buttons: a set the list lends while the row shows them
    // (RowActionsPool, ClipActions), not one made in every row.
    property RowActionsPool actionsPool: null
    property Item borrowed: null
    readonly property Item actions: borrowed !== null && borrowed.host === row ? borrowed : null
    readonly property Item actionsArea: area // where a lent set sits
    onShowActionsChanged: borrowActions()
    function borrowActions() {
        if (!actionsPool)
            return
        if (showActions) {
            borrowed = actionsPool.take(row)
        } else {
            actionsPool.give(row)
            borrowed = null
        }
    }
    function giveActions() {
        if (actionsPool)
            actionsPool.give(row)
        borrowed = null
        handleHeld = false
    }
    Component.onCompleted: borrowActions()
    Component.onDestruction: giveActions()

    // Out of sight while in the view's reuse pool. Qt (6.12) leaves a pooled
    // row drawn where it was, and taking the mouse, when a search drops rows
    // in several separate runs and the list declares a transition
    // (docs/pitfalls.md: 搜索后列表下方留着旧行). Shown again when reused.
    ListView.onPooled: {
        row.visible = false
        giveActions()
    }
    ListView.onReused: {
        row.visible = true
        borrowActions()
    }

    // Its handle (ClipActions) dragged to `position`, in the handle's coordinates.
    function handleDragged(handle: Item, position: point) {
        const view = row.ListView.view
        if (!view)
            return
        const p = handle.mapToItem(view.contentItem, position.x, position.y)
        const to = view.indexAt(row.width / 2, p.y)
        const over = to >= 0 && to !== row.index ? view.itemAtIndex(to) : null
        // Only where the mouse would be on this row once moved:
        // rows of other heights (pictures) would swap it back.
        // Below it or above it on the screen, whichever way the
        // list is laid out.
        if (over && (over.y > row.y ? p.y >= over.y + over.height - row.height : p.y < over.y + row.height))
            row.moveRequested(row.index, to)
    }

    signal clicked(int index, int modifiers)
    signal pasteRequested(int index) // its paste button, a double click
    signal previewRequested(int index) // that button, with clickToPaste; a middle click
    signal hoverLeft(int index)
    signal dragRequested(int index)
    signal moveRequested(int from, int to) // its handle dragged over row `to`
    signal moveHeld(bool held) // its handle pressed, let go
    signal menuRequested(int index, point globalPos)
    signal actionRequested(int index, int action)
    signal hoverMoved(int index, point globalPos) // the mouse moved over it (not pressed)

    // The window's, read once here (GitHub #111); text on whole device pixels (see ResultRow).
    readonly property real dpr: Screen.devicePixelRatio
    function onPixel(y) { return Math.round(y * dpr) / dpr }
    function upToPixel(y) { return Math.ceil(y * dpr - 1e-6) / dpr }

    // The style's highlight (Theme.rowHighlight). The current row's is the
    // list's own, one for all its rows (ClipboardPage); here only the hover
    // and the picked rows it does not cover. Made the first time it shows,
    // then only hidden: rows pass under a still mouse while the list
    // scrolls, and making the part on each was most of the styles' cost
    // there (a dropped frame at 240 Hz).
    readonly property bool sharedSelected: current && (selectedCount === 0 || order > 0)
    Hook {
        anchors.fill: parent
        anchors.leftMargin: Theme.rowInset
        anchors.rightMargin: Theme.rowInset
        anchors.topMargin: Theme.edge(row.dpr)
        anchors.bottomMargin: Theme.edge(row.dpr)
        visible: !row.sharedSelected && (row.order > 0 || area.containsMouse)
        lazy: true
        part: Theme.rowHighlight
        selected: row.order > 0
        hovered: area.containsMouse
        role: "list"
        dpr: row.dpr
    }

    Hook { // "固定" listed first: a line where the rest begin (the style's seam)
        visible: row.afterPinned
        lazy: true // one row has it
        x: 16
        // On the edge toward those: the bottom one in a list laid out bottom
        // to top (the clipboard turned over, GitHub #38).
        y: row.ListView.view?.verticalLayoutDirection === ListView.BottomToTop ? parent.height - height : 0
        width: parent.width - 32
        height: Theme.edge(row.dpr)
        part: Theme.separator
        dpr: row.dpr
    }

    Rectangle {
        objectName: "clipboardRowSeparator"
        visible: row.ListView.view !== null && row.index < row.ListView.view.count - 1
        x: iconSlot.x
        y: row.ListView.view?.verticalLayoutDirection === ListView.BottomToTop ? 0 : parent.height - height
        width: Math.max(0, parent.width - x - 16)
        height: Theme.edge(row.dpr)
        color: Theme.divider
    }

    Item {
        id: iconSlot
        objectName: "clipboardIconSlot"
        x: 16 + (row.reserveHintSpace ? 28 : 0)
        anchors.verticalCenter: parent.verticalCenter
        // Pictures larger, on a taller row (ClipboardPage.pictureRowHeight):
        // what is in them shows (GitHub #9); 108 x 68 holds a 16:10 screen whole,
        // 208 x 130 (largePictures) its details too (forum #137).
        width: !row.picture ? row.iconSize : row.largePictures ? 208 : 108
        height: !row.picture ? row.iconSize : row.largePictures ? 130 : 68

        Rectangle { // text and links: a sign on a soft tile
            anchors.fill: parent
            visible: row.kind <= 1 && row.swatch.length === 0
            radius: Theme.largeControlRadius
            color: row.kind === 1 ? Theme.tok("clipLinkTint", Theme.badge) : Theme.tok("clipTextTint", Theme.badge) // a style may tint it as its file icons

            Glyph {
                anchors.centerIn: parent
                glyph: row.kind === 1 ? "" : "" // Link, Font
                size: 16
                color: row.kind === 1 ? Theme.tok("clipLinkInk", Theme.accent) : Theme.tok("clipTextInk", Theme.accent)
            }
        }

        ColorSwatch { // a colour value: the colour, a translucent one half over a checkerboard
            anchors.fill: parent
            visible: row.swatch.length > 0
            color: visible ? row.swatch : "transparent"
            radius: Theme.cardRadius
            borderColor: Theme.chipBorder
            cellSize: 4
            checkerColor: Theme.checker
            checkerAltColor: Theme.checkerAlt
        }

        Image { // files and paths: the icon Explorer shows
            id: fileIcon
            objectName: "clipboardFileIcon"

            readonly property real dpr: Screen.devicePixelRatio
            readonly property int pixels: Math.round(row.iconSize * dpr)

            visible: (row.kind === 2 || row.kind === 3) && !thumbnail.ready
            x: Math.round((parent.width - width) / 2 * dpr) / dpr + 1 / 64
            y: Math.round((parent.height - height) / 2 * dpr) / dpr + 1 / 64
            width: pixels / dpr
            height: pixels / dpr
            source: visible ? row.icon : ""
            sourceSize.width: pixels / dpr // image provider: logical pixels
            sourceSize.height: pixels / dpr
            smooth: false
            asynchronous: true
            opacity: row.missing ? 0.45 : 1
        }

        Item { // pictures: a thumbnail, all of it, as large as the slot holds
            anchors.fill: parent
            visible: row.picture

            Rectangle { // while it loads
                anchors.fill: parent
                visible: row.kind === 4 && !thumbnail.ready
                radius: Theme.controlRadius
                color: Theme.track
            }

            Image {
                id: thumbnail
                objectName: "clipboardThumbnail"

                readonly property real dpr: Screen.devicePixelRatio
                readonly property bool ready: status === Image.Ready && implicitWidth > 0

                // Scaled once, when loaded, to fit the slot (sourceSize: both
                // set, the aspect kept), then drawn pixel for pixel, on whole
                // pixels: sharp. Kept scaled (ClipThumbnailProvider), so a row
                // scrolled back to shows its picture at once.
                x: 0 // the image's leading edge follows the same gutter as text/file icons
                y: Math.round((parent.height - height) / 2 * dpr) / dpr
                width: ready ? implicitWidth : parent.width
                height: ready ? implicitHeight : parent.height
                visible: ready
                // Not while it is pooled (the roles are undefined then, which a
                // source takes for the file "undefined"): a source has a scheme.
                source: row.image.indexOf(":") > 0 ? row.image : ""
                // An image provider: sourceSize in logical pixels (Qt asks
                // for it times the device pixel ratio); the implicit size is
                // its pixels over the ratio.
                sourceSize.width: parent.width
                sourceSize.height: parent.height
                asynchronous: true
                autoTransform: true
                smooth: false
            }
        }

        Rectangle { // picked: the order it is pasted in
            id: orderBadge
            visible: row.order > 0
            x: -7
            y: -6
            width: Math.max(18, orderText.implicitWidth + 8)
            height: 18
            radius: Theme.pillRadius(height)
            color: Theme.accentFill
            border.width: 2
            border.color: Theme.background

            CenteredNumber {
                id: orderText
                text: row.order
                color: Theme.accentText
                font.pixelSize: 12
                font.weight: Theme.fontStrong
            }
        }

        Rectangle { // pasted before: a white check on a blue disc, ringed like the order badge
            id: pastedMark
            objectName: "clipboardPastedMark"
            visible: row.pasted
            // Whole device pixels at 100%, 150% and 200%: even logical sizes.
            x: parent.width - 12
            y: parent.height - 12
            width: 20
            height: 20
            radius: width / 2
            color: Theme.accentFill
            border.width: 2
            border.color: Theme.background
            Accessible.role: Accessible.Graphic
            Accessible.name: qsTr("已粘贴过")

            Glyph {
                anchors.centerIn: parent
                glyph: String.fromCharCode(0xE73E) // CheckMark
                size: 12
                color: Theme.accentText
            }
        }
    }

    Column {
        y: row.onPixel((row.height - height) / 2)
        anchors.left: iconSlot.right
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: (row.showActions ? (row.actions !== null ? row.actions.actionsWidth : 0) + 14
                              : badge.visible ? badge.width + badge.anchors.rightMargin : 8) + 8
        spacing: 4

        Text {
            width: parent.width
            height: row.upToPixel(implicitHeight)
            text: row.title
            textFormat: Text.StyledText // highlighted; escaped in C++
            elide: Text.ElideRight
            maximumLineCount: 1
            color: row.missing ? Theme.subtext : Theme.text
            font.pixelSize: row.metrics.uiTitle
        }

        Text {
            visible: !row.metrics.dense
            width: parent.width
            text: row.detail
            textFormat: Text.PlainText
            elide: Text.ElideRight
            maximumLineCount: 1
            color: Theme.subtext
            font.pixelSize: row.metrics.uiCaption
        }
    }

    Rectangle { // in a group: which; else a screenshot says so, quieter
        id: badge
        visible: (row.groupName.length > 0 || row.screenshot) && !row.showActions
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: badgeRow.implicitWidth + 16
        height: row.metrics.badgeHeight
        radius: Theme.pillRadius(height)
        color: Theme.badge

        Row {
            id: badgeRow
            anchors.centerIn: parent
            spacing: 4

            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                visible: row.pinned
                glyph: "" // Pin
                size: 12
                color: Theme.accent
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: row.groupName.length > 0 ? row.groupName : qsTr("截图")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                width: Math.min(implicitWidth, 96)
                color: row.groupName.length > 0 ? Theme.accent : Theme.subtext
                font.pixelSize: row.metrics.uiCaption
            }
        }
    }

    Item {
        id: numberSlot
        objectName: "clipboardNumberSlot"
        visible: row.reserveHintSpace && row.hint > 0
        opacity: row.hintOpacity
        x: row.onPixel(Theme.tok("clipHintX", 14))
        width: 24
        height: row.height

        CenteredNumber {
            objectName: "clipboardNumber"
            text: row.hint > 0 ? String(row.hint) : ""
            font.pixelSize: row.metrics.uiTitle
            font.weight: Font.Medium
            font.strikeout: row.hintTaken
            color: row.hintTaken ? Theme.faint : row.current ? Theme.accent : Theme.subtext
            Accessible.ignored: true
        }
    }

    MouseArea {
        id: area
        // Real mouse drags take rows out as files (Clipboard::dragOut); touch still scrolls the list.
        property point pressedAt: Qt.point(0, 0)
        property bool draggedOut: false
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        onExited: row.hoverLeft(row.index)
        onPressed: (mouse) => {
            draggedOut = false
            pressedAt = Qt.point(mouse.x, mouse.y)
            preventStealing = mouse.button === Qt.LeftButton
                              && mouse.source === Qt.MouseEventNotSynthesized
        }
        onPositionChanged: (mouse) => {
            if (!pressed)
                row.hoverMoved(row.index, area.mapToGlobal(mouse.x, mouse.y))
            if (!pressed || !preventStealing || draggedOut || !(mouse.buttons & Qt.LeftButton))
                return
            if (Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y) < Application.styleHints.startDragDistance)
                return
            draggedOut = true
            row.dragRequested(row.index)
        }
        onClicked: (mouse) => {
            if (draggedOut)
                return
            if (mouse.button === Qt.RightButton)
                row.menuRequested(row.index, area.mapToGlobal(mouse.x, mouse.y))
            else if (mouse.button === Qt.MiddleButton)
                row.previewRequested(row.index)
            else
                row.clicked(row.index, mouse.modifiers)
        }
        onDoubleClicked: (mouse) => {
            if (draggedOut)
                return
            // With clickToPaste the first click pasted it already.
            if (mouse.button === Qt.LeftButton && (mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) === 0
                && !row.clickToPaste)
                row.pasteRequested(row.index)
            else if (mouse.button === Qt.MiddleButton)
                row.previewRequested(row.index)
        }
    }

    HoverTip {
        dpr: row.dpr
        readonly property RowAction hovered: row.actions !== null ? row.actions.hovered : null
        readonly property bool onKeys: !hovered && numberSlot.visible && numberSlot.opacity > 0 && row.hintTaken && area.containsMouse // (a hover handler under the area gets no hover)
                                          && numberSlot.contains(numberSlot.mapFromItem(area, area.mouseX, area.mouseY))
        readonly property bool onMark: !hovered && !onKeys && pastedMark.visible && area.containsMouse
                                       && pastedMark.contains(pastedMark.mapFromItem(area, area.mouseX, area.mouseY))
        target: hovered ? hovered : onKeys ? numberSlot : onMark ? pastedMark : null
        text: hovered ? hovered.tip
            : onMark ? qsTr("已粘贴过")
            : onKeys ? qsTr("%1 是你给 Win顺 设的快捷键，在这里按也是那个快捷键。可在设置里改用 %2")
                       .arg(row.hintModifier + "+" + row.hint).arg(row.hintModifier === "Alt" ? "Ctrl" : "Alt")
            : ""
        shortcut: hovered ? hovered.tipShortcut : ""
    }
}
