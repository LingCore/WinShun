pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A clipboard picture large (GitHub #414): beside the clipboard window, in
// the room its screen has there; else beside its list, over its preview (the
// list stays to be pointed at); else over the middle of the screen. Fitted
// and never larger than its own size until zoomed. The wheel zooms about the
// pointer, a double click goes to its own size and back, a drag moves a
// zoomed picture. It never takes the focus: the keys stay with the clipboard
// (Esc or Space closes it; ↑ ↓ show the next entry's picture here), as a
// click on the picture there does.
// The next picture is read while the one before stays on show, and takes its
// place once it is: no "loading" between them, and the window changes its
// size with the picture, in one go (WindowPlacer). It only grows while open,
// so that going from one to the next seldom moves it.
Window {
    id: viewer
    required property Window host
    property QtObject popupHost: null // Clipboard: prepareMenuWindow for DWM border, shadow and corners
    property bool open: host.visible
    property bool ready: false
    required property rect hostRect // the clipboard window, on the screen
    property real listWidth: 0 // its list's part, from its left: kept clear when there is no room beside it
    required property rect availableArea // its screen's, without the taskbar
    required property string source // empty: on its way (a file being read)
    required property size pixels // the picture's own size
    property bool animated: false
    property string caption // what it is: its type, size, bytes
    signal closeRequested()

    // What is on show (take): one of the two slots, the other reads the next.
    property string shownSource: ""
    property size shownPixels: Qt.size(0, 0)
    property string shownCaption: ""
    property int front: 0
    readonly property int frontStatus: front === 0 ? first.status : second.status
    // Nothing to show yet for a while (a large file being read): shown, saying so.
    property bool waited: false
    Timer {
        interval: 400
        running: viewer.open && viewer.shownSource.length === 0
        onTriggered: viewer.waited = true
    }

    readonly property real dpr: Screen.devicePixelRatio
    readonly property int padding: 12
    readonly property int gap: 8
    readonly property size natural: Qt.size(Math.max(1, shownPixels.width) / dpr, Math.max(1, shownPixels.height) / dpr)
    readonly property real rightRoom: availableArea.x + availableArea.width - hostRect.x - hostRect.width - 2 * gap
    readonly property real leftRoom: hostRect.x - availableArea.x - 2 * gap
    readonly property real pastListRoom: availableArea.x + availableArea.width - hostRect.x - listWidth - 2 * gap
    readonly property string side: Math.max(rightRoom, leftRoom) >= 480 ? (rightRoom >= leftRoom ? "right" : "left")
        : listWidth > 0 && pastListRoom >= 480 ? "pastList" : "middle"
    readonly property real roomWidth: side === "right" ? rightRoom : side === "left" ? leftRoom
        : side === "pastList" ? pastListRoom : availableArea.width - 2 * gap
    readonly property real roomHeight: availableArea.height - 2 * gap
    readonly property real fitted: Math.min(1, (roomWidth - 2 * padding) / natural.width,
                                            (roomHeight - 2 * padding - footer.height) / natural.height)
    readonly property real neededWidth: Math.max(420, natural.width * fitted + 2 * padding)
    readonly property real neededHeight: Math.max(260, natural.height * fitted + 2 * padding + footer.height)
    property real grownWidth: 0 // the most the pictures shown so far needed
    property real grownHeight: 0
    readonly property int wantedWidth: Math.ceil(Math.min(roomWidth, Math.max(grownWidth, neededWidth)))
    readonly property int wantedHeight: Math.ceil(Math.min(roomHeight, Math.max(grownHeight, neededHeight)))
    readonly property rect wanted: Qt.rect(
        Math.round(side === "right" ? hostRect.x + hostRect.width + gap
                   : side === "left" ? hostRect.x - gap - wantedWidth
                   : side === "pastList" ? hostRect.x + listWidth + gap
                   : availableArea.x + (availableArea.width - wantedWidth) / 2),
        Math.round(Math.max(availableArea.y + gap, Math.min(hostRect.y + (hostRect.height - wantedHeight) / 2,
                                                            availableArea.y + availableArea.height - gap - wantedHeight))),
        wantedWidth, wantedHeight)
    onWantedChanged: placer.place(wanted)

    objectName: "clipboardPictureViewer"
    transientParent: host
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.background
    visible: ready && open && (shownSource.length > 0 || waited)
    title: qsTr("查看图片") // Qt adds " - Win顺"
    Component.onCompleted: {
        if (popupHost)
            popupHost.prepareMenuWindow(this)
        placer.place(wanted)
        follow()
        ready = true
    }

    WindowPlacer {
        id: placer
        window: viewer
    }

    // The picture asked for, read in the slot not on show; the one on show
    // stays meanwhile. On its way (no source yet): the one on show stays.
    function follow() {
        const back = front === 0 ? second : first
        if (!open || source.length === 0)
            return
        if (source === shownSource) {
            back.url = "" // one asked for in between
            shownCaption = caption
            return
        }
        back.moving = animated
        back.url = source
        take(back) // read already, it may be
    }
    // Read (or not to be read): on show, with the window's size for it.
    function take(slot) {
        if (slot.inFront || slot.url.length === 0 || slot.url !== source
            || (slot.status !== Image.Ready && slot.status !== Image.Error))
            return
        const before = front === 0 ? first : second
        // Sized first, then shown: the first time, put there before it shows.
        shownPixels = pixels
        shownCaption = caption
        grownWidth = Math.max(grownWidth, neededWidth)
        grownHeight = Math.max(grownHeight, neededHeight)
        resetZoom()
        front = 1 - front
        shownSource = slot.url
        before.url = "" // its pixels go
    }
    // Closed: nothing kept, so that opened again it never shows the one before.
    function forget() {
        first.url = ""
        second.url = ""
        shownSource = ""
        shownPixels = Qt.size(0, 0)
        shownCaption = ""
        waited = false
        grownWidth = 0
        grownHeight = 0
        resetZoom()
    }
    onOpenChanged: open ? follow() : forget()
    onSourceChanged: Qt.callLater(follow) // with `animated` and `pixels`, which come with it
    onAnimatedChanged: Qt.callLater(follow)
    onCaptionChanged: if (source === shownSource) shownCaption = caption
    onPixelsChanged: if (source === shownSource) shownPixels = pixels // a file's, read after it

    function onPixel(v) { return Math.round(v * dpr) / dpr }
    function zoomIn() { view.zoomAt(1.25, view.width / 2, view.height / 2) }
    function zoomOut() { view.zoomAt(1 / 1.25, view.width / 2, view.height / 2) }
    function resetZoom() { view.zoom = 0; view.contentX = 0; view.contentY = 0 }

    // One picture: still or moving (a GIF).
    component Slot: Item {
        id: slot
        property string url: ""
        property bool moving: false
        property bool inFront: false
        property bool atNative: false
        readonly property int status: moving ? motion.status : still.status
        visible: inFront

        Image {
            id: still
            objectName: slot.inFront ? "clipboardPictureViewerImage" : ""
            anchors.fill: parent
            source: slot.moving ? "" : slot.url
            fillMode: Image.Stretch
            autoTransform: true
            asynchronous: true
            cache: false
            smooth: !slot.atNative
            // Always: a change would read it again (nothing on show meanwhile).
            // At its own size its own pixels are drawn all the same.
            mipmap: true
        }
        AnimatedImage {
            id: motion
            anchors.fill: parent
            source: slot.moving ? slot.url : ""
            fillMode: Image.Stretch
            asynchronous: true
            cache: false
            playing: slot.inFront && SystemTheme.animations
            smooth: !slot.atNative
        }
    }

    Flickable {
        id: view
        objectName: "clipboardPictureViewerView"
        readonly property real fitScale: Math.min(1, (width - 2 * viewer.padding) / viewer.natural.width,
                                                  (height - 2 * viewer.padding) / viewer.natural.height)
        property real zoom: 0 // 0: fitted
        readonly property real scale: zoom > 0 ? zoom : fitScale
        readonly property bool atNative: Math.abs(scale - 1) < 0.0001

        function zoomAt(factor, px, py) { // px, py: in the view
            const old = scale
            const next = Math.max(Math.min(fitScale, 0.05), Math.min(16, old * factor))
            if (Math.abs(next - old) < 0.0001)
                return
            const cx = (contentX + px - picture.x) / old
            const cy = (contentY + py - picture.y) / old
            zoom = next
            returnToBounds()
            contentX = Math.max(0, Math.min(contentWidth - width, picture.x + cx * next - px))
            contentY = Math.max(0, Math.min(contentHeight - height, picture.y + cy * next - py))
        }

        width: parent.width
        height: parent.height - footer.height
        clip: true
        contentWidth: Math.max(width, viewer.natural.width * scale + 2 * viewer.padding)
        contentHeight: Math.max(height, viewer.natural.height * scale + 2 * viewer.padding)
        boundsBehavior: Flickable.StopAtBounds
        interactive: zoom > 0 && (contentWidth > width || contentHeight > height)

        Item {
            id: picture
            // On whole device pixels: at its own size, each of its pixels on one of the screen's.
            x: viewer.onPixel(Math.max(viewer.padding, (view.contentWidth - width) / 2))
            y: viewer.onPixel(Math.max(viewer.padding, (view.contentHeight - height) / 2))
            width: viewer.onPixel(viewer.natural.width * view.scale)
            height: viewer.onPixel(viewer.natural.height * view.scale)

            Slot {
                id: first
                anchors.fill: parent
                inFront: viewer.front === 0
                atNative: view.atNative
                onStatusChanged: viewer.take(first)
            }
            Slot {
                id: second
                anchors.fill: parent
                inFront: viewer.front === 1
                atNative: view.atNative
                onStatusChanged: viewer.take(second)
            }
        }

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => {
                const steps = event.angleDelta.y / 120
                if (steps !== 0)
                    view.zoomAt(Math.pow(1.2, steps), event.x, event.y)
            }
        }
        TapHandler {
            onDoubleTapped: (eventPoint) => {
                if (view.zoom > 0)
                    view.zoom = 0
                else
                    view.zoomAt(1 / view.scale, eventPoint.position.x, eventPoint.position.y)
            }
        }
    }

    Text { // nothing on show yet (a large file being read), or not to be read
        objectName: "clipboardPictureViewerWaiting"
        anchors.centerIn: view
        visible: viewer.shownSource.length === 0 || viewer.frontStatus === Image.Error
        text: viewer.frontStatus === Image.Error ? qsTr("图片加载失败") : qsTr("正在加载图片…")
        color: viewer.frontStatus === Image.Error ? Theme.danger : Theme.faint
        font.pixelSize: Theme.uiCaption
    }

    Item { // what it is and how far it is zoomed; the keys; close
        id: footer
        anchors.bottom: parent.bottom
        width: parent.width
        height: 36

        // As many of the caption's parts as fit, whole (its type first, the
        // time last): never a number cut short ("1127 × 282…").
        function fitting(caption: string, room: real): string {
            let shorter = caption
            while (metrics.advanceWidth(shorter) > room) {
                const cut = shorter.lastIndexOf("·")
                if (cut < 0)
                    break
                shorter = shorter.slice(0, cut).replace(/\s+$/, "")
            }
            return shorter
        }
        // As the caption has it (a file's: wider).
        readonly property string separator: viewer.shownCaption.indexOf("  ·  ") >= 0 ? "  ·  " : " · "
        FontMetrics {
            id: metrics
            font: about.font
        }

        Hook { // the style's seam
            width: parent.width
            height: Theme.edge(viewer.dpr)
            part: Theme.separator
            dpr: viewer.dpr
        }
        Text {
            id: about
            objectName: "clipboardPictureViewerCaption"
            readonly property real room: closeButton.x - x - 8 - zoomText.implicitWidth
                                         - (hint.visible ? hint.implicitWidth + 16 : 0)
            x: viewer.padding
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, room)
            text: footer.fitting(viewer.shownCaption, room)
            textFormat: Text.PlainText
            elide: Text.ElideRight // its first part alone not fitting
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
        Text {
            id: zoomText
            objectName: "clipboardPictureViewerScale"
            anchors.left: about.right
            anchors.verticalCenter: parent.verticalCenter
            text: (viewer.shownCaption.length > 0 ? footer.separator : "") + Math.round(view.scale * 100) + "%"
            textFormat: Text.PlainText
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
        Text {
            id: hint
            anchors.right: closeButton.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            // The caption whole first.
            visible: about.x + metrics.advanceWidth(viewer.shownCaption) + zoomText.implicitWidth + 16 + implicitWidth + 8
                     <= closeButton.x
            text: qsTr("滚轮 / Ctrl+± 缩放 · 双击原尺寸 · Esc 关闭")
            color: Theme.faint
            font.pixelSize: Theme.uiCaption
        }
        FlatButton {
            id: closeButton
            dpr: viewer.dpr
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            flat: true
            width: 28
            height: 28
            glyph: String.fromCharCode(0xE8BB) // ChromeClose
            keyboardActivation: false // the clipboard keeps the keys
            Accessible.name: qsTr("关闭")
            onClicked: viewer.closeRequested()
        }
    }

    WindowEdge { fill: Theme.background }
}
