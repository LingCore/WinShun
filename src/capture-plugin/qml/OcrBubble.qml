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

    readonly property int padding: 14
    readonly property real dpr: Screen.devicePixelRatio
    readonly property real headerWidth: (mark.visible ? mark.width + 8 : 0) + headline.implicitWidth + 8 + buttons.implicitWidth

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
    width: Math.min(420, Math.max(320 + 16 * Theme.step, Math.ceil(headerWidth) + 2 * padding))
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
        spacing: 8

        Item {
            width: parent.width
            height: Math.max(headline.implicitHeight, 28)

            Glyph {
                id: mark
                anchors.verticalCenter: parent.verticalCenter
                visible: bubble.phase !== "reading"
                glyph: bubble.phase === "copied" ? String.fromCharCode(0xE73E) : String.fromCharCode(0xE7BA) // CheckMark, Warning
                size: 18
                color: bubble.phase === "copied" ? Theme.success : Theme.caution
            }
            Text {
                id: headline
                objectName: "ocrBubbleHeadline"
                anchors.left: mark.visible ? mark.right : parent.left
                anchors.leftMargin: mark.visible ? 8 : 0
                anchors.right: buttons.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: bubble.phase === "reading" ? qsTr("正在识别…")
                    : bubble.phase === "copied" ? qsTr("已复制识别的文字") : qsTr("没识别到文字")
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: Theme.uiTitle
                font.weight: Theme.fontStrong
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                FlatButton {
                    objectName: "ocrBubbleEdit"
                    dpr: bubble.dpr
                    visible: bubble.phase === "copied"
                    flat: true
                    role: "toolbar"
                    keyboardActivation: false
                    corner: Theme.smallControlRadius
                    height: 28
                    fontSize: Theme.uiCaption
                    glyphSize: 2 * Math.round(Math.min(16, Theme.uiCaption + 2) / 2)
                    glyph: String.fromCharCode(0xE70F) // Edit
                    text: qsTr("编辑")
                    onClicked: {
                        bubble.host.editOcrText(bubble.words)
                        bubble.dismiss()
                    }
                }
                FlatButton {
                    objectName: "ocrBubbleClose"
                    dpr: bubble.dpr
                    anchors.verticalCenter: parent.verticalCenter
                    flat: true
                    role: "toolbar"
                    keyboardActivation: false
                    corner: Theme.smallControlRadius
                    width: 28
                    height: 28
                    glyphSize: 12
                    glyph: String.fromCharCode(0xE711) // Cancel
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("关闭")
                    onClicked: bubble.dismiss()
                }
            }
        }

        Text { // the first lines of what was copied, or why nothing was
            objectName: "ocrBubbleBody"
            width: parent.width
            visible: text.length > 0
            text: bubble.phase === "copied" ? bubble.words : bubble.phase === "none" ? bubble.detail : ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: bubble.phase === "copied" ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiCaption
            lineHeight: 1.3
        }
    }

    WindowEdge {
        id: windowEdge
        objectName: "ocrBubbleEdge"
        fill: Theme.background
    }
}
