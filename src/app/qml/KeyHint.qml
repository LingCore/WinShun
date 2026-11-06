import QtQuick
import WinShun

// "[Ctrl ↵] 打开位置" in the footer.
Row {
    id: hint

    property string keys
    property string label
    property UiScale metrics: Theme.metrics
    property real dpr: 0 // the window's, passed by a host that has it (GitHub #111)
    // Screen.* only when no host passed it: a default binding to it would make a
    // Screen object even where the host's value replaces it.
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio
    // Its width when shown, also while hidden (a hidden Row lays out nothing).
    readonly property real fullWidth: label.length > 0 ? keyText.implicitWidth + 10 + spacing + labelText.implicitWidth : 0

    spacing: 5

    function onPixel(v) { return Math.round(v * hint.pixelRatio) / hint.pixelRatio }

    Item {
        id: cap
        anchors.verticalCenter: parent.verticalCenter
        width: keyText.implicitWidth + 10
        height: hint.metrics.keycapHeight

        Hook { // the style's key cap (Theme.keyHint): it only tells, never pressed
            id: capPart
            anchors.fill: parent
            part: Theme.keyHint
            dpr: hint.pixelRatio
        }

        Text {
            id: keyText
            x: hint.onPixel((cap.width - width) / 2)
            y: hint.onPixel((cap.height - height) / 2 + capPart.out("contentOffsetY", 0))
            text: hint.keys
            color: capPart.out("foreground", Theme.subtext)
            font.pixelSize: hint.metrics.uiCaption
        }
    }

    Text {
        id: labelText
        anchors.verticalCenter: parent.verticalCenter
        text: hint.label
        color: Theme.faint
        font.pixelSize: hint.metrics.uiBody
    }
}
