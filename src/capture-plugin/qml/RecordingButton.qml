pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

Button {
    id: button
    property string glyph
    property string tip
    property bool tipsEnabled: false
    property bool selectionFill: true

    implicitWidth: Math.max(32, row.implicitWidth + (text.length > 0 ? 20 : 12))
    implicitHeight: 36
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    font.pixelSize: Theme.fontCaption
    Accessible.name: tip.length > 0 ? tip : text

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            spacing: 6
            anchors.centerIn: parent
            Glyph {
                visible: button.glyph.length > 0
                anchors.verticalCenter: parent.verticalCenter
                glyph: button.glyph
                size: 16
                color: button.highlighted ? Theme.accentText : button.enabled ? Theme.text : Theme.faint
            }
            Text {
                visible: button.text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: button.text
                font: button.font
                color: button.highlighted ? Theme.accentText : button.enabled ? Theme.text : Theme.faint
            }
        }
    }
    background: Rectangle {
        radius: Theme.controlRadius
        color: button.highlighted ? (button.down ? Qt.darker(Theme.accentFill, 1.15) : button.hovered ? Qt.lighter(Theme.accentFill, 1.08) : Theme.accentFill)
             : button.down ? Theme.controlPressed : button.hovered ? Theme.controlHover : button.checked && button.selectionFill ? Theme.selection : "transparent"
        border.width: button.visualFocus ? 2 : 0
        border.color: Theme.text
    }
    ToolTip {
        id: tooltip
        parent: button
        popupType: Popup.Window
        visible: button.tipsEnabled && button.Window.window && button.Window.window.visible
                 && button.tip.length > 0 && (button.hovered || button.visualFocus)
        delay: 450
        timeout: 5000
        width: Math.min(480, implicitWidth, Screen.desktopAvailableWidth - 16)
        text: button.tip
        font.pixelSize: Theme.fontCaption
        padding: 8
        contentItem: Text { text: button.tip; font: tooltip.font; color: Theme.text; wrapMode: Text.Wrap }
        background: Rectangle {
            color: Theme.menuBackground
            radius: Theme.controlRadius
            border.width: 1
            border.color: Theme.controlBorder
        }
        enter: null
        exit: null
    }
}
