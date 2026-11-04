import QtQuick
import QtQuick.Controls.Basic
import WinShun

Button {
    id: chip
    property real maximumWidth: 280
    implicitWidth: Math.min(maximumWidth, Math.ceil(measure.advanceWidth) + 24)
    implicitHeight: 28
    checkable: true
    padding: 8
    font.pixelSize: Theme.uiCaption
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    TextMetrics { id: measure; text: chip.text; font: chip.font }
    contentItem: Text {
        text: chip.text
        font.pixelSize: chip.font.pixelSize
        font.weight: surface.out("fontWeight", chip.font.weight)
        color: surface.out("foreground", chip.checked ? Theme.accentText : chip.hovered ? Theme.text : Theme.subtext)
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Hook { // the style's chip (role "filter"): a file's matches, ContentMatchesView
        id: surface
        part: Theme.chipSurface
        checked: chip.checked
        hovered: chip.hovered
        pressed: chip.pressed
        focused: chip.activeFocus
        corner: Theme.controlRadius
        role: "filter"

        Rectangle { // the keyboard's ring, the host's own: a part may draw no focus
            anchors.fill: parent
            z: 1 // over the part, made after it
            visible: chip.activeFocus
            radius: Theme.pillRadius(height)
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
        }
    }
}
