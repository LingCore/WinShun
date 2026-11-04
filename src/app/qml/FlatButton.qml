import QtQuick
import WinShun

// A Windows 11 style push button, optionally with an icon glyph.
Rectangle {
    id: button

    property string text
    property string glyph
    property string trailingGlyph // after the text (a drop-down's chevron)
    property bool highlighted: false
    property bool flat: false // no background until hovered
    // false: Tab skips it and Space and Enter do nothing; only a click works
    // (for a button that a key meant for another window must not press).
    property bool keyboardActivation: true
    property real corner: -1 // a radius of the host's (-1: the style's)
    property string role // where it stands (Hook.role: "footer", "toolbar", "dialog" …)
    property real dpr: 0 // the window's, passed by a host that has it (GitHub #111)
    // Screen.* only when no host passed it: a default binding to it would make a
    // Screen object even where the host's value replaces it.
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio
    readonly property color foreground: surface.out("foreground", highlighted ? Theme.accentText : Theme.text)

    signal clicked()

    implicitWidth: Math.max(32, content.implicitWidth + (text.length > 0 ? 24 : 16))
    implicitHeight: 36
    radius: corner >= 0 ? corner : Theme.controlRadius
    opacity: enabled ? 1 : surface.out("disabledOpacity", 0.45)
    activeFocusOnTab: keyboardActivation
    color: "transparent"

    Hook { // the style's key (Theme.keySurface)
        id: surface
        anchors.fill: parent
        part: Theme.keySurface
        hovered: area.containsMouse
        pressed: area.pressed
        highlighted: button.highlighted
        flat: button.flat
        corner: button.corner
        role: button.role
        dpr: button.pixelRatio
    }

    Keys.onSpacePressed: if (keyboardActivation) button.clicked()
    Keys.onReturnPressed: if (keyboardActivation) button.clicked()
    Keys.onEnterPressed: if (keyboardActivation) button.clicked()

    Row {
        id: content
        anchors.centerIn: parent
        anchors.verticalCenterOffset: surface.out("contentOffsetY", 0)
        spacing: 6

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.glyph.length > 0
            glyph: button.glyph
            size: 14
            color: button.foreground
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.text.length > 0
            text: button.text
            color: button.foreground
            font.pixelSize: Theme.fontBody
            font.weight: surface.out("fontWeight", Font.Normal)
        }
        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.trailingGlyph.length > 0
            glyph: button.trailingGlyph
            size: 10
            color: button.foreground
        }
    }

    Rectangle { // keyboard focus
        visible: button.activeFocus
        anchors.fill: parent
        anchors.margins: -3
        radius: Theme.largeControlRadius
        color: "transparent"
        border.width: 2
        border.color: Theme.text
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
