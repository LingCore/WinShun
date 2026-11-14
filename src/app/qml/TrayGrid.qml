pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// 程序 › 运行: the notification-area icons under the windows, as tiles
// (Launcher::trayApps). A click switches to the icon's window (or clicks the
// icon), a right click opens the icon's own menu. The words typed narrow
// them as they narrow the windows.
GridView {
    id: grid

    property real dpr: 1
    // Where the grid's top is in the window, for tiles on whole device pixels.
    property real viewY: 0

    signal openRequested(int index)
    signal menuRequested(int index, point globalPos)

    readonly property int tileHeight: 84
    readonly property int iconSize: 32
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

    function stopScrolling() { wheelRest = 0 }
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
        required property var modelData
        readonly property bool current: GridView.isCurrentItem

        width: grid.tileWidth
        height: grid.tileHeight
        transform: Translate {
            y: {
                const deviceY = (grid.viewY + tile.y - grid.contentY) * grid.dpr
                return (Math.round(deviceY) - deviceY) / grid.dpr
            }
        }
        Accessible.role: Accessible.Button
        Accessible.name: tile.modelData.name
        Accessible.onPressAction: grid.openRequested(tile.index)

        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: Theme.largeControlRadius
            color: tile.current ? Theme.selection : tileArea.containsMouse ? Theme.hover : "transparent"
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
        Image {
            readonly property int pixels: Math.round(grid.iconSize * grid.dpr)

            x: (tile.width - grid.iconSize) / 2 + 1 / 64
            y: 10 + 1 / 64
            width: pixels / grid.dpr
            height: pixels / grid.dpr
            visible: tile.modelData.icon !== ""
            source: tile.modelData.icon
            sourceSize.width: pixels / grid.dpr
            sourceSize.height: pixels / grid.dpr
            smooth: false
            asynchronous: true
            fillMode: Image.PreserveAspectFit
        }
        Glyph { // a tray icon with no window of its own: a plain tile
            visible: tile.modelData.icon === ""
            anchors.horizontalCenter: parent.horizontalCenter
            y: 10
            iconName: "squares-four"
            size: grid.iconSize
            color: Theme.subtext
        }
        Text {
            x: 6
            y: 48
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            text: tile.modelData.name
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
    }
}
