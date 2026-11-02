import QtQuick
import WinShun

Rectangle {
    id: preview
    readonly property real line: Theme.edge(Screen.devicePixelRatio)
    implicitHeight: Theme.headerHeight + Theme.rowHeight + 2
    color: Theme.background
    radius: Theme.windowRadius
    border.color: Theme.divider
    border.width: line
    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("界面大小效果预览")

    Item {
        id: search
        x: 16
        width: parent.width - 32
        height: Theme.headerHeight

        Image {
            id: searchIcon
            objectName: "appearanceSearchLogo"
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.searchIconSize
            height: width
            source: "qrc:/branding/logo.svg"
            sourceSize.width: width
            sourceSize.height: height
            smooth: false
            Accessible.ignored: true
        }
        Text {
            anchors.left: searchIcon.right
            anchors.leftMargin: 12
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("搜索文件、文件夹…")
            font.pixelSize: Theme.uiSearch
            color: Theme.subtext
            elide: Text.ElideRight
        }
    }

    Rectangle {
        x: 1
        y: search.height
        width: parent.width - 2
        height: preview.line
        color: Theme.divider
    }
    Glyph {
        id: folderIcon
        x: 16
        y: search.height + (Theme.rowHeight - height) / 2
        glyph: "\uE8B7"
        size: 28
        color: Theme.accent
    }
    Column {
        x: folderIcon.x + folderIcon.width + 12
        y: search.height + (Theme.rowHeight - height) / 2
        width: parent.width - x - 16
        spacing: 2
        Text {
            width: parent.width
            text: qsTr("项目资料")
            color: Theme.text
            font.pixelSize: Theme.uiTitle
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            text: qsTr("文件夹 · 效果预览")
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
            elide: Text.ElideRight
        }
    }
}
