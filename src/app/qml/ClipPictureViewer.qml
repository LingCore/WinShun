pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A clipboard picture large (GitHub #414): beside the clipboard window, in
// the room its screen has there (over the middle of the screen when there is
// too little), fitted and never larger than its own size until zoomed. The
// wheel zooms about the pointer, a double click goes to its own size and
// back, a drag moves a zoomed picture. It never takes the focus: the keys
// stay with the clipboard (Esc or Space closes it; ↑ ↓ show the next
// entry's picture here), as a click on the picture there does.
Window {
    id: viewer
    required property Window host
    property QtObject popupHost: null // Clipboard: prepareMenuWindow for DWM border, shadow and corners
    property bool open: host.visible
    property bool ready: false
    required property rect hostRect // the clipboard window, on the screen
    required property rect availableArea // its screen's, without the taskbar
    required property string source // empty: on its way (a file being read)
    required property size pixels // the picture's own size
    property bool animated: false
    property string caption // what it is: its type, size, bytes
    signal closeRequested()

    readonly property real dpr: Screen.devicePixelRatio
    readonly property int padding: 12
    readonly property int gap: 8
    readonly property size natural: Qt.size(Math.max(1, pixels.width) / dpr, Math.max(1, pixels.height) / dpr)
    // Beside the clipboard window on its roomier side, else over the screen.
    readonly property real rightRoom: availableArea.x + availableArea.width - hostRect.x - hostRect.width - 2 * gap
    readonly property real leftRoom: hostRect.x - availableArea.x - 2 * gap
    readonly property bool beside: Math.max(rightRoom, leftRoom) >= 480
    readonly property real roomWidth: beside ? Math.max(rightRoom, leftRoom) : availableArea.width - 2 * gap
    readonly property real roomHeight: availableArea.height - 2 * gap
    readonly property real fitted: Math.min(1, (roomWidth - 2 * padding) / natural.width,
                                            (roomHeight - 2 * padding - footer.height) / natural.height)

    objectName: "clipboardPictureViewer"
    transientParent: host
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.background
    visible: ready && open
    title: qsTr("查看图片") // Qt adds " - Win顺"
    Component.onCompleted: {
        if (popupHost)
            popupHost.prepareMenuWindow(this)
        ready = true
    }
    width: Math.ceil(Math.min(roomWidth, Math.max(420, natural.width * fitted + 2 * padding)))
    height: Math.ceil(Math.min(roomHeight, Math.max(260, natural.height * fitted + 2 * padding + footer.height)))
    x: Math.round(!beside ? availableArea.x + (availableArea.width - width) / 2
                  : rightRoom >= leftRoom ? hostRect.x + hostRect.width + gap : hostRect.x - gap - width)
    y: Math.round(Math.max(availableArea.y + gap, Math.min(hostRect.y + (hostRect.height - height) / 2,
                                                           availableArea.y + availableArea.height - gap - height)))

    function onPixel(v) { return Math.round(v * dpr) / dpr }

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

            Loader {
                id: loader
                anchors.fill: parent
                active: viewer.source.length > 0
                sourceComponent: viewer.animated ? moving : still
            }
            Component {
                id: still
                Image {
                    objectName: "clipboardPictureViewerImage"
                    source: viewer.source
                    fillMode: Image.Stretch
                    autoTransform: true
                    asynchronous: true
                    cache: false
                    smooth: !view.atNative
                    mipmap: !view.atNative && view.scale < 1
                }
            }
            Component {
                id: moving
                AnimatedImage {
                    source: viewer.source
                    fillMode: Image.Stretch
                    asynchronous: true
                    cache: false
                    playing: SystemTheme.animations
                    smooth: !view.atNative
                }
            }
            Text {
                readonly property int status: loader.item ? (loader.item as Image).status : Image.Loading
                anchors.centerIn: parent
                visible: status !== Image.Ready
                text: status === Image.Error ? qsTr("图片加载失败") : qsTr("正在加载图片…")
                color: status === Image.Error ? Theme.danger : Theme.faint
                font.pixelSize: Theme.uiCaption
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
        Connections {
            target: viewer
            function onSourceChanged() { view.zoom = 0; view.contentX = 0; view.contentY = 0 }
        }
    }

    Item { // what it is and how far it is zoomed; the keys; close
        id: footer
        anchors.bottom: parent.bottom
        width: parent.width
        height: 36

        Hook { // the style's seam
            width: parent.width
            height: Theme.edge(viewer.dpr)
            part: Theme.separator
            dpr: viewer.dpr
        }
        Text {
            id: about
            objectName: "clipboardPictureViewerCaption"
            x: viewer.padding
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, closeButton.x - x - 8 - (hint.visible ? hint.implicitWidth + 16 : 0))
            text: (viewer.caption.length > 0 ? viewer.caption + " · " : "") + Math.round(view.scale * 100) + "%"
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
        Text {
            id: hint
            anchors.right: closeButton.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            visible: about.x + about.implicitWidth + 16 + implicitWidth + 8 <= closeButton.x // the caption first
            text: qsTr("滚轮缩放 · 双击看原始大小 · Esc 关闭")
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
