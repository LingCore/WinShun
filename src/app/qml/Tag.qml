import QtQuick
import WinShun

// A small tag beside a name, in the accent colour: 随包, AI (PluginCard).
Rectangle {
    id: tag

    property string text

    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
    width: label.implicitWidth + 12
    height: 20
    radius: Theme.smallControlRadius
    color: Theme.badge

    property real dpr: Screen.devicePixelRatio // a host with the window's can pass it (GitHub #111)
    function onPixel(v) { return Math.round(v * dpr) / dpr }

    Text {
        id: label
        x: tag.onPixel((tag.width - width) / 2)
        y: tag.onPixel((tag.height - height) / 2)
        text: tag.text
        color: Theme.accent
        font.pixelSize: 12
    }
}
