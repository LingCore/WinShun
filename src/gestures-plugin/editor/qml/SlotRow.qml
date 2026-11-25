pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A trigger that has one action in all (a program's, with `app`): the wheel
// turned with the right button held. Laid out as RuleRow: a tile with the
// icon, what it is, what it does as the control that opens the picker, and
// a button that clears it while the pointer is on the row.
Item {
    id: row

    required property GestureEditor editor
    required property ActionPicker picker
    required property string trigger // "wheel:up"
    required property string title
    property string caption
    property string app
    property real dpr: Screen.devicePixelRatio
    readonly property real edge: Theme.edge(row.dpr)
    // The rule now: {has, kind, title, icon, note, id}; read again when the rules change.
    readonly property var info: {
        row.editor.revision
        return row.editor.slot(row.trigger, row.app)
    }
    readonly property bool clearShown: row.info.has && (rowHover.hovered || clearArea.containsMouse)

    function askAction() {
        row.picker.ask(actionButton, row.info,
            (kind, value) => row.editor.setSlot(row.trigger, row.app, kind, value), null)
    }

    width: parent ? parent.width : 0
    height: Theme.snap(56, row.dpr)

    Hook {
        x: row.edge
        width: parent.width - 2 * row.edge
        height: row.edge
        part: row.y > 0 ? Theme.separator : null
        extra: ({ card: true })
        orientation: Qt.Horizontal
        dpr: row.dpr
    }

    HoverHandler { id: rowHover }

    Rectangle {
        id: tile
        x: 18
        anchors.verticalCenter: parent.verticalCenter
        width: 56
        height: 32
        radius: Theme.smallControlRadius
        color: Theme.tagFill

        Glyph {
            anchors.centerIn: parent
            glyph: row.trigger === "wheel:up" ? "ph:arrow-line-up" : "ph:arrow-line-down"
            size: 20
            color: Theme.text
        }
    }

    Column {
        anchors.left: tile.right
        anchors.leftMargin: 14
        anchors.right: actionButton.left
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Text {
            width: parent.width
            text: row.title
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        Text {
            width: parent.width
            text: row.info.note.length > 0 ? row.caption + " · " + row.info.note : row.caption
            elide: Text.ElideRight
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }
    }

    ActionButton {
        id: actionButton
        anchors.right: clearButton.left
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        width: row.width < 560 ? 176 : 224 // the program page has half the width
        dpr: row.dpr
        open: row.picker.open && row.picker.anchor === actionButton
        unset: !row.info.has || row.info.kind === "nothing"
        invalid: row.info.kind === "invalid"
        icon: row.info.icon
        text: !row.info.has ? qsTr("未设置") : row.info.kind === "nothing" ? qsTr("什么都不做") : row.info.title
        onClicked: row.askAction()
    }

    Item { // clear: shown while the pointer is on the row
        id: clearButton
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        width: 28
        height: 28
        visible: row.clearShown

        Hook {
            id: clearPart
            anchors.fill: parent
            part: Theme.keySurface
            hovered: clearArea.containsMouse
            pressed: clearArea.pressed
            flat: true
            corner: Theme.smallControlRadius
            role: "toolbar"
            dpr: row.dpr
        }
        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: clearPart.out("contentOffsetY", 0)
            glyph: "ph:x"
            size: 16
            color: clearArea.containsMouse ? Theme.text : Theme.subtext
        }
        MouseArea {
            id: clearArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: row.editor.clearSlot(row.trigger, row.app)
        }
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("清除")
    }
}
