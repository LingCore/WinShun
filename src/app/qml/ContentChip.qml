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
        font: chip.font
        color: chip.checked ? Theme.accent : Theme.subtext
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.controlRadius
        color: chip.checked ? Theme.badge : chip.hovered ? Theme.hover : Theme.control
        border.width: chip.activeFocus ? 2 : 0
        border.color: Theme.accent
    }
}
