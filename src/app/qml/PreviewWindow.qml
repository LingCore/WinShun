pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Win顺's own preview of a result (FilePreview, Alt+P on a row): a picture
// fitted in (the wheel zooms, a double click goes to its own size and back),
// the start of a text file, a document through the handler Explorer's
// preview pane uses (a window of its own over the content area, see
// PreviewHost), or a large thumbnail and what is known about the file.
// Beside the launcher, never taking the focus from it: the keys stay
// there, Alt+P or the × closes this, ↑ ↓ show the next row's file here.
Window {
    id: window

    required property FilePreview preview
    required property WindowFrame frame

    readonly property int padding: 16
    readonly property real dpr: Screen.devicePixelRatio

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    // See-through to Mica, as the launcher; drawn as active though it never is (WindowFrame::setNoActivate).
    // (An own frame has no Mica: Theme.backdrop is false there.)
    color: Theme.backdrop && SystemTheme.materials ? "transparent" : Theme.background
    title: qsTr("预览") // Qt adds " - Win顺"

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    // ---- the header: the file, what it is, 打开 and close ----------------------------

    Item {
        id: header
        width: parent.width
        height: Theme.headerHeight
        Component.onCompleted: window.frame.addDragArea(header)

        Image {
            id: icon
            x: window.padding
            anchors.verticalCenter: parent.verticalCenter
            width: 24
            height: 24
            sourceSize: Qt.size(24 * window.dpr, 24 * window.dpr) // image://: logical × the scale
            source: window.preview.iconUrl
            asynchronous: true
            smooth: false
        }
        Column {
            anchors.left: icon.right
            anchors.leftMargin: 10
            anchors.right: buttons.left
            anchors.rightMargin: 12
            y: window.onPixel((header.height - height) / 2)
            spacing: 1

            Text {
                width: parent.width
                text: window.preview.name
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: Theme.uiTitle
                font.weight: Theme.fontStrong
            }
            Text {
                width: parent.width
                visible: text.length > 0
                text: window.preview.summary
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.subtext
                font.pixelSize: Theme.uiCaption
            }
        }
        Row {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            FlatButton {
                dpr: window.dpr
                id: openButton
                visible: window.preview.path.length > 0
                flat: true
                height: 32
                glyph: "" // OpenInNewWindow
                text: qsTr("打开")
                onClicked: window.preview.openFile()
                Component.onCompleted: window.frame.addControl(openButton)
            }
            FlatButton {
                dpr: window.dpr
                id: closeButton
                flat: true
                width: 32
                height: 32
                glyph: "" // ChromeClose
                onClicked: window.preview.closeByUser()
                Component.onCompleted: window.frame.addControl(closeButton)
            }
        }
        Hook { // the style's seam
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.edge(window.dpr)
            part: Theme.separator
            dpr: window.dpr
        }
    }

    // ---- the content -----------------------------------------------------------------

    Item {
        id: body
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true

        // A picture: fitted in, never larger than its own size (one pixel of
        // it on one of the screen's); the wheel zooms about the pointer, a
        // double click toggles fitted and its own size.
        Flickable {
            id: picture
            readonly property bool shown: window.preview.kind === FilePreview.Picture
            readonly property size natural: Qt.size(window.preview.imageSize.width / window.dpr,
                                                    window.preview.imageSize.height / window.dpr)
            readonly property real fitScale: natural.width > 0 && natural.height > 0
                ? Math.min(1, (width - 2 * window.padding) / natural.width, (height - 2 * window.padding) / natural.height) : 1
            property real zoom: 0 // 0: fitted
            readonly property real scale: zoom > 0 ? zoom : fitScale
            readonly property bool atNative: Math.abs(scale - 1) < 0.0001

            function zoomAt(factor, px, py) { // px, py: in the view
                const old = scale
                const next = Math.max(Math.min(fitScale, 0.05), Math.min(16, old * factor))
                if (Math.abs(next - old) < 0.0001)
                    return
                const cx = (contentX + px - image.x) / old
                const cy = (contentY + py - image.y) / old
                zoom = next
                returnToBounds()
                contentX = Math.max(0, Math.min(contentWidth - width, image.x + cx * next - px))
                contentY = Math.max(0, Math.min(contentHeight - height, image.y + cy * next - py))
            }

            visible: shown
            anchors.fill: parent
            contentWidth: Math.max(width, natural.width * scale + 2 * window.padding)
            contentHeight: Math.max(height, natural.height * scale + 2 * window.padding)
            boundsBehavior: Flickable.StopAtBounds
            interactive: zoom > 0 && (contentWidth > width || contentHeight > height)

            Connections {
                target: window.preview
                function onContentChanged() { picture.zoom = 0; picture.contentX = 0; picture.contentY = 0 }
            }

            Image {
                id: image
                // On whole device pixels: at its own size, each of its pixels on one of the screen's.
                x: window.onPixel(Math.max(window.padding, (picture.contentWidth - width) / 2))
                y: window.onPixel(Math.max(window.padding, (picture.contentHeight - height) / 2))
                width: window.onPixel(picture.natural.width * picture.scale)
                height: window.onPixel(picture.natural.height * picture.scale)
                source: picture.shown ? window.preview.imageUrl : ""
                cache: false
                fillMode: Image.Stretch
                smooth: !picture.atNative
                mipmap: !picture.atNative && picture.scale < 1
            }

            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: (event) => {
                    const steps = event.angleDelta.y / 120
                    if (steps !== 0)
                        picture.zoomAt(Math.pow(1.2, steps), event.x, event.y)
                }
            }
            TapHandler {
                onDoubleTapped: (eventPoint) => {
                    if (picture.zoom > 0)
                        picture.zoom = 0
                    else
                        picture.zoomAt(1 / picture.scale, eventPoint.position.x, eventPoint.position.y)
                }
            }
        }

        // The start of a text file, a line a row (long ones in pieces): numbered, monospaced, wrapped.
// Picked with the mouse (the area over it; it does not flick), across rows;
// Ctrl+C in the launcher copies it (FilePreview.selectedText).
        ListView {
            id: lines
            objectName: "previewTextLines"
            readonly property bool shown: window.preview.kind === FilePreview.Text
            readonly property int digits: Math.max(2, String(window.preview.lineCount).length)
            readonly property real numberWidth: digitMetrics.advanceWidth * digits + 12

            // What is picked: from where the press was (the anchor) to where
            // the mouse is (the focus), each a row and a position in it.
            property int anchorRow: 0
            property int anchorPos: 0
            property int focusRow: 0
            property int focusPos: 0
            readonly property bool backwards: focusRow < anchorRow || (focusRow === anchorRow && focusPos < anchorPos)
            readonly property int fromRow: backwards ? focusRow : anchorRow
            readonly property int fromPos: backwards ? focusPos : anchorPos
            readonly property int toRow: backwards ? anchorRow : focusRow
            readonly property int toPos: backwards ? anchorPos : focusPos

            function pick(aRow: int, aPos: int, fRow: int, fPos: int) {
                anchorRow = aRow
                anchorPos = aPos
                focusRow = fRow
                focusPos = fPos
                window.preview.setSelection(fromRow, fromPos, toRow, toPos)
            }
            // The row and position under (x, y) of this view; above or below it, the start or end of the row at its edge.
            function pointAt(x: real, y: real): var {
                if (count === 0)
                    return null
                const row = indexAt(1, contentY + Math.max(0, Math.min(height - 1, y)))
                if (row < 0) // under the last row
                    return {row: count - 1, pos: itemAtIndex(count - 1)?.length ?? 0}
                const item = itemAtIndex(row)
                if (!item)
                    return null
                return {row: row, pos: y < 0 ? 0 : y >= height ? item.length : item.positionAt(x, y)}
            }

            visible: shown
            anchors.fill: parent
            anchors.topMargin: 8
            anchors.bottomMargin: 8
            clip: true
            model: shown ? window.preview.lines : null
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true

            TextMetrics {
                id: digitMetrics
                font: lineFont.font
                text: "0"
            }
            Text { id: lineFont; visible: false; font.family: "Consolas"; font.pixelSize: Theme.uiCaption + 1 }

            Connections {
                target: window.preview
                function onContentChanged() { lines.pick(0, 0, 0, 0) }
            }

            delegate: Item {
                id: line
                required property string display
                required property int index
                readonly property int number: window.preview.lineNumberAt(index)
                readonly property int length: lineText.length
                readonly property bool inPick: index >= lines.fromRow && index <= lines.toRow
                readonly property int pickFrom: inPick && index === lines.fromRow ? lines.fromPos : 0
                readonly property int pickTo: !inPick ? 0 : index === lines.toRow ? lines.toPos : lineText.length
                width: lines.width
                height: lineText.height

                function positionAt(x: real, y: real): int {
                    const p = lines.mapToItem(lineText, x, y)
                    return lineText.positionAt(p.x, p.y)
                }
                function wordAt(position: int): var {
                    lineText.cursorPosition = position
                    lineText.selectWord()
                    return {from: lineText.selectionStart, to: lineText.selectionEnd}
                }
                function showPick() { lineText.select(pickFrom, pickTo) }
                onPickFromChanged: showPick()
                onPickToChanged: showPick()
                Component.onCompleted: showPick()
                ListView.onReused: showPick()

                Text {
                    width: lines.numberWidth
                    horizontalAlignment: Text.AlignRight
                    text: line.number > 0 ? line.number : ""
                    color: Theme.faint
                    font: lineFont.font
                }
                TextEdit {
                    id: lineText
                    x: lines.numberWidth + 12
                    width: lines.width - x - window.padding
                    text: line.display
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                    readOnly: true
                    activeFocusOnPress: false
                    persistentSelection: true
                    selectionColor: Theme.textSelection
                    selectedTextColor: Theme.text
                    color: Theme.text
                    font: lineFont.font
                }
            }
        }
        MouseArea {
            id: picker
            objectName: "previewTextPicker"
            property real lastX: 0
            property real lastY: 0
            visible: lines.shown
            anchors.fill: lines
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.IBeamCursor
            function extend() {
                const at = lines.pointAt(lastX, lastY)
                if (at)
                    lines.pick(lines.anchorRow, lines.anchorPos, at.row, at.pos)
            }
            onPressed: (mouse) => {
                if (mouse.source !== Qt.MouseEventNotSynthesized) { // touch flicks the text
                    mouse.accepted = false
                    return
                }
                const at = lines.pointAt(mouse.x, mouse.y)
                if (at)
                    lines.pick(at.row, at.pos, at.row, at.pos)
            }
            onPositionChanged: (mouse) => {
                if (!pressed)
                    return
                lastX = mouse.x
                lastY = mouse.y
                extend()
            }
            onDoubleClicked: (mouse) => {
                const at = lines.pointAt(mouse.x, mouse.y)
                const item = at ? lines.itemAtIndex(at.row) : null
                if (item) {
                    const word = item.wordAt(at.pos)
                    lines.pick(at.row, word.from, at.row, word.to)
                }
            }
            // Held beyond the top or bottom edge, the text runs on.
            Timer {
                interval: 30
                repeat: true
                running: picker.pressed && (picker.lastY < 0 || picker.lastY > picker.height)
                onTriggered: {
                    const step = Math.min(60, 4 + Math.abs(picker.lastY < 0 ? picker.lastY : picker.lastY - picker.height) / 4)
                    lines.contentY = Math.max(lines.originY, Math.min(lines.originY + lines.contentHeight - lines.height,
                                                                         lines.contentY + (picker.lastY < 0 ? -step : step)))
                    picker.extend()
                }
            }
        }
        ListScrollBar {
            visible: lines.shown
            flickable: lines
            anchors.right: parent.right
            anchors.top: lines.top
            anchors.bottom: lines.bottom
        }

        // A document through its preview handler: drawn by the handler in a
        // window of its own placed over this area (PreviewHost).
        Item {
            id: handlerArea
            readonly property bool shown: window.preview.kind === FilePreview.Handler
            anchors.fill: parent
            // A page on the window: the handler's window has corners of its
            // own (small round ones), which would notch the header's edge
            // if it filled the area.
            anchors.margins: 8
            visible: shown
            onShownChanged: if (shown) window.preview.setHandlerArea(handlerArea)
            onWidthChanged: if (shown) window.preview.setHandlerArea(handlerArea)
            onHeightChanged: if (shown) window.preview.setHandlerArea(handlerArea)
        }

        // A thumbnail (or the icon) and what is known about it: a folder, or
        // a file nothing else shows.
        Column {
            id: facts
            readonly property bool shown: window.preview.kind === FilePreview.Facts
            visible: shown
            anchors.centerIn: parent
            width: Math.min(parent.width - 2 * window.padding, 520)
            spacing: 16

            Image {
                id: thumbnail
                anchors.horizontalCenter: parent.horizontalCenter
                readonly property real side: Math.min(256, body.height - factsTable.height - 3 * window.padding)
                width: Math.max(0, Math.min(side, implicitWidth / window.dpr))
                height: implicitWidth > 0 ? width * implicitHeight / implicitWidth : 0
                visible: status === Image.Ready && implicitWidth > 0
                source: facts.shown ? window.preview.imageUrl : ""
                cache: false
                smooth: true
                mipmap: true
            }
            Grid {
                id: factsTable
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 2
                columnSpacing: 16
                rowSpacing: 6

                Repeater {
                    model: facts.shown ? window.preview.facts : []
                    delegate: Text {
                        required property var modelData
                        required property int index
                        text: modelData
                        textFormat: Text.PlainText
                        color: index % 2 === 0 ? Theme.subtext : Theme.text
                        font.pixelSize: Theme.uiBody
                        horizontalAlignment: index % 2 === 0 ? Text.AlignRight : Text.AlignLeft
                        width: index % 2 === 0 ? implicitWidth : Math.min(implicitWidth, facts.width - 120)
                        // A long value (a deep folder, the note on a downloaded
                        // file) goes on over a line or two, not cut off.
                        wrapMode: index % 2 === 0 ? Text.NoWrap : Text.WrapAtWordBoundaryOrAnywhere
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }
                }
            }
        }

        // Reading it, a moment after it began; or why there is nothing to show.
        Text {
            anchors.centerIn: parent
            width: parent.width - 2 * window.padding
            horizontalAlignment: Text.AlignHCenter
            visible: text.length > 0
            text: window.preview.kind === FilePreview.Loading ? (window.preview.slow ? qsTr("正在读取…") : "")
                : window.preview.kind === FilePreview.Message ? window.preview.message : ""
            wrapMode: Text.Wrap
            color: Theme.faint
            font.pixelSize: Theme.uiBody
        }
    }

    WindowEdge {
        id: windowEdge
        fill: Theme.background
    }
}
