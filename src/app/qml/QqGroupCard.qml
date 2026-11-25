pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

// The 拾穗计划 page's QQ group: the QR code on a light plate (a phone scans
// it), the group's number to copy, and its link to open in QQ. Opened over
// its button, which toggles it, so a press there does not close it first.
Popup {
    id: card

    required property SettingsEditor editor
    readonly property var group: Gleaning.qqGroup
    readonly property color ink: Qt.rgba(0.36, 0.19, 0.06, 1) // always on the light plate
    readonly property color inkSoft: Qt.rgba(0.55, 0.34, 0.17, 1)
    property bool copied: false

    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    onClosed: copied = false

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 }
    }

    background: Rectangle {
        radius: 14
        color: Qt.rgba(1, 0.98, 0.95, 1)
        border.width: 1
        border.color: Qt.rgba(0.80, 0.55, 0.32, 0.45)
    }

    contentItem: Column {
        spacing: 10

        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 152
            height: 152
            source: card.group?.qr ?? ""
            sourceSize.width: width // SVG: logical pixels, Qt adds the device pixel ratio
            sourceSize.height: height
            smooth: true
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("QQ 群 · %1").arg(card.group?.name ?? "")
            color: card.ink
            font.pixelSize: Theme.fontBody
            font.weight: Font.Bold
        }
        Item { // the number, and a click copies it
            anchors.horizontalCenter: parent.horizontalCenter
            implicitWidth: numberRow.implicitWidth
            implicitHeight: numberRow.implicitHeight

            Row {
                id: numberRow
                spacing: 6

                Text {
                    id: numberText
                    text: qsTr("群号 %1").arg(card.group?.number ?? "")
                    color: card.ink
                    font.pixelSize: Theme.fontCaption
                    font.underline: copyArea.containsMouse
                }
                Glyph {
                    anchors.verticalCenter: numberText.verticalCenter
                    iconName: card.copied ? "check" : "copy"
                    size: 14
                    color: copyArea.containsMouse || card.copied ? card.ink : card.inkSoft
                }
            }
            MouseArea {
                id: copyArea
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    card.editor.copyText(card.group.number);
                    card.copied = true;
                }
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("用手机 QQ 扫码，进群一起交流\n建议和问题请到官网反馈")
            color: card.inkSoft
            font.pixelSize: Theme.fontCaption
            lineHeight: 1.15
        }
        Text { // the link, for QQ on this computer
            id: openLink
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("在电脑 QQ 中打开")
            color: card.ink
            font.pixelSize: Theme.fontCaption
            font.weight: Theme.fontStrong
            font.underline: openArea.containsMouse

            MouseArea {
                id: openArea
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: card.editor.openUrl(card.group.url)
            }
        }
    }
}
