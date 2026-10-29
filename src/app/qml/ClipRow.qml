import QtQuick
import WinShun

// One clipboard history entry: what it is (a picture's thumbnail, a file's
// icon, a colour's swatch, or a sign for text and links), its first line
// (or where the query matched), where it came from and when. A click
// shows it in the preview; its paste button or a double click pastes it
// (ClipboardPage.qml). With clickToPaste a click pastes it, as in Windows'
// own history, and that button shows the preview instead. The first nine show on the icon the number that
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
    property int hint: 0 // the number that pastes this row (Alt+1...), on its icon; 0 for none
    property string hintModifier: "Alt" // ... with this held
    property bool hintTaken: false // another program holds that chord as a global hotkey: shown struck through
    property real hintOpacity: 1
    property string pasteTip: qsTr("粘贴") // where the paste button pastes ("粘贴到搜索框")
    property bool clickToPaste: false // a click pastes (Clipboard::clickToPaste)
    property bool largePictures: false // a picture's thumbnail larger (Clipboard::largePictures)

    // Thumbnail layout is independent of the clipboard kind (and its paste/drag actions).
    readonly property bool picture: kind === 4 || image.length > 0
    readonly property bool current: ListView.isCurrentItem
    readonly property int selectedCount: ListView.view ? ListView.view.model.selectedCount : 0
    readonly property bool showActions: current || area.containsMouse || moveAction.held

    // Out of sight while in the view's reuse pool. Qt (6.12) leaves a pooled
    // row drawn where it was, and taking the mouse, when a search drops rows
    // in several separate runs and the list declares a transition
    // (docs/pitfalls.md: 搜索后列表下方留着旧行). Shown again when reused.
    ListView.onPooled: row.visible = false
    ListView.onReused: row.visible = true

    signal clicked(int index, int modifiers)
    signal pasteRequested(int index) // its paste button, a double click
    signal previewRequested(int index) // that button, with clickToPaste
    signal dragRequested(int index)
    signal moveRequested(int from, int to) // its handle dragged over row `to`
    signal moveHeld(bool held) // its handle pressed, let go
    signal menuRequested(int index, point globalPos)
    signal actionRequested(int index, int action)
    signal hoverMoved(int index, point globalPos) // the mouse moved over it (not pressed)

    // Text on whole device pixels (see ResultRow).
    function onPixel(y) { return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio }
    function upToPixel(y) { return Math.ceil(y * Screen.devicePixelRatio - 1e-6) / Screen.devicePixelRatio }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: Theme.largeControlRadius
        color: row.order > 0 || (row.current && row.selectedCount === 0) ? Theme.selection
               : area.containsMouse ? Theme.hover : "transparent"
    }

    Rectangle { // "固定" listed first: a line where the rest begin
        visible: row.afterPinned
        x: 16
        // On the edge toward those: the bottom one in a list laid out bottom
        // to top (the clipboard turned over, GitHub #38).
        y: row.ListView.view?.verticalLayoutDirection === ListView.BottomToTop ? parent.height - height : 0
        width: parent.width - 32
        height: Theme.edge(Screen.devicePixelRatio)
        color: Theme.divider
    }

    Rectangle { // accent pill on the current row
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
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        // Pictures larger, on a taller row (ClipboardPage.pictureRowHeight):
        // what is in them shows (GitHub #9); 108 x 68 holds a 16:10 screen whole,
        // 208 x 130 (largePictures) its details too (forum #137).
        width: !row.picture ? 32 : row.largePictures ? 208 : 108
        height: !row.picture ? 32 : row.largePictures ? 130 : 68

        Rectangle { // text and links: a sign on a soft tile
            anchors.fill: parent
            visible: row.kind <= 1 && row.swatch.length === 0
            radius: 6
            color: Theme.badge

            Glyph {
                anchors.centerIn: parent
                glyph: row.kind === 1 ? "" : "" // Link, Font
                size: 16
                color: Theme.accent
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
            readonly property int pixels: Math.round(32 * dpr)

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
                // pixels: sharp.
                x: Math.round((parent.width - width) / 2 * dpr) / dpr
                y: Math.round((parent.height - height) / 2 * dpr) / dpr
                width: ready ? implicitWidth / dpr : parent.width
                height: ready ? implicitHeight / dpr : parent.height
                visible: ready
                source: row.image
                cache: false // keep pixels only while this delegate needs them
                // An image file: sourceSize is in the image's own pixels.
                sourceSize.width: Math.round(parent.width * dpr)
                sourceSize.height: Math.round(parent.height * dpr)
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
                font.weight: Font.DemiBold
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
        anchors.rightMargin: (row.showActions ? actions.width + actions.anchors.rightMargin
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
            font.pixelSize: Theme.uiTitle
        }

        Text {
            width: parent.width
            text: row.detail
            textFormat: Text.PlainText
            elide: Text.ElideRight
            maximumLineCount: 1
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
    }

    Rectangle { // in a group: which; else a screenshot says so, quieter
        id: badge
        visible: (row.groupName.length > 0 || row.screenshot) && !row.showActions
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: badgeRow.implicitWidth + 16
        height: Theme.badgeHeight
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
                font.pixelSize: Theme.uiCaption
            }
        }
    }

    IconNumber { // "Alt+1" (or "Ctrl+1"): pastes this row
        id: iconNumber
        opacity: row.hintOpacity
        // From the top left corner of the icon, or of the picture as drawn
        // (narrower or flatter than its slot), rightwards, as in the launcher.
        readonly property bool picture: row.picture && thumbnail.ready
        keys: row.hint > 0 ? row.hintModifier + "+" + row.hint : ""
        taken: row.hintTaken
        x: row.onPixel(iconSlot.x + (picture ? thumbnail.x : fileIcon.visible ? fileIcon.x : 0) - 5)
        y: row.onPixel(iconSlot.y + (picture ? thumbnail.y : fileIcon.visible ? fileIcon.y : 0) - 4)
    }

    MouseArea {
        id: area
        // Real mouse drags export images; touch still scrolls the list.
        property point pressedAt: Qt.point(0, 0)
        property bool draggedOut: false
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: (mouse) => {
            draggedOut = false
            pressedAt = Qt.point(mouse.x, mouse.y)
            preventStealing = row.kind === 4 && mouse.button === Qt.LeftButton
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
        }

        Row {
            id: actions
            visible: row.showActions
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            RowAction { // dragged up or down: to another place in its group
                id: moveAction
                visible: row.movable
                draggable: true
                glyph: "" // GripperBarHorizontal
                tip: qsTr("拖动调整顺序")
                tipShortcut: "Ctrl+↑/↓"
                onHeldChanged: row.moveHeld(held)
                onDragMoved: (position) => {
                    const view = row.ListView.view
                    if (!view)
                        return
                    const p = moveAction.mapToItem(view.contentItem, position.x, position.y)
                    const to = view.indexAt(row.width / 2, p.y)
                    const over = to >= 0 && to !== row.index ? view.itemAtIndex(to) : null
                    // Only where the mouse would be on this row once moved:
                    // rows of other heights (pictures) would swap it back.
                    // Below it or above it on the screen, whichever way the
                    // list is laid out.
                    if (over && (over.y > row.y ? p.y >= over.y + over.height - row.height : p.y < over.y + row.height))
                        row.moveRequested(row.index, to)
                }
            }
            RowAction { // a click pastes already (clickToPaste): the preview here instead
                id: pasteAction
                glyph: row.clickToPaste ? "" : "" // View, Paste
                tip: row.clickToPaste ? qsTr("预览") : row.pasteTip
                tipShortcut: row.clickToPaste ? "Alt+P" : "Enter"
                onClicked: row.clickToPaste ? row.previewRequested(row.index) : row.pasteRequested(row.index)
            }
            RowAction {
                id: pinAction
                glyph: row.pinned ? "" : "" // Unpin, Pin
                tip: row.pinned ? qsTr("取消固定") : qsTr("固定（不会过期）")
                tipShortcut: "Ctrl+P"
                onClicked: row.actionRequested(row.index, Clipboard.Pin)
            }
            RowAction {
                id: removeAction
                glyph: "" // Delete
                tip: qsTr("删除")
                tipShortcut: "Delete"
                onClicked: row.actionRequested(row.index, Clipboard.Remove)
            }
        }
    }

    HoverTip {
        readonly property RowAction hovered: moveAction.tipWanted ? moveAction
                                           : pasteAction.tipWanted ? pasteAction
                                           : pinAction.tipWanted ? pinAction
                                           : removeAction.tipWanted ? removeAction : null
        readonly property bool onKeys: !hovered && iconNumber.visible && iconNumber.taken && area.containsMouse // (a hover handler under the area gets no hover)
                                          && iconNumber.contains(iconNumber.mapFromItem(area, area.mouseX, area.mouseY))
        readonly property bool onMark: !hovered && !onKeys && pastedMark.visible && area.containsMouse
                                       && pastedMark.contains(pastedMark.mapFromItem(area, area.mouseX, area.mouseY))
        target: hovered ? hovered : onKeys ? iconNumber : onMark ? pastedMark : null
        text: hovered ? hovered.tip
            : onMark ? qsTr("已粘贴过")
            : onKeys ? qsTr("%1 被别的程序占用了，在这里按也会交给那个程序。可在设置里改用 %2")
                       .arg(iconNumber.keys).arg(row.hintModifier === "Alt" ? "Ctrl" : "Alt")
            : ""
        shortcut: hovered ? hovered.tipShortcut : ""
    }
}
