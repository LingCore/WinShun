pragma ComponentBehavior: Bound
import QtQuick
import WinShun

Window {
    id: window
    required property Window host
    required property rect anchorRect
    required property rect availableArea
    required property var info
    required property UiScale metrics
    readonly property bool hovered: pointer.hovered
    readonly property real rightRoom: availableArea.x + availableArea.width - anchorRect.x - anchorRect.width - 16
    readonly property real leftRoom: anchorRect.x - availableArea.x - 16
    signal copyRequested(string text, bool remember)

    objectName: "clipboardHoverPreview"
    transientParent: host
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.background
    visible: host.visible
    width: Math.ceil(Math.min(Math.max(280, Math.min(480, measure.advanceWidth + 32)), availableArea.width - 16))
    height: Math.ceil(Math.min(Math.max(80, body.naturalHeight), 480, availableArea.height - 16))
    // Keep the list anchored. Prefer the right, then the left, and clamp on
    // narrow monitors. Vertical placement follows the hovered row.
    x: Math.round(Math.max(availableArea.x + 8, Math.min(
        rightRoom >= width || rightRoom >= leftRoom
            ? anchorRect.x + anchorRect.width + 8 : anchorRect.x - width - 8,
        availableArea.x + availableArea.width - width - 8)))
    y: Math.round(Math.max(availableArea.y + 8, Math.min(anchorRect.y,
        availableArea.y + availableArea.height - height - 8)))

    TextMetrics {
        id: measure
        text: (window.info.text ?? "").slice(0, 256)
        font.pixelSize: window.metrics.uiBody
    }
    ClipPreview {
        id: body
        anchors.fill: parent
        anchors.margins: 1
        metrics: window.metrics
        info: window.info
        compact: true
        parentTop: 1
        onCopyRequested: (text, remember) => window.copyRequested(text, remember)
    }
    HoverHandler { id: pointer }
    WindowEdge { fill: Theme.background }
}
