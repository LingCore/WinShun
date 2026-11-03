pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The presenter's ink on one monitor (GitHub #189, docs/design-presenter.md
// 4.2): a transparent, topmost window the size of the monitor, made by
// PresenterHost. What is not ink shows the live desktop; W and K put a
// board beneath the ink of their own. The mouse and keys go to the
// monitor's PresenterScreen, which hands them to its annotation model; the
// laser pointer's trail is this window's own. While the presenter lets the
// mouse through, the host makes the window transparent for input.
Window {
    id: window

    required property Presenter presenter
    required property PresenterScreen screenModel

    readonly property AnnotationModel annot: screenModel.model
    readonly property real dpr: screenModel.dpr
    readonly property real unit: 1 / dpr // image px to logical px
    readonly property bool onBoard: screenModel.board !== 0
    readonly property int boardFadeMs: 120

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"
    title: qsTr("演示")

    onActiveChanged: if (active) ink.forceActiveFocus()

    Item {
        id: ink

        anchors.fill: parent
        focus: true
        Component.onCompleted: window.screenModel.cursorItem = ink

        Keys.onPressed: (event) => { event.accepted = window.screenModel.keyPressed(event.key, event.modifiers) }

        AnnotationCanvas { // the ink on the desktop
            anchors.fill: parent
            model: window.screenModel.desk
            sparse: true
            live: true
            scaleX: window.unit
            scaleY: window.unit
        }

        Rectangle { // the board
            anchors.fill: parent
            color: window.screenModel.board === 2 ? "#1E2124" : "#FFFFFF"
            opacity: window.onBoard ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { OpacityAnimator { duration: window.boardFadeMs; easing.type: Easing.OutCubic } }
        }

        AnnotationCanvas { // the ink on the board
            anchors.fill: parent
            model: window.screenModel.boardModel
            visible: window.onBoard && model !== null
            sparse: true
            live: true
            scaleX: window.unit
            scaleY: window.unit
        }

        MouseArea { // beneath the text box and the number box: their presses stay with them
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: (m) => {
                window.screenModel.mousePressed(m.x, m.y, m.button, m.modifiers)
                if (m.button === Qt.LeftButton && window.presenter.laser)
                    laser.press(m.x, m.y)
            }
            onPositionChanged: (m) => {
                window.screenModel.mouseMoved(m.x, m.y, m.modifiers)
                laser.move(m.x, m.y)
            }
            onReleased: (m) => {
                window.screenModel.mouseReleased(m.x, m.y, m.button)
                if (m.button === Qt.LeftButton)
                    laser.release()
            }
            onDoubleClicked: (m) => {
                if (m.button === Qt.LeftButton)
                    window.screenModel.mouseDoubleClicked(m.x, m.y)
            }
            onWheel: (w) => {
                window.screenModel.wheeled(w.angleDelta.y !== 0 ? w.angleDelta.y : w.angleDelta.x)
                w.accepted = true
            }
        }

        AnnotateObjects { // the selected objects' handles, the text box, the number box
            anchors.fill: parent
            annot: window.annot
            keyTarget: ink
            sx: window.unit
            sy: window.unit
            dpr: window.dpr
            onKeyForwarded: (key, modifiers) => window.screenModel.keyPressed(key, modifiers)
        }

        LaserTrail {
            id: laser
            color: window.presenter.laserColor
            lineWidth: 10
        }
    }

    Connections {
        target: window.presenter
        function onModeChanged() {
            if (window.presenter.mode !== 1)
                laser.clear()
        }
    }
}
