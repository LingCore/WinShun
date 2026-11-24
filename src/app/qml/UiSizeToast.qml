pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Compact, non-interactive confirmation pill shown briefly when stepping
// 界面大小 via Ctrl++ / Ctrl+- / Ctrl+0 in windows without a status footer
// (SettingsWindow, or Launcher in pure-search mode).
Item {
    id: toast

    property real dpr: 1 // passed by host window (GitHub #111: no per-item Screen attachment)
    property string message: ""
    property int level: 2
    property int steps: 4
    property bool shown: false

    function present(text: string, currentLevel: int, totalSteps: int): void {
        message = text
        level = Math.max(0, currentLevel)
        steps = totalSteps > 0 ? totalSteps : 4
        shown = true
        hideTimer.restart()
    }

    function dismiss(): void {
        hideTimer.stop()
        shown = false
    }

    implicitWidth: Theme.snapUp(content.implicitWidth + 28, dpr)
    implicitHeight: Theme.snapUp(Math.max(32, content.implicitHeight + 14), dpr)
    width: implicitWidth
    height: implicitHeight
    opacity: shown ? 1 : 0
    visible: opacity > 0

    Accessible.role: Accessible.StaticText
    Accessible.name: message
    Accessible.ignored: !shown

    Behavior on opacity {
        enabled: SystemTheme.animations && (toast.Window.window?.visible ?? false)
        NumberAnimation {
            duration: toast.shown ? 120 : 220
            easing.type: Easing.OutCubic
        }
    }

    Timer {
        id: hideTimer
        interval: 1600
        onTriggered: toast.shown = false
    }

    Hook {
        anchors.fill: parent
        part: Theme.menuFrame
        dpr: toast.dpr
    }

    Row {
        id: content
        x: Theme.snap((parent.width - width) / 2, toast.dpr)
        y: Theme.snap((parent.height - height) / 2, toast.dpr)
        spacing: 8

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "text-aa"
            size: 16
            color: Theme.accent
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: toast.message
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontCaption
            font.weight: Theme.fontStrong
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            visible: toast.steps > 1

            Repeater {
                model: toast.steps

                delegate: Rectangle {
                    required property int index
                    readonly property bool active: index === toast.level

                    width: active ? 12 : 4
                    height: 4
                    radius: Theme.pillRadius(height)
                    color: active ? Theme.accent : Qt.alpha(Theme.subtext, 0.35)
                }
            }
        }
    }
}
