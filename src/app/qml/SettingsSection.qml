import QtQuick
import QuickFind

// A card holding SettingRows, with an optional title and note above it.
Column {
    id: section

    property string title
    property string note
    default property alias rows: card.data

    spacing: 8

    Text {
        visible: section.title.length > 0
        text: section.title
        color: Theme.text
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
    }

    Text {
        width: parent.width
        visible: section.note.length > 0
        text: section.note
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }

    Rectangle {
        width: parent.width
        height: card.implicitHeight
        radius: 6
        color: Theme.card
        border.width: 1
        border.color: Theme.cardBorder

        Column {
            id: card
            width: parent.width
        }
    }
}
