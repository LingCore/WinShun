pragma ComponentBehavior: Bound
import QtQuick
import WinShun

Item {
    id: picker
    required property var groups
    property string query
    property bool flipped: false
    property real dpr: 1
    property int rowHeight: 40
    readonly property var matches: {
        const words = query.trim().toLocaleLowerCase().split(/\s+/).filter(word => word.length > 0)
        return groups.filter(group => group.group > 0
            && words.every(word => group.title.toLocaleLowerCase().includes(word)))
    }
    signal chosen(var group)
    objectName: "clipboardGroupPicker"

    onMatchesChanged: {
        list.currentIndex = matches.length > 0 ? 0 : -1
        list.positionViewAtBeginning()
    }
    function handleKey(event) {
        const down = flipped ? -1 : 1
        switch (event.key) {
        case Qt.Key_Down: list.currentIndex = Math.min(list.count - 1, Math.max(0, list.currentIndex + down)); break
        case Qt.Key_Up: list.currentIndex = Math.min(list.count - 1, Math.max(0, list.currentIndex - down)); break
        case Qt.Key_PageDown: list.currentIndex = Math.min(list.count - 1, Math.max(0, list.currentIndex + down * 5)); break
        case Qt.Key_PageUp: list.currentIndex = Math.min(list.count - 1, Math.max(0, list.currentIndex - down * 5)); break
        case Qt.Key_Home: list.currentIndex = list.count > 0 ? 0 : -1; break
        case Qt.Key_End: list.currentIndex = list.count - 1; break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (list.currentIndex >= 0 && list.currentIndex < matches.length)
                chosen(matches[list.currentIndex].group)
            break
        default: return false
        }
        if (list.currentIndex >= 0)
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        event.accepted = true
        return true
    }
    ListView {
        id: list
        objectName: "clipboardGroupResults"
        anchors.fill: parent
        anchors.margins: 4
        clip: true
        reuseItems: true
        boundsBehavior: Flickable.StopAtBounds
        verticalLayoutDirection: picker.flipped ? ListView.BottomToTop : ListView.TopToBottom
        model: picker.matches
        currentIndex: count > 0 ? 0 : -1
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            width: list.width
            height: picker.rowHeight
            Accessible.role: Accessible.ListItem
            Accessible.name: qsTr("%1，%2 项").arg(modelData.title).arg(modelData.size)
            Accessible.selected: list.currentIndex === index
            Accessible.onPressAction: picker.chosen(modelData.group)
            Hook {
                anchors.fill: parent
                anchors.leftMargin: Theme.rowInset
                anchors.rightMargin: Theme.rowInset
                part: Theme.rowHighlight
                selected: list.currentIndex === row.index
                hovered: mouse.containsMouse
                role: "list"
                dpr: picker.dpr
            }
            Glyph {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                glyph: row.modelData.pinned ? "\uE718" : "\uE8B7"
                size: 18
                color: Theme.accent
            }
            Text {
                x: 44
                width: row.width - x - countLabel.width - 30
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.title
                textFormat: Text.PlainText
                font.pixelSize: Theme.fontBody
                color: Theme.text
                elide: Text.ElideRight
            }
            Text {
                id: countLabel
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.size
                font.pixelSize: Theme.fontCaption
                color: Theme.faint
            }
            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: picker.chosen(row.modelData.group)
            }
        }
    }
    Text {
        anchors.centerIn: parent
        width: parent.width - 32
        visible: picker.matches.length === 0
        text: picker.query.trim().length > 0 ? qsTr("没有匹配的分组") : qsTr("还没有分组")
        color: Theme.subtext
        font.pixelSize: Theme.fontBody
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }
    ListScrollBar {
        flickable: list
        anchors.right: parent.right
        height: parent.height
        dpr: picker.dpr
    }
}
