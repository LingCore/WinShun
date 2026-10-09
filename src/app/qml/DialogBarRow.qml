import QtQuick
import WinShun

// A row of the dialog bar's list (DialogBarWindow.qml): icon, highlighted name, the
// folder it is in, and on the right where it comes from ("资源管理器", "固定",
// "最近"…), or a key to press.
Item {
    id: row

    required property int index
    required property string name
    required property string nameHtml
    required property string folder
    required property string path
    required property string icon
    required property bool isDir
    property string tag

    readonly property bool current: ListView.isCurrentItem

    signal clicked(int index)
    signal menuRequested(int index, point globalPos)

    Accessible.role: Accessible.ListItem
    Accessible.name: tag.length > 0 ? name + ", " + folder + ", " + tag : name + ", " + folder
    Accessible.selected: current
    Accessible.onPressAction: clicked(index)

    // Text on whole device pixels (see ResultRow).
    function onPixel(y) { return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: 6
        color: row.current ? Theme.selection : area.containsMouse ? Theme.hover : "transparent"
    }

    Rectangle { // accent pill on the current row, as in Windows 11 lists
        visible: row.current
        x: 7
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: 16
        radius: 1.5
        color: Theme.accent
    }

    Item {
        id: iconSlot
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 32 // 48 device pixels at 150 %: a size icons are drawn for
        height: 32

        Image { // exactly as many device pixels as the icon has (see ResultRow)
            readonly property real dpr: Screen.devicePixelRatio
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
    }

    Column {
        y: row.onPixel((row.height - height) / 2)
        anchors.left: iconSlot.right
        anchors.leftMargin: 12
        anchors.right: tagText.left
        anchors.rightMargin: 10
        spacing: 2

        Text {
            width: Math.min(implicitWidth, parent.width)
            text: row.nameHtml
            textFormat: Text.StyledText
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        Text {
            width: parent.width
            text: row.folder
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            maximumLineCount: 1
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }
    }

    Text {
        id: tagText
        anchors.right: parent.right
        anchors.rightMargin: 18
        y: row.onPixel((row.height - height) / 2)
        text: row.tag
        color: Theme.faint
        font.pixelSize: Theme.fontCaption
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
                row.clicked(row.index)
        }
    }
}
