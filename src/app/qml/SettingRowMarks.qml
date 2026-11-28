import QtQuick
import WinShun

// What marks a SettingRow out, under its content (which the row keeps above,
// z 1: this is made after it): the style's highlight while
// it is the option Enter goes to in a search (Theme.rowHighlight; not under
// the pointer: a row is no button, only the controls on it are), and the
// flash that draws the eye to a row gone to. Made by the row the first time
// either is needed (SettingRow.needMarks): of the settings window's three
// hundred rows a handful ever are, and each carried these nine objects.
Item {
    id: marks

    required property SettingRow row

    // Draws the eye to the row once it has been gone to.
    function flash() {
        if (SystemTheme.animations) {
            flashAnimation.restart()
        } else {
            flashMark.opacity = 0.14
            flashTimer.restart()
        }
    }

    anchors.fill: parent
    anchors.margins: row.edge

    Hook {
        anchors.fill: parent
        part: marks.row.current ? Theme.rowHighlight : null
        selected: marks.row.current
        role: "panel"
        dpr: marks.row.dpr
    }

    Rectangle {
        id: flashMark
        anchors.fill: parent
        radius: Theme.insetCardRadius
        color: Theme.accent
        opacity: 0
        visible: opacity > 0

        SequentialAnimation {
            id: flashAnimation
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.16; duration: 160; easing.type: Easing.OutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.04; duration: 360; easing.type: Easing.InOutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.16; duration: 200; easing.type: Easing.InOutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0; duration: 700; easing.type: Easing.InQuad }
        }
        Timer {
            id: flashTimer
            interval: 1600
            onTriggered: flashMark.opacity = 0
        }
    }
}
