pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// What a FilePreview shows, without a frame: a picture fitted in (the wheel
// zooms, a double click goes to its own size and back), the start of a text
// file, a document through the handler Explorer's preview pane uses (a
// window of its own over the content area, see PreviewHost), or a large
// thumbnail and what is known about the file. In the preview's own window
// (PreviewWindow.qml) and in the search panel's 预览 tab (ContentPreviewView,
// GitHub #224), the same.
Item {
    id: body

    required property FilePreview preview
    required property real dpr
    property int padding: 16

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    clip: true

    // A picture: fitted in, never larger than its own size (one pixel of
    // it on one of the screen's); the wheel zooms about the pointer, a
    // double click toggles fitted and its own size.
    Flickable {
        id: picture
        readonly property bool shown: body.preview.kind === FilePreview.Picture
        readonly property size natural: Qt.size(body.preview.imageSize.width / body.dpr,
                                                body.preview.imageSize.height / body.dpr)
        readonly property real fitScale: natural.width > 0 && natural.height > 0
            ? Math.min(1, (width - 2 * body.padding) / natural.width, (height - 2 * body.padding) / natural.height) : 1
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
        contentWidth: Math.max(width, natural.width * scale + 2 * body.padding)
        contentHeight: Math.max(height, natural.height * scale + 2 * body.padding)
        boundsBehavior: Flickable.StopAtBounds
        interactive: zoom > 0 && (contentWidth > width || contentHeight > height)

        Connections {
            target: body.preview
            function onContentChanged() { picture.zoom = 0; picture.contentX = 0; picture.contentY = 0 }
        }

        Image {
            id: image
            // On whole device pixels: at its own size, each of its pixels on one of the screen's.
            x: body.onPixel(Math.max(body.padding, (picture.contentWidth - width) / 2))
            y: body.onPixel(Math.max(body.padding, (picture.contentHeight - height) / 2))
            width: body.onPixel(picture.natural.width * picture.scale)
            height: body.onPixel(picture.natural.height * picture.scale)
            source: picture.shown ? body.preview.imageUrl : ""
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
        readonly property bool shown: body.preview.kind === FilePreview.Text
        readonly property int digits: Math.max(2, String(body.preview.lineCount).length)
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
            body.preview.setSelection(fromRow, fromPos, toRow, toPos)
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
        model: shown ? body.preview.lines : null
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true

        TextMetrics {
            id: digitMetrics
            font: lineFont.font
            text: "0"
        }
        Text { id: lineFont; visible: false; font.family: "Consolas"; font.pixelSize: Theme.uiCaption + 1 }

        Connections {
            target: body.preview
            function onContentChanged() { lines.pick(0, 0, 0, 0) }
        }

        delegate: Item {
            id: line
            required property string display
            required property int index
            readonly property int number: body.preview.lineNumberAt(index)
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
                width: lines.width - x - body.padding
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
        dpr: body.dpr
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
        readonly property bool shown: body.preview.kind === FilePreview.Handler
        anchors.fill: parent
        // A page on the window: the handler's window has corners of its
        // own (small round ones), which would notch the header's edge
        // if it filled the area.
        anchors.margins: 8
        visible: shown
        onShownChanged: if (shown) body.preview.setHandlerArea(handlerArea)
        onWidthChanged: if (shown) body.preview.setHandlerArea(handlerArea)
        onHeightChanged: if (shown) body.preview.setHandlerArea(handlerArea)
    }

    // A thumbnail (or the icon) and what is known about it: a folder, or
    // a file nothing else shows.
    Column {
        id: facts
        readonly property bool shown: body.preview.kind === FilePreview.Facts
        visible: shown
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * body.padding, 520)
        spacing: 16

        Image {
            id: thumbnail
            anchors.horizontalCenter: parent.horizontalCenter
            readonly property real side: Math.min(256, body.height - factsTable.height - 3 * body.padding)
            width: Math.max(0, Math.min(side, implicitWidth / body.dpr))
            height: implicitWidth > 0 ? width * implicitHeight / implicitWidth : 0
            visible: status === Image.Ready && implicitWidth > 0
            source: facts.shown ? body.preview.imageUrl : ""
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
                model: facts.shown ? body.preview.facts : []
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
        width: parent.width - 2 * body.padding
        horizontalAlignment: Text.AlignHCenter
        visible: text.length > 0
        text: body.preview.kind === FilePreview.Loading ? (body.preview.slow ? qsTr("正在读取…") : "")
            : body.preview.kind === FilePreview.Message ? body.preview.message : ""
        wrapMode: Text.Wrap
        color: Theme.faint
        font.pixelSize: Theme.uiBody
    }
}
