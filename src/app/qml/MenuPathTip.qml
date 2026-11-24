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
    // Shown with 外观 › 悬停提示 off too (SystemTheme.hoverTips, GitHub #368,
    // #413) when the row's own name is cut off: then it is the name's words.
    property bool essential: false
    // Tip category (GitHub #440): defaults to "path" (folder menu full paths).
    property string kind: "path"
    readonly property bool allowed: essential || (SystemTheme.hoverTips
        && (kind === "action"    ? SystemTheme.hoverTipsActions
          : kind === "interface" ? SystemTheme.hoverTipsInterface
                                 : SystemTheme.hoverTipsPaths))

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
        if (allowed && target && text.length > 0 && host.visible)
            delay.start()
    }
    onTargetChanged: Qt.callLater(sync)
    onTextChanged: Qt.callLater(sync)
    onAllowedChanged: Qt.callLater(sync)
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
            // In front of the host's other windows shown since it was last
            // (the folder menu's columns): shown again, it keeps its place.
            tip.raise()
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
