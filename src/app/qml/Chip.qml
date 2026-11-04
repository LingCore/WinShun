import QtQuick
import WinShun

// A removable tag: "node_modules ×".
Item {
    id: chip

    property string text
    property bool marked: false // has the words the settings are searched for
    readonly property real dpr: Screen.devicePixelRatio // one Screen attached object for all its items (GitHub #111)
    signal removeClicked()

    implicitWidth: Theme.snapUp(label.implicitWidth + 40, chip.dpr) // whole device pixels: the next chip's edge in a Flow
    implicitHeight: 32

    Hook { // the style's card (Theme.cardPart), small: a tag
        anchors.fill: parent
        part: Theme.cardPart
        corner: Theme.controlRadius
        extra: ({ inset: true })
        dpr: chip.dpr
    }
    Rectangle { // what the settings search found
        visible: chip.marked
        anchors.fill: parent
        anchors.margins: Theme.edge(chip.dpr)
        radius: Math.max(0, Theme.controlRadius - Theme.edge(chip.dpr))
        color: Theme.selection
    }

    Text {
        id: label
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        text: chip.text
        textFormat: Text.PlainText
        color: Theme.text
        font.pixelSize: Theme.fontBody
    }

    Item {
        anchors.right: parent.right
        anchors.rightMargin: 4
        y: Theme.snap((parent.height - height) / 2, chip.dpr)
        width: 22
        height: 22

        Hook { // the style's key, flat until hovered (Theme.keySurface)
            id: removePart
            anchors.fill: parent
            part: Theme.keySurface
            hovered: area.containsMouse
            pressed: area.pressed
            flat: true
            corner: Theme.smallControlRadius
            role: "toolbar"
            dpr: chip.dpr
        }

        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: removePart.out("contentOffsetY", 0)
            glyph: "" // Cancel
            size: 12
            color: removePart.out("foreground", area.containsMouse ? Theme.text : Theme.subtext)
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.removeClicked()
        }
    }
}
