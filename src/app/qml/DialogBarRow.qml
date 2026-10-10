import QtQuick
import WinShun

// A row of the dialog bar's list (DialogBarWindow.qml): icon, highlighted name, the
// folder it is in, and on the right where it comes from ("资源管理器", "固定",
// "最近"…), or a key to press. Where there is room, when it was last written
// at the end of its name's line. While the number keys' modifier is held,
// its number on the right (Ctrl+1 goes to the first row shown).
Item {
    id: row

    required property int index
    required property string name
    required property string nameHtml
    required property string folder
    required property string path
    required property string icon
    required property bool isDir
    required property string modified // "昨天 14:32"; empty when not known
    property string tag
    property bool showModified: true
    property int hint: 0 // the number keys' modifier held: this row's number, 0 for none

    readonly property bool current: ListView.isCurrentItem

    signal clicked(int index)
    signal menuRequested(int index, point globalPos)

    Accessible.role: Accessible.ListItem
    Accessible.name: tag.length > 0 ? name + ", " + folder + ", " + tag : name + ", " + folder
    Accessible.selected: current
    Accessible.onPressAction: clicked(index)

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
        anchors.right: keycap.visible ? keycap.left : tagText.left
        anchors.rightMargin: 10
        spacing: 2

        Item {
            width: parent.width
            height: row.upToPixel(nameText.implicitHeight) // the folder below on a whole pixel too

            Text {
                id: nameText
                width: Math.min(implicitWidth, parent.width - (date.visible ? date.width + 10 : 0))
                text: row.nameHtml
                textFormat: Text.StyledText
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: Theme.fontBody
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
                font.pixelSize: Theme.fontCaption
            }
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

    KeyCap {
        id: keycap
        visible: row.hint > 0
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        number: row.hint
    }

    Text {
        id: tagText
        visible: !keycap.visible
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
