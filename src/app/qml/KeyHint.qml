import QtQuick
import WinShun

// "[Ctrl ↵] 打开位置" in the footer.
Row {
    id: hint

    property string keys
    property string label
    property UiScale metrics: Theme.metrics
    // Its width when shown, also while hidden (a hidden Row lays out nothing).
    readonly property real fullWidth: label.length > 0 ? keyText.implicitWidth + 10 + spacing + labelText.implicitWidth : 0

    spacing: 5

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    Rectangle {
        id: cap
        anchors.verticalCenter: parent.verticalCenter
        width: keyText.implicitWidth + 10
        height: hint.metrics.keycapHeight
        radius: Theme.controlRadius
        color: Theme.keycap

        Text {
            id: keyText
            x: hint.onPixel((cap.width - width) / 2)
            y: hint.onPixel((cap.height - height) / 2)
            text: hint.keys
            color: Theme.subtext
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
