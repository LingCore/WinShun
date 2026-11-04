pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The words read from a capture or a pin (识别文字, CaptureOcr): beside the
// picture, one window used again each time. The text can be selected and
// edited before it goes anywhere: 复制全部 puts it on the clipboard (into
// the history), 用它搜索 takes the selection, else all of it, as one line
// into the launcher's search box, 网页搜索 into the default web search.
// Esc or 关闭 closes it.
Window {
    id: window

    required property CaptureOcr ocr
    required property WindowFrame frame

    readonly property int padding: 16
    readonly property real dpr: Screen.devicePixelRatio

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: Theme.backdrop && SystemTheme.materials ? "transparent" : Theme.background
    title: qsTr("识别文字") // Qt adds " - Win顺"
    width: 460
    height: 360

    // A new picture: its text replaces whatever was edited before.
    Connections {
        target: window.ocr
        function onChanged() {
            if (!window.ocr.busy) {
                words.text = window.ocr.text
                words.cursorPosition = 0
                words.forceActiveFocus()
            } else {
                words.text = ""
            }
        }
    }

    Item {
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: window.ocr.close()
    }

    // ---- the header: what this is, close -------------------------------------------

    Item {
        id: header
        width: parent.width
        height: Theme.headerHeight
        Component.onCompleted: window.frame.addDragArea(header)

        Text {
            x: window.padding
            y: window.onPixel((header.height - height) / 2)
            text: window.ocr.title.length > 0 ? window.ocr.title : qsTr("识别文字")
            color: Theme.text
            font.pixelSize: Theme.uiTitle
            font.weight: Theme.fontStrong
        }
        FlatButton {
            id: closeButton
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            flat: true
            width: 32
            height: 32
            glyph: "" // ChromeClose
            onClicked: window.ocr.close()
            Component.onCompleted: window.frame.addControl(closeButton)
        }
        Hook { // the style's separator
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.edge(window.dpr)
            part: Theme.separator
            orientation: Qt.Horizontal
            dpr: window.dpr
        }
    }

    // ---- the text ---------------------------------------------------------------------

    Item {
        id: field
        anchors.top: header.bottom
        anchors.topMargin: 12
        anchors.left: parent.left
        anchors.leftMargin: window.padding
        anchors.right: parent.right
        anchors.rightMargin: window.padding
        anchors.bottom: footer.top

        Hook { // the style's input field (Theme.inputField)
            id: fieldPart
            anchors.fill: parent
            part: Theme.inputField
            focused: words.activeFocus
            hovered: fieldHover.hovered
            dpr: window.dpr
        }
        HoverHandler { id: fieldHover }

        Flickable {
            id: flick
            anchors.fill: parent
            anchors.margins: 10
            anchors.rightMargin: 14 // room for the scroll bar
            clip: true
            contentWidth: width
            contentHeight: words.height
            boundsBehavior: Flickable.StopAtBounds
            visible: !window.ocr.busy && window.ocr.message.length === 0

            function ensureVisible(r) {
                if (contentY >= r.y)
                    contentY = r.y
                else if (contentY + height <= r.y + r.height)
                    contentY = r.y + r.height - height
            }

            TextEdit {
                id: words
                width: flick.width
                height: Math.max(contentHeight, flick.height) // a click below short text still lands in it
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                color: fieldPart.out("textColor", Theme.text)
                selectionColor: Theme.textSelection
                selectedTextColor: Theme.text
                font.pixelSize: Theme.uiBody
                onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
                Keys.onEscapePressed: (event) => {
                    if (words.inputMethodComposing) // Esc while composing belongs to the input method
                        event.accepted = false
                    else
                        window.ocr.close()
                }
            }
        }

        ListScrollBar {
            flickable: flick
            visible: flick.visible && flick.contentHeight > flick.height
            anchors.right: parent.right
            anchors.top: flick.top
            anchors.bottom: flick.bottom
        }

        Column { // reading, or the next action when the component is missing
            anchors.centerIn: parent
            width: parent.width - 32
            visible: !flick.visible
            spacing: 12
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: window.ocr.busy ? qsTr("正在识别…") : window.ocr.message
                color: Theme.subtext
                font.pixelSize: Theme.uiBody
            }
            FlatButton {
                objectName: "setupEnhancedOcr"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: window.ocr.needsPackage
                text: qsTr("安装增强文字识别…")
                onClicked: window.ocr.setupRecognition()
            }
        }
    }

    // ---- what to do with it ------------------------------------------------------------

    Row {
        id: footer
        anchors.right: parent.right
        anchors.rightMargin: window.padding
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        topPadding: 12
        spacing: 8

        FlatButton {
            text: qsTr("用它搜索")
            enabled: !window.ocr.busy && words.text.trim().length > 0
            onClicked: window.ocr.search(words.selectedText.length > 0 ? words.selectedText : words.text)
        }
        FlatButton { // a QR code that holds a web address: opened only by this click
            visible: window.ocr.link.length > 0
            highlighted: true
            text: qsTr("打开链接")
            onClicked: window.ocr.openLink(window.ocr.link)
        }
        FlatButton {
            visible: window.ocr.link.length === 0
            text: qsTr("网页搜索")
            enabled: !window.ocr.busy && words.text.trim().length > 0
            onClicked: window.ocr.webSearch(words.selectedText.length > 0 ? words.selectedText : words.text)
        }
        FlatButton {
            highlighted: window.ocr.link.length === 0
            text: qsTr("复制全部")
            enabled: !window.ocr.busy && words.text.length > 0
            onClicked: window.ocr.copyAll(words.text)
        }
        FlatButton {
            text: qsTr("关闭")
            onClicked: window.ocr.close()
        }
    }

    WindowEdge {
        id: windowEdge
        fill: Theme.background
    }
}
