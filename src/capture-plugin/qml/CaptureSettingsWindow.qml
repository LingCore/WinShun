pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The screen capture's settings: opened by 设置 on its card in Win顺's 插件
// page (showSettings) or from its tray menu. A window of the plugin's own,
// drawn as Win顺's settings window: the title bar, the page's name and
// tabs, and under them the tab's cards of options, which scroll.
Window {
    id: window

    required property CaptureSettings editor
    required property WindowFrame frame

    width: 720
    height: 640
    // Never narrower than the tabs (their labels are longer in English).
    minimumWidth: Math.max(560, Math.ceil(groupTabs.implicitWidth) + 48)
    minimumHeight: 360
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.page
    title: qsTr("截图设置") // shown as "截图设置 - Win顺"
    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)

    // One Screen attached object for the whole window (GitHub #111; docs/pitfalls.md).
    readonly property real dpr: Screen.devicePixelRatio

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

    // The page's name and its tabs, staying put while the options scroll (as
    // in Win顺's settings window, SettingsWindow.qml's pageHeader).
    Item {
        id: pageHeader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        height: Theme.snap(pageHeaderColumn.implicitHeight + 16, window.dpr)

        Column {
            id: pageHeaderColumn
            x: 24
            y: 8
            width: parent.width - 48
            spacing: 6

            Text {
                text: qsTr("截图")
                color: Theme.text
                font.pixelSize: Theme.fontDisplay
                font.weight: Theme.fontStrong
            }
            PageTabs {
                id: groupTabs
                objectName: "captureGroupTabs"
                labels: options.groups.map(group => group.title)
                current: options.currentGroup
                Accessible.name: qsTr("截图设置分类")
                onActivated: (index) => {
                    window.clearFocus()
                    options.currentGroup = index
                    flick.contentY = 0
                }
            }
        }

        Rectangle { // once the options scroll under it
            x: 24
            width: parent.width - 48
            anchors.bottom: parent.bottom
            height: Theme.edge(window.dpr)
            color: Theme.divider
            opacity: flick.contentY > 2 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
    }

    Flickable {
        id: flick
        objectName: "captureSettingsFlick"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: pageHeader.bottom
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: options.implicitHeight + 36
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

        CaptureOptions {
            id: options
            x: 24
            y: 12
            width: flick.width - 48
            editor: window.editor
            dpr: window.dpr
            // On whole device pixels wherever it has scrolled to (as Win顺's settings).
            transform: ScrollPixelAlignment {
                view: flick
                viewY: flick.y
                itemY: options.y
                pixelBias: 0.02
            }
        }
    }

    WindowEdge {}
}
