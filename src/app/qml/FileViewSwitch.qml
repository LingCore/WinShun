pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import WinShun

// The panel's two views, at the footer's right as in Explorer's status bar:
// the list with its columns, or 大图 (thumbnails). With 大图 a slider for the
// pictures' size goes beside it (Alt+wheel over them does the same). Icons in
// a track like the footer's other switches, so it takes no row of its own.
Row {
    id: viewSwitch

    required property Launcher launcher

    signal modeRequested(bool gallery)

    spacing: 10

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    Slider {
        id: size
        anchors.verticalCenter: parent.verticalCenter
        visible: viewSwitch.launcher.gallery
        width: 96
        height: Theme.chipHeight
        padding: 0
        from: 96
        to: 320
        stepSize: 16
        value: viewSwitch.launcher.thumbnailSize
        focusPolicy: Qt.NoFocus // typing stays in the search box
        onMoved: viewSwitch.launcher.thumbnailSize = value
        Accessible.name: qsTr("缩略图大小")

        // As Windows 11's: a thin track filled with the accent up to a round
        // thumb whose accent middle grows under the mouse.
        background: Rectangle {
            x: size.leftPadding + 7
            y: viewSwitch.onPixel(size.topPadding + (size.availableHeight - height) / 2)
            width: size.availableWidth - 14
            height: 4
            radius: 2
            color: Theme.chipBorder

            Rectangle {
                width: viewSwitch.onPixel(size.visualPosition * parent.width)
                height: parent.height
                radius: 2
                color: Theme.accent
            }
        }
        handle: Rectangle {
            x: viewSwitch.onPixel(size.leftPadding + size.visualPosition * (size.availableWidth - width))
            y: viewSwitch.onPixel(size.topPadding + (size.availableHeight - height) / 2)
            width: 14
            height: 14
            radius: 7
            color: Theme.chipActive
            border.width: 1
            border.color: Theme.chipBorder

            Rectangle {
                anchors.centerIn: parent
                readonly property int across: size.pressed ? 6 : size.hovered ? 9 : 7
                width: across
                height: across
                radius: across / 2
                color: Theme.accent
            }
        }
    }

    component ViewChip: Rectangle {
        id: chip

        required property bool gallery
        required property string glyph
        required property string name
        readonly property bool active: gallery === viewSwitch.launcher.gallery
        readonly property bool hovered: area.containsMouse

        width: height + 6
        height: Theme.chipHeight - 4
        radius: Theme.controlRadius
        color: active ? Theme.chipActive : hovered ? Theme.chipHover : "transparent"
        border.width: active ? 1 : 0
        border.color: Theme.chipBorder
        Accessible.role: Accessible.RadioButton
        Accessible.name: name
        Accessible.checked: active

        Glyph {
            x: viewSwitch.onPixel((chip.width - width) / 2)
            y: viewSwitch.onPixel((chip.height - height) / 2)
            glyph: chip.glyph
            size: 16
            color: chip.active || chip.hovered ? Theme.text : Theme.subtext
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: if (!chip.active) viewSwitch.modeRequested(chip.gallery)
        }
    }

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: chips.implicitWidth + 4
        height: Theme.chipHeight
        radius: Theme.cardRadius
        color: Theme.track

        Row {
            id: chips
            x: 2
            y: 2
            spacing: 2

            ViewChip {
                id: listChip
                objectName: "fileViewList"
                gallery: false
                glyph: "\uE8FD" // BulletedList
                name: qsTr("列表")
            }
            ViewChip {
                id: galleryChip
                objectName: "fileViewGallery"
                gallery: true
                glyph: "\uE8A9" // ViewAll
                name: qsTr("大图")
            }
        }
    }

    HoverTip {
        target: listChip.hovered ? listChip : galleryChip.hovered ? galleryChip : size.hovered && !size.pressed ? size : null
        text: listChip.hovered ? listChip.name : galleryChip.hovered ? galleryChip.name : qsTr("缩略图大小")
        shortcut: size.hovered && !listChip.hovered && !galleryChip.hovered ? qsTr("Alt+滚轮") : ""
    }
}
