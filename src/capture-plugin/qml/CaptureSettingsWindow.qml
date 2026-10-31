pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The screen capture's settings: opened by 设置 on its card in Win顺's 插件
// page (showSettings) or from its tray menu. A window of the plugin's own,
// drawn as Win顺's settings window: the title bar, one page that scrolls.
Window {
    id: window

    required property CaptureSettings editor
    required property WindowFrame frame

    width: 720
    height: 640
    minimumWidth: 560
    minimumHeight: 360
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.page
    title: qsTr("截图设置") // shown as "截图设置 - Win顺"
    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)

    function clearFocus() { focusSink.forceActiveFocus() }

    Item {
        id: focusSink
        focus: true
        Keys.onEscapePressed: if (!window.editor.recordingHotkey) window.close()
    }

    TitleBar {
        id: titleBar
        z: 1
        width: parent.width
        frame: window.frame
        title: window.title + " - " + Qt.application.displayName
    }

    Flickable {
        id: flick
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: page.implicitHeight + 48
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ListWheelScroll {
            view: flick
            animate: SystemTheme.animations
        }

        MouseArea { // clicking empty space finishes editing a field
            width: flick.width
            height: Math.max(flick.height, flick.contentHeight)
            onClicked: window.clearFocus()
        }

        Column {
            id: page
            x: 24
            y: 8
            width: flick.width - 48
            spacing: 20
            // On whole device pixels wherever it has scrolled to (as Win顺's settings).
            transform: ScrollPixelAlignment {
                view: flick
                viewY: flick.y
                itemY: page.y
                pixelBias: 0.02
            }

            Text {
                text: qsTr("截图")
                color: Theme.text
                font.pixelSize: Theme.fontDisplay
                font.weight: Theme.fontStrong
            }
            CaptureOptions {
                width: parent.width
                editor: window.editor
            }
        }
    }

    WindowEdge {}
}
