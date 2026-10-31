pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// take_screenshot (MCP, App::confirmScreenshot): an AI client asks for a
// picture of a monitor. Shown in the middle of that monitor, above the rest,
// and kept out of every capture (WDA_EXCLUDEFROMCAPTURE). Only a click on
// 允许 takes the picture; 拒绝, Esc or no answer in time refuse. Enter does
// nothing: a key meant for another window must not say yes.
Window {
    id: window

    required property WindowFrame frame
    required property string client // who asks
    required property string monitor // which monitor, in words
    property int secondsLeft: 30

    // 1: allowed; 0: refused; -1: no answer in time.
    signal answered(int how)

    readonly property int padding: 20
    readonly property real dpr: Screen.devicePixelRatio
    property bool done: false

    function answer(how) {
        if (done)
            return
        done = true
        countdown.stop()
        answered(how)
    }

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: Theme.backdrop && SystemTheme.materials ? "transparent" : Theme.background
    title: qsTr("AI 请求截屏") // Qt adds " - Win顺"
    width: 440
    height: header.height + body.implicitHeight + buttons.height + 2 * window.padding + 12

    Timer {
        id: countdown
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            window.secondsLeft -= 1
            if (window.secondsLeft <= 0)
                window.answer(-1)
        }
    }

    Item {
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: window.answer(0)
    }

    Item {
        id: header
        width: parent.width
        height: Theme.headerHeight
        Component.onCompleted: window.frame.addDragArea(header)

        Glyph {
            id: headerIcon
            x: window.padding
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            glyph: "" // Camera
            size: 16
            color: Theme.text
        }
        Text {
            anchors.left: headerIcon.right
            anchors.leftMargin: 8
            y: window.onPixel((header.height - height) / 2)
            text: qsTr("AI 请求截屏")
            color: Theme.text
            font.pixelSize: Theme.uiTitle
            font.weight: Theme.fontStrong
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.edge(Screen.devicePixelRatio)
            color: Theme.divider
        }
    }

    Column {
        id: body
        x: window.padding
        anchors.top: header.bottom
        anchors.topMargin: window.padding
        width: parent.width - 2 * window.padding
        spacing: 8

        Text {
            width: parent.width
            text: qsTr("“%1”想截取 %2 的画面。").arg(window.client).arg(window.monitor)
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            text: qsTr("点“允许”才截，图只交给它，不进剪贴板历史。这个窗口不会被截进去。")
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            text: qsTr("%1 秒后自动拒绝").arg(window.secondsLeft)
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }
    }

    Row {
        id: buttons
        anchors.right: parent.right
        anchors.rightMargin: window.padding
        anchors.bottom: parent.bottom
        anchors.bottomMargin: window.padding
        height: denyButton.implicitHeight
        spacing: 8

        FlatButton {
            id: denyButton
            text: qsTr("拒绝")
            onClicked: window.answer(0)
        }
        FlatButton {
            highlighted: true
            keyboardActivation: false
            text: qsTr("允许")
            onClicked: window.answer(1)
        }
    }

    WindowEdge {}
}
