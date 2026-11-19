pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import WinShun

// Where a new gesture is drawn: a recessed area in its card, to be drawn on
// with the right button (the left works too), read as the plugin reads a
// drag on the desktop (PadReader). While drawing the trail is the accent's
// and what is read shows large; on release the gesture is added to the list
// (or, set already, shown where it is).
Item {
    id: pad

    required property GestureEditor editor
    property string app // the program the new gesture is for ("" : all)
    property bool selection: false // for words selected
    property real dpr: Screen.devicePixelRatio
    property string message // why a drag was not taken, for a moment
    property var points: []
    readonly property bool drawing: area.pressed

    width: parent ? parent.width : 0
    height: 152

    PadReader { id: reader }

    Timer {
        id: messageTimer
        interval: 2200
        onTriggered: pad.message = ""
    }

    Rectangle { // the recess: darker than the card, an edge inside it
        id: recess
        anchors.fill: parent
        anchors.margins: 12
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
                size: 32
                color: Theme.faint
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: pad.message.length > 0 ? pad.message : qsTr("在这里按住右键画一个手势，松开后选它要做的事")
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
                visible: pad.drawing
                preferredRendererType: Shape.GeometryRenderer

                ShapePath {
                    strokeColor: Theme.accent
                    strokeWidth: 3
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin

                    PathPolyline { path: pad.points }
                }
            }
        }

        Column { // while drawing: what is read
            anchors.centerIn: parent
            spacing: 4
            visible: pad.drawing && reader.arrows.length > 0

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: reader.arrows
                color: Theme.text
                font.pixelSize: 36
                font.weight: Theme.fontStrong
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: reader.tooLong ? qsTr("最多 6 段，画简单一点") : qsTr("松开后选它要做的事")
                color: reader.tooLong ? Theme.danger : Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }

        MouseArea {
            id: area
            objectName: "padArea"
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            preventStealing: true // a drag draws; the page does not scroll
            cursorShape: Qt.CrossCursor

            onPressed: (mouse) => {
                pad.message = ""
                pad.points = [Qt.point(mouse.x, mouse.y)]
                reader.begin(mouse.x, mouse.y, pad.dpr)
            }
            onPositionChanged: (mouse) => {
                if (!pressed)
                    return
                const last = pad.points[pad.points.length - 1]
                if (Math.abs(mouse.x - last.x) + Math.abs(mouse.y - last.y) < 2)
                    return
                pad.points = pad.points.concat([Qt.point(mouse.x, mouse.y)])
                reader.add(mouse.x, mouse.y)
            }
            onReleased: pad.finish()
            onCanceled: {
                pad.points = []
                reader.clear()
            }
        }
    }

    function finish() {
        const gesture = reader.gesture
        const tooLong = reader.tooLong
        pad.points = []
        if (tooLong) {
            pad.message = qsTr("最多 6 段，画简单一点")
        } else if (gesture.length === 0) {
            pad.message = qsTr("没读出方向，再画长一点")
        } else {
            pad.editor.addOrFind(gesture, 0, pad.selection, pad.app)
            reader.clear()
            return
        }
        messageTimer.restart()
        reader.clear()
    }
}
