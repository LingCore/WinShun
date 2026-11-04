pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// 识别屏幕文字's answer (#198, CaptureApp::readScreenText): a small card by the
// region read, while it reads (正在识别…), then 已复制 with the first lines,
// or 没识别到文字. It never takes the focus, so Ctrl+V goes on into the window
// in front (WindowFrame::setNoActivate, Qt::WindowDoesNotAcceptFocus); it
// goes after a few seconds, and stays while the mouse is on it. 编辑 opens the
// words in the OCR window (OcrPanel), which takes the focus and closes on Esc.
// `host` is the CaptureApp: editOcrText.
Window {
    id: bubble

    required property WindowFrame frame
    required property QtObject host

    // "reading", "copied" or "none"
    property string phase: "reading"
    property string words
    property string detail // why nothing was read, when it is not just no words

    readonly property int padding: 12

    // Called by the app with each change, before it places the window.
    function present(newPhase, newWords, newDetail) {
        phase = newPhase
        words = newWords
        detail = newDetail
        hideTimer.restart()
    }

    function dismiss() {
        hideTimer.stop()
        visible = false
    }

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.backdrop && SystemTheme.materials ? "transparent" : Theme.background
    title: qsTr("识别文字") // Qt adds " - Win顺"
    width: 300 + 12 * Theme.step
    height: body.implicitHeight + 2 * padding

    Timer {
        id: hideTimer
        interval: bubble.phase === "copied" ? 4000 : 3000
        running: bubble.visible && bubble.phase !== "reading" && !hover.hovered
        onTriggered: bubble.dismiss()
    }

    Item { // the whole window: the timer waits while the mouse is anywhere on it
        anchors.fill: parent

        HoverHandler {
            id: hover
        }
    }

    Column {
        id: body
        x: bubble.padding
        y: bubble.padding
        width: parent.width - 2 * bubble.padding
        spacing: 6

        Item {
            width: parent.width
            height: Math.max(headline.implicitHeight, 28)

            Glyph {
                id: mark
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                visible: bubble.phase !== "reading"
                glyph: bubble.phase === "copied" ? String.fromCharCode(0xE73E) : String.fromCharCode(0xE7BA) // CheckMark, Warning
                size: 14
                color: bubble.phase === "copied" ? Theme.success : Theme.caution
            }
            Text {
                id: headline
                anchors.left: mark.visible ? mark.right : parent.left
                anchors.leftMargin: mark.visible ? 8 : 0
                anchors.right: buttons.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: bubble.phase === "reading" ? qsTr("正在识别…")
                    : bubble.phase === "copied" ? qsTr("已复制识别的文字") : qsTr("没识别到文字")
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: Theme.uiBody
                font.weight: Theme.fontStrong
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                FlatButton {
                    objectName: "ocrBubbleEdit"
                    visible: bubble.phase === "copied"
                    flat: true
                    height: 28
                    glyph: String.fromCharCode(0xE70F) // Edit
                    text: qsTr("编辑")
                    onClicked: {
                        bubble.host.editOcrText(bubble.words)
                        bubble.dismiss()
                    }
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    height: 24
                    radius: Theme.smallControlRadius
                    color: closeArea.pressed ? Theme.controlPressed : closeArea.containsMouse ? Theme.controlHover : "transparent"

                    Glyph {
                        anchors.centerIn: parent
                        glyph: String.fromCharCode(0xE711) // Cancel
                        size: 10
                        color: Theme.subtext
                    }
                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: bubble.dismiss()
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("关闭")
                }
            }
        }

        Text { // the first lines of what was copied, or why nothing was
            width: parent.width
            visible: text.length > 0
            text: bubble.phase === "copied" ? bubble.words : bubble.phase === "none" ? bubble.detail : ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: bubble.phase === "copied" ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiCaption
            lineHeight: 1.1
        }
    }

    WindowEdge {}
}
