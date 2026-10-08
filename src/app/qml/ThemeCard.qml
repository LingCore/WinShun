pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// One choice of the theme picker: a small picture of the launcher in that
// theme and its name under it. "system" shows light and dark side by side.
Item {
    id: card

    property string mode: "light" // light | dark | system
    property string label
    property bool selected: false

    signal activated()

    implicitWidth: 176
    implicitHeight: frame.height + caption.height
    activeFocusOnTab: true

    Keys.onSpacePressed: card.activated()
    Keys.onReturnPressed: card.activated()
    Keys.onEnterPressed: card.activated()

    // The launcher in miniature: search box, three results (the first one
    // selected), a status line. Fixed colours: it shows the theme, not the current one.
    component Preview: Rectangle {
        id: preview

        property bool dark: false
        readonly property color accent: dark ? Qt.lighter(SystemTheme.accent, 1.55) : SystemTheme.accent
        readonly property color ink: dark ? "#5A5A5A" : "#CFCFCF"
        readonly property color faintInk: dark ? "#3A3A3A" : "#E3E3E3"

        color: dark ? "#202020" : "#F9F9F9"

        Rectangle { // search box
            x: parent.width * 0.08
            y: parent.height * 0.1
            width: parent.width * 0.84
            height: parent.height * 0.17
            radius: height / 2
            color: preview.dark ? "#2B2B2B" : "#FFFFFF"
            border.width: 1
            border.color: preview.dark ? "#3A3A3A" : "#E2E2E2"

            Rectangle { // caret
                x: parent.height * 0.6
                anchors.verticalCenter: parent.verticalCenter
                width: 1.5
                height: parent.height * 0.5
                color: preview.accent
            }
            Rectangle { // typed text
                x: parent.height * 0.6 + 5
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width * 0.3
                height: 3
                radius: 1.5
                color: preview.ink
            }
        }

        Column {
            x: parent.width * 0.08
            y: parent.height * 0.34
            width: parent.width * 0.84
            spacing: parent.height * 0.035

            Repeater {
                model: 3
                delegate: Rectangle {
                    id: result
                    required property int index
                    width: parent.width
                    height: preview.height * 0.15
                    radius: 3
                    color: index === 0 ? (preview.dark ? "#2F2F2F" : "#EAEAEA") : "transparent"

                    Rectangle { // selection mark
                        visible: result.index === 0
                        x: 1
                        anchors.verticalCenter: parent.verticalCenter
                        width: 2
                        height: parent.height * 0.5
                        radius: 1
                        color: preview.accent
                    }
                    Rectangle { // icon
                        x: parent.height * 0.35
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.height * 0.55
                        height: width
                        radius: 2
                        color: result.index === 0 ? preview.accent : preview.ink
                        opacity: result.index === 0 ? 0.85 : 1
                    }
                    Rectangle { // name
                        x: parent.height * 1.2
                        y: parent.height * 0.28
                        width: parent.width * (0.5 - result.index * 0.08)
                        height: 3
                        radius: 1.5
                        color: preview.ink
                    }
                    Rectangle { // folder
                        x: parent.height * 1.2
                        y: parent.height * 0.6
                        width: parent.width * (0.32 + result.index * 0.05)
                        height: 2
                        radius: 1
                        color: preview.faintInk
                    }
                }
            }
        }

        Rectangle { // status line
            x: parent.width * 0.08
            y: parent.height * 0.88
            width: parent.width * 0.26
            height: 2
            radius: 1
            color: preview.faintInk
        }
    }

    Rectangle {
        id: frame
        width: card.width
        height: Math.round(card.width * 0.64)
        radius: 8
        color: "transparent"
        border.width: card.selected ? 2 : 1
        border.color: card.selected ? Theme.accent : area.containsMouse ? Theme.subtext : Theme.cardBorder

        Behavior on border.color { ColorAnimation { duration: 120 } }

        Item {
            id: pictures
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            scale: area.pressed ? 0.98 : 1

            Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }

            Preview {
                width: pictures.width
                height: pictures.height
                radius: 5
                dark: card.mode === "dark"
            }
            Item { // "system": the right half dark
                visible: card.mode === "system"
                x: pictures.width / 2
                width: pictures.width / 2
                height: pictures.height
                clip: true

                Preview {
                    x: -pictures.width / 2
                    width: pictures.width
                    height: pictures.height
                    radius: 5
                    dark: true
                }
            }
        }

        Rectangle { // tick in the corner of the chosen one
            visible: card.selected
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 9
            width: 20
            height: 20
            radius: 10
            color: Theme.accent

            Glyph {
                anchors.centerIn: parent
                glyph: "" // CheckMark
                size: 11
                color: Theme.accentText
            }
        }

        Rectangle { // keyboard focus
            visible: card.activeFocus
            anchors.fill: parent
            anchors.margins: -4
            radius: 11
            color: "transparent"
            border.width: 2
            border.color: Theme.text
        }
    }

    Row {
        id: caption
        y: frame.height
        height: 36
        spacing: 8

        Rectangle { // radio button
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            radius: 9
            color: card.selected ? Theme.accent : "transparent"
            border.width: card.selected ? 0 : 1
            border.color: Theme.subtext

            Rectangle {
                visible: card.selected
                anchors.centerIn: parent
                width: area.containsMouse ? 9 : 8
                height: width
                radius: width / 2
                color: Theme.accentText
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: card.width - 26
            text: card.label
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: card.activated()
    }
}
