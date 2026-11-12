pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The 程序 scope: a grid of app icons over the launcher's result rows — the
// apps used most, or all installed ones (the strip above chooses). A word
// typed narrows it by name (Launcher does that, so the rows are the menu's
// and Enter's too). Clicking a tile or Enter opens it; a right click asks for
// the same menu a file's row has.
GridView {
    id: grid

    required property Launcher launcher
    property real dpr: 1 // the window's, passed by the launcher (GitHub #111)

    signal openRequested(int index)
    signal menuRequested(int index, point globalPos)

    readonly property int gutter: 8
    readonly property int tileHeight: 116
    readonly property int iconSize: 52
    readonly property int sideMargin: 16
    readonly property int columns: Math.max(3, Math.floor((width - 2 * sideMargin) / 104))
    // Exactly the room divided by the columns (a hair under, so the floor does not drop one):
    // the last tile's right edge meets the margin, no empty strip.
    readonly property real cellW: Math.floor((width - 2 * sideMargin) / columns * 100) / 100
    readonly property real tileWidth: Math.max(72, cellW - gutter)

    clip: true
    model: launcher.results
    cellWidth: cellW
    cellHeight: tileHeight + gutter
    leftMargin: sideMargin
    topMargin: 8
    rightMargin: sideMargin
    bottomMargin: 8
    boundsBehavior: Flickable.StopAtBounds
    highlightFollowsCurrentItem: false
    keyNavigationEnabled: false
    onCurrentIndexChanged: positionViewAtIndex(currentIndex, GridView.Contain)

    // The wheel scrolls the grid (it is not a row list; nothing to expand).
    WheelHandler {
        target: null
        acceptedModifiers: Qt.NoModifier
        onWheel: event => {
            const maxY = Math.max(0, grid.contentHeight - grid.height)
            grid.contentY = Math.max(0, Math.min(maxY, grid.contentY - event.angleDelta.y))
            event.accepted = true
        }
    }

    delegate: Item {
        id: tile

        required property int index
        required property string name
        required property string icon
        required property string path
        readonly property bool current: GridView.isCurrentItem
        readonly property bool starred: grid.launcher.starredApps.indexOf(path) >= 0

        width: grid.cellW
        height: grid.tileHeight
        Accessible.role: Accessible.Button
        Accessible.name: tile.name

        Rectangle { // a card, as the picture's
            anchors.fill: parent
            anchors.margins: 2
            radius: Theme.controlRadius
            color: tile.current ? Theme.selection : tileArea.containsMouse ? Theme.hover : Theme.layer
            border.width: Theme.edge(grid.dpr)
            border.color: Theme.divider
        }
        Image {
            readonly property real dpr: grid.dpr
            readonly property int pixels: Math.round(grid.iconSize * dpr)

            anchors.horizontalCenter: parent.horizontalCenter
            y: 16
            width: pixels / dpr
            height: pixels / dpr
            source: tile.icon
            sourceSize.width: pixels / dpr
            sourceSize.height: pixels / dpr
            smooth: false
            asynchronous: true
            fillMode: Image.PreserveAspectFit
        }
        Text {
            x: 6
            y: 74
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            text: tile.name
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.uiCaption
            elide: Text.ElideRight
            wrapMode: Text.Wrap
            maximumLineCount: 2
            lineHeight: 0.95
        }
        MouseArea {
            id: tileArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: mouse => {
                if (mouse.button === Qt.RightButton)
                    grid.menuRequested(tile.index, mapToGlobal(mouse.x, mouse.y))
                else
                    grid.openRequested(tile.index)
            }
        }
        Item { // the star: 常用软件, on the picture's corner (while hovered, or kept starred)
            id: starBadge
            width: 26
            height: 26
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 7
            visible: tileArea.containsMouse || tile.starred

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.layer
                border.width: Theme.edge(grid.dpr)
                border.color: Theme.divider
            }
            Text {
                anchors.centerIn: parent
                text: tile.starred ? "★" : "☆"
                color: tile.starred ? Theme.accent : Theme.subtext
                font.pixelSize: 15
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: grid.launcher.toggleAppStar(tile.path)
            }
        }
    }
}
