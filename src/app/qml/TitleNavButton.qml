pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Back / Forward icon button for a window title bar (TitleBar.leadingTools).
// Flat until hovered, dims quietly when there is nowhere to go, and opens a
// history dropdown on right-click or press-and-hold (as web browsers do).
Item {
    id: button

    property string iconName
    property string accessibleName
    property bool active: true
    property bool warm: false
    property color ink: Theme.subtext
    property color strongInk: Theme.text
    property real dpr: 1

    readonly property bool hovered: area.containsMouse
    readonly property bool leftPressed: active && area.pressed && (area.pressedButtons & Qt.LeftButton) !== 0
    property bool heldMenu: false

    signal clicked()
    signal menuRequested()

    implicitWidth: 30
    implicitHeight: 28

    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    Accessible.onPressAction: if (button.active) button.clicked()

    Hook { // the style's toolbar key (Theme.keySurface), flat until hovered
        id: surface
        anchors.fill: parent
        visible: !button.warm
        part: button.warm ? null : Theme.keySurface
        hovered: button.active && area.containsMouse
        pressed: button.leftPressed
        flat: true
        role: "toolbar"
        dpr: button.dpr
    }

    Rectangle { // warm-scene hover surface (拾穗计划)
        anchors.fill: parent
        visible: button.warm && button.active && (area.containsMouse || button.leftPressed)
        radius: Theme.controlRadius
        color: button.leftPressed ? Qt.darker(WarmPalette.chip, 1.06) : WarmPalette.chip
    }

    Glyph {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: button.warm ? 0 : surface.out("contentOffsetY", 0)
        inkScale: button.warm ? 1 : surface.out("contentScale", 1)
        iconName: button.iconName
        size: 14
        opacity: button.active ? 1 : 0.34
        color: button.warm
               ? (button.active && area.containsMouse ? WarmPalette.ink : button.ink)
               : surface.out("foreground", button.active && area.containsMouse ? button.strongInk : button.ink)
    }

    Timer {
        id: holdTimer
        interval: 450
        onTriggered: {
            if (button.active && area.pressed && area.containsMouse) {
                button.heldMenu = true
                button.menuRequested()
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: button.active ? Qt.PointingHandCursor : Qt.ArrowCursor
        onPressed: (mouse) => {
            if (!button.active)
                return
            if (mouse.button === Qt.LeftButton) {
                button.heldMenu = false
                holdTimer.restart()
            }
        }
        onReleased: holdTimer.stop()
        onCanceled: {
            holdTimer.stop()
            button.heldMenu = false
        }
        onClicked: (mouse) => {
            holdTimer.stop()
            if (!button.active)
                return
            if (mouse.button === Qt.RightButton) {
                button.menuRequested()
                return
            }
            if (mouse.button === Qt.LeftButton && !button.heldMenu)
                button.clicked()
            button.heldMenu = false
        }
    }
}
