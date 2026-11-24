pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// One gesture of a list, a row of a card as the settings window's rows are:
// the gesture in a small flat tile, what it is and where it counts beside
// it, what it does as the control on the right (its picker opens under it),
// and a delete button that shows while the pointer is on the row. The row
// itself and its card take no hover.
Item {
    id: row

    // The rules model's roles.
    required property int sourceRow
    required property string arrows
    required property string description
    required property string whereTitle
    required property string triggerButton
    required property string triggerButtonTitle
    required property bool triggerButtonDisabled
    required property bool selection
    required property string kind
    required property string actionTitle
    required property string actionNote
    required property string actionIcon
    required property string actionId
    required property bool provisional

    required property GestureEditor editor
    required property ActionPicker picker
    property string app // the program the list is for ("" : all)
    property bool showSelection: true // false on the page that is only of gestures with words selected
    property real dpr: Screen.devicePixelRatio
    readonly property real edge: Theme.edge(row.dpr)
    readonly property bool deleteShown: rowHover.hovered || removeArea.containsMouse
    readonly property bool showTriggerButton: row.editor.buttons.length > 1 || row.triggerButton.length > 0

    // Asks what it does: its picker opens under the action. A gesture just drawn, not given one, is dropped if the picker closes without a choice.
    function askAction() {
        row.picker.ask(actionButton, { kind: row.provisional ? "none" : row.kind, id: row.actionId },
            (kind, value) => row.editor.setAction(row.sourceRow, kind, value, row.app),
            () => { if (row.provisional) row.editor.discardProvisional() })
    }

    // Draws the eye to the row (a gesture drawn again that was set already).
    function flash() {
        if (SystemTheme.animations) {
            flashAnimation.restart()
        } else {
            flashMark.opacity = 0.14
            flashTimer.restart()
        }
    }

    width: parent ? parent.width : 0
    height: Theme.snap(56, row.dpr)

    Hook { // separates rows inside a card (Theme.separator); the first row has none above it
        x: row.edge
        width: parent.width - 2 * row.edge
        height: row.edge
        part: row.y > 0 ? Theme.separator : null
        extra: ({ card: true })
        orientation: Qt.Horizontal
        dpr: row.dpr
    }

    HoverHandler { id: rowHover }

    Rectangle { // see flash()
        id: flashMark
        anchors.fill: parent
        anchors.margins: row.edge
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

    Rectangle { // the gesture: a tag, not a button
        id: tile
        x: row.width < 560 ? 14 : 18
        anchors.verticalCenter: parent.verticalCenter
        width: row.width < 560 ? 56 : 64
        height: 32
        radius: Theme.smallControlRadius
        color: Theme.tagFill

        Text {
            anchors.centerIn: parent
            width: parent.width - 8
            horizontalAlignment: Text.AlignHCenter
            text: row.arrows
            color: Theme.text
            font.pixelSize: row.arrows.length <= 2 ? 18 : 14
            font.weight: Theme.fontStrong
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: 10
        }
    }

    Column {
        anchors.left: tile.right
        anchors.leftMargin: row.width < 560 ? 10 : 14
        anchors.right: actionButton.left
        anchors.rightMargin: row.width < 560 ? 10 : 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Text {
            width: parent.width
            text: row.description
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        Row {
            id: metaRow
            width: parent.width
            height: 20
            spacing: 6
            clip: true

            Text {
                id: selectionLabel
                visible: row.showSelection && row.selection
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("选中文字时") + " ·"
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
            TriggerButton {
                id: triggerBtn
                visible: row.showTriggerButton
                objectName: "triggerButton"
                editor: row.editor
                ruleRow: row.sourceRow
                triggerTitle: row.triggerButtonTitle
                triggerDisabled: row.triggerButtonDisabled
                app: row.app
                dpr: row.dpr
            }
            Text {
                id: triggerSep
                visible: row.showTriggerButton
                anchors.verticalCenter: parent.verticalCenter
                text: "·"
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
            ZoneButton {
                id: zoneBtn
                objectName: "zoneButton"
                editor: row.editor
                ruleRow: row.sourceRow
                whereTitle: row.whereTitle
                app: row.app
                dpr: row.dpr
            }
            Text {
                visible: row.actionNote.length > 0
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, metaRow.width
                    - (selectionLabel.visible ? selectionLabel.implicitWidth + metaRow.spacing : 0)
                    - (triggerBtn.visible ? triggerBtn.implicitWidth + triggerSep.implicitWidth + 2 * metaRow.spacing : 0)
                    - zoneBtn.implicitWidth - metaRow.spacing)
                elide: Text.ElideRight
                text: "· " + row.actionNote
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    ActionButton {
        id: actionButton
        objectName: "ruleActionButton"
        anchors.right: removeButton.left
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        width: row.width < 560 ? 156 : 224 // the program page has half the width
        dpr: row.dpr
        open: row.picker.open && row.picker.anchor === actionButton
        asking: row.provisional
        unset: row.kind === "nothing"
        invalid: row.kind === "invalid"
        icon: row.provisional ? "ph:plus" : row.actionIcon
        text: row.provisional ? qsTr("选一个动作…") : row.kind === "nothing" ? qsTr("什么都不做") : row.actionTitle
        onClicked: row.askAction()
    }

    Item { // delete: shown while the pointer is on the row
        id: removeButton
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        width: 28
        height: 28
        visible: row.deleteShown

        Hook {
            id: removePart
            anchors.fill: parent
            part: Theme.keySurface
            hovered: removeArea.containsMouse
            pressed: removeArea.pressed
            flat: true
            corner: Theme.smallControlRadius
            role: "toolbar"
            dpr: row.dpr
        }
        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: removePart.out("contentOffsetY", 0)
            glyph: "ph:trash"
            size: 14
            color: removeArea.containsMouse ? Theme.danger : Theme.subtext
        }
        MouseArea {
            id: removeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: row.editor.remove(row.sourceRow, row.app)
        }
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("删除这个手势")
    }
}
