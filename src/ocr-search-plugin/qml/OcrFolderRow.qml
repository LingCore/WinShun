pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

RowLayout {
    id: row
    required property string path
    signal removed()
    spacing: 4
    Glyph { glyph: "\uE8B7"; size: 16; color: Theme.subtext }
    Text {
        id: pathLabel
        Layout.fillWidth: true
        text: row.path
        elide: Text.ElideMiddle
        color: Theme.text
        font.pixelSize: Theme.fontCaption
        HoverHandler { id: pathHover }
        HoverTip { target: pathHover.hovered ? pathLabel : null; text: row.path }
    }
    FlatButton {
        id: removeButton
        glyph: "\uE711"
        flat: true
        role: "toolbar"
        implicitWidth: 32
        Accessible.name: qsTr("移除目录")
        HoverHandler { id: removeHover }
        HoverTip { target: removeHover.hovered ? removeButton : null; text: qsTr("移除目录，保留原文件") }
        onClicked: row.removed()
    }
}
