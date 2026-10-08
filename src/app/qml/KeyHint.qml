import QtQuick
import WinShun

// "[Ctrl ↵] 打开位置" in the footer.
Row {
    id: hint

    property string keys
    property string label

    spacing: 5

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: keyText.implicitWidth + 10
        height: 22
        radius: 4
        color: Theme.keycap
        border.width: 1
        border.color: Theme.keycapBorder

        Text {
            id: keyText
            anchors.centerIn: parent
            text: hint.keys
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: hint.label
        color: Theme.faint
        font.pixelSize: Theme.fontBody
    }
}
