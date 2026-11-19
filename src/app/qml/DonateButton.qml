pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// 捐赠, in the settings' title bar (TitleBar.tools), after lcstp.com's 向我捐赠
// button: its pixel heart and its pink, as a key of the app's (rounded, a
// darker lower edge). The pink is the site's darkened until white words
// read on it (4.6:1, docs/colors.md); hovered it darkens, as the site's does
// not need to. The heart beats while the pointer rests on it, as there.
Item {
    id: button

    property real dpr: 1

    signal clicked()

    readonly property color face: area.pressed ? "#B8224F" : area.containsMouse ? "#CC285A" : "#DB2E62"

    implicitWidth: heart.width + label.implicitWidth + 24
    implicitHeight: 28

    Accessible.role: Accessible.Button
    Accessible.name: label.text
    Accessible.description: tip.text
    Accessible.onPressAction: button.clicked()

    Rectangle { // the lower edge, under the face
        anchors.fill: parent
        radius: Theme.controlRadius
        color: "#A3284D"
    }
    Rectangle { // the face, the edge showing one pixel below it
        width: parent.width
        height: parent.height - 1 / button.dpr
        radius: Theme.controlRadius
        color: button.face

        Rectangle { // the light along its top
            x: Theme.controlRadius
            y: 1 / button.dpr
            width: parent.width - 2 * x
            height: 1 / button.dpr
            color: Qt.rgba(1, 1, 1, 0.22)
        }
    }

    Item { // lcstp.com's pixel heart: 9 × 8 cells of whole device pixels, so every cell is as wide
        id: heart

        readonly property int cellPixels: Math.max(1, Math.round(2 * button.dpr))
        property bool big: false
        readonly property real cell: (cellPixels + (big ? 1 : 0)) / button.dpr

        x: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 9 * cellPixels / button.dpr
        height: 8 * cellPixels / button.dpr

        Item { // a beat draws it a pixel a cell larger, about the same centre
            anchors.centerIn: parent
            width: 9 * heart.cell
            height: 8 * heart.cell

            Repeater {
                // The site's SVG, as drawn there: the edge, the red over it, the glint.
                model: [
                    [2, 0, 2, 1, 0], [5, 0, 2, 1, 0], [1, 1, 7, 1, 0], [0, 2, 9, 2, 0],
                    [1, 4, 7, 1, 0], [2, 5, 5, 1, 0], [3, 6, 3, 1, 0], [4, 7, 1, 1, 0],
                    [2, 1, 2, 1, 1], [5, 1, 2, 1, 1], [1, 2, 7, 2, 1], [2, 4, 5, 1, 1],
                    [3, 5, 3, 1, 1], [4, 6, 1, 1, 1],
                    [2, 2, 1, 1, 2]
                ]
                delegate: Rectangle {
                    required property var modelData
                    x: modelData[0] * heart.cell
                    y: modelData[1] * heart.cell
                    width: modelData[2] * heart.cell
                    height: modelData[3] * heart.cell
                    color: modelData[4] === 0 ? "#2A0D1A" : modelData[4] === 1 ? "#E3173F" : "#FFFFFF"
                }
            }
        }
    }

    SequentialAnimation { // the site's heart-beat, two beats and a rest
        running: area.containsMouse && SystemTheme.animations
        loops: Animation.Infinite
        onRunningChanged: if (!running) heart.big = false

        PropertyAction { target: heart; property: "big"; value: true }
        PauseAnimation { duration: 60 }
        PropertyAction { target: heart; property: "big"; value: false }
        PauseAnimation { duration: 50 }
        PropertyAction { target: heart; property: "big"; value: true }
        PauseAnimation { duration: 50 }
        PropertyAction { target: heart; property: "big"; value: false }
        PauseAnimation { duration: 640 }
    }

    Text {
        id: label
        anchors.left: heart.right
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -0.5 / button.dpr // the face's centre, above the edge
        text: qsTr("捐赠")
        color: Theme.accentText
        font.pixelSize: Theme.fontCaption
        font.weight: Font.Medium
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }

    HoverTip {
        id: tip
        target: area.containsMouse ? button : null
        text: qsTr("在爱发电支持 Win顺")
        dpr: button.dpr
    }
}
