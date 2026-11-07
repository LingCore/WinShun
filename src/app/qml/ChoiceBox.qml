pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

// Keep Basic's editing and selection behavior; the theme owns one field surface.
ComboBox {
    id: control
    property real dpr: Screen.devicePixelRatio
    implicitHeight: 36
    flat: true // suppress Basic's second background inside an editable field
    font.pixelSize: Theme.fontBody
    opacity: enabled ? 1 : fieldSurface.out("disabledOpacity", 0.4)
    palette.text: fieldSurface.out("textColor", Theme.text)
    palette.buttonText: fieldSurface.out("textColor", Theme.text)
    palette.base: Theme.control
    palette.button: Theme.control
    palette.highlight: Theme.accentFill
    palette.highlightedText: Theme.accentText
    rightPadding: 34

    background: Hook {
        id: fieldSurface
        part: Theme.inputField
        hovered: control.hovered
        focused: control.activeFocus || control.popup.visible
        dpr: control.dpr
    }
    indicator: Glyph {
        x: control.mirrored ? 12 : control.width - width - 12
        y: (control.height - height) / 2
        glyph: "\uE70D"
        size: 12
        color: control.enabled ? Theme.subtext : Theme.faint
    }
    delegate: ItemDelegate {
        id: option
        required property int index
        width: ListView.view ? ListView.view.width : control.width
        height: Theme.menuItemHeight
        text: control.textAt(index)
        highlighted: control.highlightedIndex === index
        hoverEnabled: true
        font.pixelSize: Theme.menuTextSize
        font.weight: control.currentIndex === index ? Font.DemiBold : Font.Normal
        contentItem: Text {
            text: option.text
            font: option.font
            color: Theme.text
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: option.highlighted || option.hovered ? Theme.menuHover : "transparent"
            radius: Theme.menuItemRadius
            border.width: option.visualFocus ? Theme.edge(control.dpr) : 0
            border.color: Theme.accentLine
        }
    }
    popup: ComboPopup { control: control }
}
