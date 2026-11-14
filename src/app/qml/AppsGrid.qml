pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// 程序 (GitHub #296): the launcher's rows as a grid of app icons, each with
// its name under it. Which apps, the strip under the box chooses (TypeStrip);
// words typed narrow them (Launcher::showApps). The rows are the result
// model's, so Enter, the menu and the actions are a row's. A click opens the
// app, a right click asks for the row's menu; the star at a tile's corner
// (there while the mouse is on the tile, and kept on a favorite) puts the app
// into the favorites or takes it out.
GridView {
    id: grid

    required property Launcher launcher
    property real dpr: 1 // the window's, passed by the launcher (GitHub #111)
    // Where the grid's top is in the window, for tiles on whole device pixels.
    property real viewY: 0

    signal openRequested(int index)
    signal menuRequested(int index, point globalPos)

    // Tiles, icons and words on multiples of 4 logical pixels: whole device
    // pixels at 125-200 % (as the row of apps over the taskbar).
    readonly property int tileHeight: 96
    readonly property int iconSize: 40
    readonly property int columns: Math.max(1, Math.floor((width - 32) / 104))
    readonly property int tileWidth: Math.max(8, Math.floor((width - 32) / columns / 8) * 8)
    readonly property int sideMargin: Math.max(0, Math.floor((width - columns * tileWidth) / 8) * 4)

    clip: true
    cellWidth: tileWidth
    cellHeight: tileHeight
    leftMargin: sideMargin
    rightMargin: width - sideMargin - columns * tileWidth
    topMargin: 4
    bottomMargin: 4
    boundsBehavior: Flickable.StopAtBounds
    interactive: false // the wheel moves it, by whole rows of tiles
    highlightFollowsCurrentItem: false
    keyNavigationEnabled: false
    // The current tile in view, by whole rows of tiles: none cut off at the top.
    function reveal(index) {
        if (index < 0 || index >= count)
            return
        const row = Math.floor(index / columns)
        const first = Math.round((contentY + topMargin) / tileHeight)
        const whole = Math.max(1, Math.floor((height - topMargin) / tileHeight))
        if (row < first)
            contentY = row * tileHeight - topMargin
        else if (row >= first + whole)
            contentY = (row - whole + 1) * tileHeight - topMargin
    }
    onCurrentIndexChanged: reveal(currentIndex)
    onCountChanged: {
        if (count > 0 && currentIndex < 0)
            currentIndex = 0
    }

    function stopScrolling() { // as the rows' list has it (Main.qml): nothing here keeps moving
        wheelRest = 0
    }
    property real wheelRest: 0
    WheelHandler {
        target: null
        acceptedModifiers: Qt.NoModifier
        onWheel: (event) => {
            grid.wheelRest += event.angleDelta.y
            const steps = grid.wheelRest > 0 ? Math.floor(grid.wheelRest / 120) : Math.ceil(grid.wheelRest / 120)
            if (steps === 0)
                return
            grid.wheelRest -= steps * 120
            const top = -grid.topMargin
            const end = Math.max(top, grid.contentHeight + grid.bottomMargin - grid.height)
            grid.contentY = Math.max(top, Math.min(end, grid.contentY - steps * grid.tileHeight))
        }
    }

    delegate: Item {
        id: tile

        required property int index
        required property string name
        required property string icon
        required property bool favorite
        required property bool windowResult
        required property bool trayApp
        readonly property bool current: GridView.isCurrentItem

        width: grid.tileWidth
        height: grid.tileHeight
        // Vertically on whole device pixels too (see the rows').
        transform: Translate {
            y: {
                const deviceY = (grid.viewY + tile.y - grid.contentY) * grid.dpr
                return (Math.round(deviceY) - deviceY) / grid.dpr
            }
        }
        Accessible.role: Accessible.Button
        Accessible.name: tile.name
        Accessible.onPressAction: grid.openRequested(tile.index)

        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: Theme.largeControlRadius
            color: tile.current ? Theme.selection : tileArea.containsMouse || starArea.containsMouse ? Theme.hover : "transparent"
        }
        Rectangle { // accent pill on the current one, as on the current row
            visible: tile.current
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height - 6
            width: 18
            height: 3
            radius: Theme.pillRadius(height)
            color: Theme.accentLine
        }
        Image { // as a row's icon (ResultRow.qml): its own pixels, unsmoothed
            readonly property int pixels: Math.round(grid.iconSize * grid.dpr)

            x: (tile.width - grid.iconSize) / 2 + 1 / 64
            y: 12 + 1 / 64
            width: pixels / grid.dpr
            height: pixels / grid.dpr
            visible: tile.icon !== ""
            source: tile.icon
            sourceSize.width: pixels / grid.dpr
            sourceSize.height: pixels / grid.dpr
            smooth: false
            asynchronous: true
            fillMode: Image.PreserveAspectFit
        }
        Glyph { // 运行's tray icon without a picture of its own: a plain tile
            visible: tile.icon === ""
            anchors.horizontalCenter: parent.horizontalCenter
            y: 12
            iconName: "squares-four"
            size: grid.iconSize
            color: Theme.subtext
        }
        Rectangle { // 托盘: the icon is a notification-area one, not a window's
            visible: tile.trayApp
            x: 4
            y: 4
            width: 20
            height: 20
            radius: Theme.pillRadius(height)
            color: Theme.hover
            Glyph {
                anchors.centerIn: parent
                iconName: "tray"
                size: 12
                color: Theme.subtext
            }
        }
        Text {
            x: 6
            y: 58
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            text: tile.name
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.uiCaption
            elide: Text.ElideRight
            wrapMode: Text.Wrap
            maximumLineCount: 2
        }
        MouseArea {
            id: tileArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton)
                    grid.menuRequested(tile.index, mapToGlobal(mouse.x, mouse.y))
                else
                    grid.openRequested(tile.index)
            }
        }
        Item { // the star: a favorite, or one click from it
            x: parent.width - width - 4
            y: 4
            width: 24
            height: 24
            visible: !tile.windowResult && !tile.trayApp && (tile.favorite || tileArea.containsMouse || starArea.containsMouse)
            Accessible.role: Accessible.Button
            Accessible.name: tile.favorite ? qsTr("从收藏夹移除") : qsTr("加入收藏夹")
            Accessible.onPressAction: starArea.toggle()

            Glyph {
                anchors.centerIn: parent
                iconName: "star"
                filled: tile.favorite
                size: 14
                color: tile.favorite ? Theme.accent : starArea.containsMouse ? Theme.text : Theme.subtext
            }
            MouseArea {
                id: starArea
                function toggle() {
                    grid.launcher.trigger(tile.index, tile.favorite ? Launcher.RemoveFavorite : Launcher.AddFavorite)
                }
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: toggle()
            }
        }
    }
}
