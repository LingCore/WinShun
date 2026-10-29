pragma ComponentBehavior: Bound

import QtQuick
import WinShun

Item {
    id: tile

    required property int index
    required property string name
    required property var nameMatches
    required property string folder
    required property string path
    required property string icon
    required property string galleryThumbnail
    required property string size
    required property string modifiedDate
    required property bool isApp
    required property bool offline
    required property bool missing
    required property bool selected
    property NameTip nameTip: null
    readonly property bool current: GridView.isCurrentItem
    readonly property var results: GridView.view ? GridView.view.model : null
    readonly property int selectedCount: results ? results.selectedCount : 0
    readonly property bool wantsTip: area.containsMouse && !area.pressed
    signal clicked(int index, int modifiers)
    signal doubleClicked(int index, int modifiers)
    signal menuRequested(int index, point globalPos, int modifiers)
    signal dragRequested(int index)

    Accessible.role: Accessible.ListItem
    Accessible.name: name
    Accessible.description: folder
    Accessible.selected: selected || current

    function dropTip() {
        if (nameTip && nameTip.source === tile)
            nameTip.dismiss()
    }
    function offerTip() {
        if (!nameTip)
            return
        if (!wantsTip) {
            nameTip.withdraw(tile)
            return
        }
        nameTip.offer(tile, title, {
            name: name, folder: folder, path: path, size: size, modified: modifiedDate,
            onDisk: !isApp && !offline && !missing, at: area.mapToGlobal(area.mouseX, area.mouseY)
        })
    }
    onWantsTipChanged: offerTip()
    onPathChanged: dropTip()
    Component.onDestruction: dropTip()

    Rectangle {
        anchors.fill: parent
        anchors.margins: 4
        radius: Theme.controlRadius
        color: tile.selected || (tile.current && tile.selectedCount === 0)
            ? Theme.selection : area.containsMouse ? Theme.hover : "transparent"
        border.width: tile.current ? Theme.edge(Screen.devicePixelRatio) : 0
        border.color: Theme.accent
    }

    Item {
        id: picture
        x: 12
        y: 12
        width: parent.width - 24
        height: Math.max(32, parent.height - 72)

        Image {
            anchors.centerIn: parent
            width: 48
            height: 48
            source: tile.icon
            sourceSize: Qt.size(48, 48)
            asynchronous: true
            visible: preview.status !== Image.Ready || preview.implicitWidth <= 0
        }
        Image {
            id: preview
            objectName: "fileGalleryThumbnail"
            anchors.fill: parent
            source: tile.galleryThumbnail
            sourceSize: Qt.size(picture.width, picture.height)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            visible: status === Image.Ready && implicitWidth > 0
        }
    }
    Text {
        id: title
        x: 12
        y: picture.y + picture.height + 6
        width: parent.width - 24
        text: tile.results && tile.nameMatches.length > 0
            ? tile.results.elidedHtml(tile.name, tile.nameMatches, width, font, tile.results.highlightColor) : tile.name
        textFormat: tile.nameMatches.length > 0 ? Text.StyledText : Text.PlainText
        elide: Text.ElideMiddle
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.uiBody
        color: tile.missing || tile.offline ? Theme.faint : Theme.text
    }
    Text {
        x: title.x
        y: title.y + title.height + 3
        width: title.width
        text: tile.offline ? qsTr("离线") : tile.missing ? qsTr("已不存在") : tile.folder
        textFormat: Text.PlainText
        elide: Text.ElideMiddle
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.uiCaption
        color: Theme.subtext
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        property point pressedAt
        property bool draggedOut: false
        onPressed: mouse => {
            tile.dropTip()
            draggedOut = false
            pressedAt = Qt.point(mouse.x, mouse.y)
            preventStealing = mouse.button === Qt.LeftButton && !tile.isApp && !tile.offline && !tile.missing
                && mouse.source === Qt.MouseEventNotSynthesized
        }
        onPositionChanged: mouse => {
            if (!pressed) {
                tile.offerTip()
            } else if (preventStealing && !draggedOut && (mouse.buttons & Qt.LeftButton)
                && Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y) >= Application.styleHints.startDragDistance) {
                draggedOut = true
                tile.dragRequested(tile.index)
            }
        }
        onWheel: wheel => { tile.dropTip(); wheel.accepted = false }
        onClicked: mouse => {
            if (draggedOut)
                return
            if (mouse.button === Qt.RightButton)
                tile.menuRequested(tile.index, mapToGlobal(mouse.x, mouse.y), mouse.modifiers)
            else
                tile.clicked(tile.index, mouse.modifiers)
        }
        onDoubleClicked: mouse => {
            if (mouse.button === Qt.LeftButton && !draggedOut)
                tile.doubleClicked(tile.index, mouse.modifiers)
        }
    }
}
