pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import WinShun

// Where a new gesture or multi-button combo is drawn: a recessed area in its
// card, driven by PadReader's window event filter so multi-button chords
// (hold Right/Middle/Side + optional stroke + press a second button) are
// captured directly without MouseArea's single-button limitation.
Item {
    id: pad

    required property GestureEditor editor
    property string app // the program the new gesture is for ("" : all)
    property bool selection: false // for words selected
    property bool blocked: false // true while a picker popup is open
    property real dpr: Screen.devicePixelRatio
    property string message // why a recording was not taken, for a moment
    property bool preferPin: true // in multi-button mode, scope newly drawn gestures to the held button by default
    readonly property bool multiButton: editor.buttons.length > 1
    readonly property bool effectivePin: multiButton && preferPin
    readonly property bool drawing: reader.drawing

    width: parent ? parent.width : 0
    height: multiButton ? 188 : 152

    PadReader {
        id: reader
        target: area
        dpr: pad.dpr
        chinese: pad.editor.chinese
        active: !pad.blocked
        pinTrigger: pad.effectivePin
        enabledButtons: pad.editor.buttons

        onFinished: (gesture, tooLong, leftPrimaryChord) => {
            if (leftPrimaryChord) {
                pad.message = qsTr("左键只能作为组合手势的第二个按键，请先按住右键、中键或侧键")
            } else if (tooLong) {
                pad.message = qsTr("最多 6 段，画简单一点")
            } else if (gesture.length === 0) {
                pad.message = qsTr("没读出方向：请画长一点，或按住后再点另一个鼠标键")
            } else {
                pad.message = ""
                pad.editor.addOrFind(gesture, 0, pad.selection, pad.app)
                return
            }
            messageTimer.restart()
        }
    }

    Timer {
        id: messageTimer
        interval: 2800
        onTriggered: pad.message = ""
    }

    // Shown only when multiple trigger buttons are enabled: lets the user choose
    // whether new gestures recorded on the pad bind to the button physically held
    // (e.g. X1-only) or work with any enabled trigger button.
    Row {
        id: scopeBar
        visible: pad.multiButton
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: 10
        height: visible ? 28 : 0
        spacing: 8

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("录制按键范围：")
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }
        FlatButton {
            objectName: "scopePinButton"
            anchors.verticalCenter: parent.verticalCenter
            implicitHeight: 26
            fontSize: Theme.fontCaption
            highlighted: pad.preferPin
            text: qsTr("按所按按键")
            dpr: pad.dpr
            onClicked: pad.preferPin = true
        }
        FlatButton {
            objectName: "scopeAnyButton"
            anchors.verticalCenter: parent.verticalCenter
            implicitHeight: 26
            fontSize: Theme.fontCaption
            highlighted: !pad.preferPin
            text: qsTr("任意启用按键")
            dpr: pad.dpr
            onClicked: pad.preferPin = false
        }
    }

    Rectangle { // the recess: darker than the card, an edge inside it
        id: recess
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.top: pad.multiButton ? scopeBar.bottom : parent.top
        anchors.margins: 12
        anchors.topMargin: pad.multiButton ? 8 : 12
        radius: Theme.controlRadius
        color: Theme.page

        Hook {
            anchors.fill: parent
            part: Theme.cardPart
            corner: Theme.controlRadius
            extra: ({ inset: true })
            dpr: pad.dpr
        }

        Column { // at rest: what it is for
            anchors.centerIn: parent
            width: parent.width - 32
            spacing: 8
            visible: !pad.drawing

            Glyph {
                anchors.horizontalCenter: parent.horizontalCenter
                glyph: "ph:mouse-right-click"
                size: 36
                color: Theme.faint
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: pad.message.length > 0 ? pad.message : qsTr("按住手势键画方向，或点另一个键组合，松开选动作")
                color: pad.message.length > 0 ? Theme.danger : Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }

        Item { // the trail, kept inside the recess
            anchors.fill: parent
            anchors.margins: 1
            clip: true

            Shape {
                anchors.fill: parent
                visible: pad.drawing && reader.points.length >= 2
                preferredRendererType: Shape.GeometryRenderer

                ShapePath {
                    strokeColor: Theme.accent
                    strokeWidth: 3
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin

                    PathPolyline { path: reader.points }
                }
            }
        }

        Column { // while drawing: what is read (or prompt before movement/chord)
            anchors.centerIn: parent
            width: parent.width - 24
            spacing: 4
            visible: pad.drawing

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: reader.arrows.length > 0
                text: reader.arrows
                color: Theme.text
                font.pixelSize: reader.arrows.length <= 3 ? 36 : 26
                font.weight: Theme.fontStrong
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: reader.tooLong ? qsTr("最多 6 段，画简单一点")
                    : reader.hasChord ? qsTr("已记录组合键，松开后选它要做的事")
                    : reader.arrows.length > 0 ? qsTr("松开选动作，或再点另一个鼠标键组合")
                    : qsTr("拖动画出方向，或直接点另一个鼠标键组合")
                color: reader.tooLong ? Theme.danger : Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }

        // PadReader's window event filter captures all 5 mouse buttons on this item;
        // MouseArea only provides the crosshair cursor without stealing multi-button events.
        Item {
            id: area
            objectName: "padArea"
            anchors.fill: parent

            HoverHandler { cursorShape: Qt.CrossCursor }
        }
    }
}
