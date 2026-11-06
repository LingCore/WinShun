pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Short menus cannot contain a wrapped path. A non-interactive tooltip
// window keeps the path readable without taking focus from the folder.
Window {
    id: tip
    required property Window host
    required property rect availableArea
    property Item target: null
    property string text
    property string shownText
    property bool shown: false

    objectName: "menuPathTip"
    transientParent: host
    flags: Qt.ToolTip | Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput
    color: "transparent"
    visible: shown && host.visible
    width: Math.ceil(Math.min(probe.implicitWidth + 20, 560, Math.max(40, availableArea.width - 16)))
    height: Math.ceil(Math.min(label.implicitHeight + 12, Math.max(24, availableArea.height - 16)))

    function sync() {
        shown = false
        delay.stop()
        if (target && text.length > 0 && host.visible)
            delay.start()
    }
    onTargetChanged: Qt.callLater(sync)
    onTextChanged: Qt.callLater(sync)
    Connections {
        target: tip.host
        function onVisibleChanged() { tip.sync() }
    }
    Timer {
        id: delay
        interval: 250
        onTriggered: {
            if (!tip.target || !tip.host.visible)
                return
            tip.shownText = tip.text
            const p = tip.target.mapToGlobal(0, 0)
            const area = tip.availableArea
            tip.x = Math.round(Math.max(area.x + 8, Math.min(p.x, area.x + area.width - tip.width - 8)))
            const above = p.y - tip.height - 6
            const y = above >= area.y + 8 ? above : p.y + tip.target.height + 6
            tip.y = Math.round(Math.max(area.y + 8, Math.min(y, area.y + area.height - tip.height - 8)))
            tip.shown = true
        }
    }
    Hook {
        anchors.fill: parent
        part: Theme.menuFrame
        dpr: tip.Screen.devicePixelRatio
    }
    Text {
        id: probe
        visible: false
        text: tip.shownText
        textFormat: Text.PlainText
        font.pixelSize: Theme.fontCaption
    }
    Text {
        id: label
        objectName: "menuPathLabel"
        anchors.fill: parent
        anchors.margins: 10
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        text: tip.shownText
        textFormat: Text.PlainText
        font.pixelSize: Theme.fontCaption
        color: Theme.text
        wrapMode: Text.Wrap
        elide: Text.ElideRight
    }
}
