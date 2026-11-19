import QtQuick
import WinShun

// The small key beside 捐赠 (GitHub #402): × in the settings' title bar asks
// to move the button to the 拾穗计划 page (DonateMoveDialog), the arrow on
// that page puts it back. Flat until hovered, its icon quieter than words;
// `ink` and `strongInk` (hovered) for the page's warm field.
Item {
    id: key

    property string iconName
    property string tip
    property real dpr: 1
    property color ink: Theme.subtext
    property color strongInk: Theme.text

    signal clicked()

    implicitWidth: 24
    implicitHeight: 28

    Accessible.role: Accessible.Button
    Accessible.name: tip
    Accessible.onPressAction: key.clicked()

    Hook { // the style's key (Theme.keySurface), flat: nothing until the pointer comes
        anchors.fill: parent
        part: Theme.keySurface
        hovered: area.containsMouse
        pressed: area.pressed
        flat: true
        dpr: key.dpr
    }
    Glyph {
        anchors.centerIn: parent
        iconName: key.iconName
        size: 12
        color: area.containsMouse ? key.strongInk : key.ink
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: key.clicked()
    }
    HoverTip {
        target: area.containsMouse ? key : null
        text: key.tip
        dpr: key.dpr
    }
}
