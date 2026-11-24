import QtQuick
import WinShun

// Which trigger mouse button a gesture rule responds to: "任意按键" (any enabled
// trigger button), or a specific button ("仅右键", "仅中键", "仅侧键（后退）", "仅侧键（前进）").
// Styled identically to ZoneButton so metadata links stay cohesive and quiet.
Item {
    id: trigger

    required property GestureEditor editor
    required property int ruleRow
    required property string triggerTitle
    property bool triggerDisabled: false
    property string app
    property real dpr: 0

    implicitWidth: words.implicitWidth + 4 + chevron.width
    implicitHeight: 20
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: triggerTitle

    Keys.onSpacePressed: menu.open = !menu.open
    Keys.onReturnPressed: menu.open = !menu.open

    Text {
        id: words
        anchors.verticalCenter: parent.verticalCenter
        text: trigger.triggerTitle
        color: trigger.triggerDisabled ? Theme.danger : area.containsMouse || menu.open ? Theme.text : Theme.subtext
        font.pixelSize: Theme.fontCaption
    }
    Glyph {
        id: chevron
        anchors.left: words.right
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        glyph: "ph:caret-down"
        size: 10
        color: words.color
    }
    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -4
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: menu.open = !menu.open
    }
    DropMenu {
        id: menu
        anchor: trigger
        menuWidth: 184
        entries: {
            trigger.editor.revision
            return trigger.editor.buttonChoices(trigger.ruleRow)
        }
        onActivated: (index) => trigger.editor.setTriggerButton(trigger.ruleRow, entries[index].button, trigger.app)
    }
}
