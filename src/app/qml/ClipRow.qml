import QtQuick
import WinShun

// One clipboard history entry: what it is (a picture's thumbnail, a file's
// icon, a colour's swatch, or a sign for text and links), its first line
// (or where the query matched), where it came from and when. A click
// pastes it (ClipboardPage.qml). Picked rows show the order they will be
// pasted in; the current and the hovered row show buttons: preview, pin,
// delete.
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
    property int hint: 0 // its modifier held: the number that pastes this row (Alt+1...), 0 for none
    property bool previewing: false // its preview is open beside the list

    readonly property bool current: ListView.isCurrentItem
    readonly property int selectedCount: ListView.view ? ListView.view.model.selectedCount : 0
    readonly property bool showActions: (current || area.containsMouse) && hint === 0

    signal clicked(int index, int modifiers)
    signal previewRequested(int index)
    signal menuRequested(int index, point globalPos)
    signal actionRequested(int index, int action)

    // Text on whole device pixels (see ResultRow).
    function onPixel(y) { return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio }
    function upToPixel(y) { return Math.ceil(y * Screen.devicePixelRatio - 1e-6) / Screen.devicePixelRatio }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: 6
        color: row.order > 0 || (row.current && row.selectedCount === 0) ? Theme.selection
               : area.containsMouse ? Theme.hover : "transparent"
    }

    Rectangle { // accent pill on the current row
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
        width: 32
        height: 32

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
            radius: 6
            borderColor: Theme.chipBorder
            cellSize: 4
            checkerColor: Theme.checker
            checkerAltColor: Theme.checkerAlt
        }

        Image { // files and paths: the icon Explorer shows
            id: fileIcon

            readonly property real dpr: Screen.devicePixelRatio
            readonly property int pixels: Math.round(iconSlot.width * dpr)

            visible: row.kind === 2 || row.kind === 3
            x: 1 / 64
            y: 1 / 64
            width: pixels / dpr
            height: pixels / dpr
            source: visible ? row.icon : ""
            sourceSize.width: pixels / dpr // image provider: logical pixels
            sourceSize.height: pixels / dpr
            smooth: false
            asynchronous: true
            opacity: row.missing ? 0.45 : 1
        }

        Rectangle { // pictures: a thumbnail, cut to a square
            anchors.fill: parent
            visible: row.kind === 4
            radius: 4
            color: Theme.track
            clip: true

            Image {
                anchors.fill: parent
                source: row.kind === 4 ? row.image : ""
                // A PNG file: sourceSize is in the image's own pixels.
                sourceSize.width: Math.round(parent.width * Screen.devicePixelRatio)
                sourceSize.height: Math.round(parent.height * Screen.devicePixelRatio)
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                smooth: true
            }
        }

        Rectangle { // picked: the order it is pasted in
            id: orderBadge
            visible: row.order > 0
            x: -7
            y: -6
            width: Math.max(18, orderText.implicitWidth + 8)
            height: 18
            radius: 9
            color: Theme.accent
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
    }

    Column {
        y: row.onPixel((row.height - height) / 2)
        anchors.left: iconSlot.right
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: (row.showActions ? actions.width + actions.anchors.rightMargin
                              : keycap.visible ? keycap.width + keycap.anchors.rightMargin
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
            font.pixelSize: Theme.fontTitle
        }

        Text {
            width: parent.width
            text: row.detail
            textFormat: Text.PlainText
            elide: Text.ElideRight
            maximumLineCount: 1
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }
    }

    Rectangle { // in a group: which
        id: badge
        visible: row.groupName.length > 0 && !row.showActions && row.hint === 0
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: badgeRow.implicitWidth + 16
        height: 24
        radius: 12
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
                text: row.groupName
                textFormat: Text.PlainText
                elide: Text.ElideRight
                width: Math.min(implicitWidth, 96)
                color: Theme.accent
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    KeyCap { // the modifier held (Alt): it and this number paste the row
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

        Row {
            id: actions
            visible: row.showActions
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            RowAction {
                id: previewAction
                glyph: "" // View
                checked: row.previewing
                tip: row.previewing ? qsTr("关闭预览") : qsTr("预览")
                tipShortcut: "Alt+P"
                onClicked: row.previewRequested(row.index)
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
        readonly property RowAction hovered: previewAction.tipWanted ? previewAction
                                           : pinAction.tipWanted ? pinAction
                                           : removeAction.tipWanted ? removeAction : null
        target: hovered
        text: hovered ? hovered.tip : ""
        shortcut: hovered ? hovered.tipShortcut : ""
    }
}
