import QtQuick
import WinShun

// One option: title and explanation on the left, its control on the right,
// optional extra content (lists, inputs) underneath.
Item {
    id: row

    property string title
    property string description
    default property alias trailing: trailingRow.data
    property alias body: bodyColumn.data
    property bool bodyShown: true // false: no room is kept for a body with nothing to show now

    width: parent ? parent.width : 0
    implicitHeight: content.implicitHeight + 28

    Rectangle { // separates rows inside a card
        visible: row.y > 0
        x: 1
        width: parent.width - 2
        height: 1
        color: Theme.cardBorder
    }

    Column {
        id: content
        x: 18
        y: 14
        width: parent.width - 36
        spacing: 12

        Item {
            width: parent.width
            height: Math.max(texts.implicitHeight, trailingRow.implicitHeight)

            Column {
                id: texts
                anchors.left: parent.left
                anchors.right: trailingRow.left
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                Text {
                    width: parent.width
                    text: row.title
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: row.description
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                    lineHeight: 1.15
                }
            }

            Row {
                id: trailingRow
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
            }
        }

        Column {
            id: bodyColumn
            width: parent.width
            spacing: 10
            visible: row.bodyShown && children.length > 0
        }
    }
}
