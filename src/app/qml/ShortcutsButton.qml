import QtQuick
import WinShun

// The info button at the right of a footer: a click opens the shortcuts the
// footer has no room for (ShortcutTip), another closes them; so does a click
// anywhere else in the window. `host` is what the tip's window is placed and
// styled by (the launcher, the clipboard); `frame` the window's, which drags
// by the footer around it. `where` says whose shortcuts they are
// (Shortcuts.entries), `context` what they depend on.
Rectangle {
    id: info

    required property QtObject host
    required property WindowFrame frame
    property string where
    property var context: ({})

    readonly property bool open: tipLoader.item !== null && (tipLoader.item as ShortcutTip).visible

    width: 32
    height: 32
    radius: Theme.controlRadius
    color: "transparent"
    readonly property real dpr: Screen.devicePixelRatio
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("快捷键")
    Component.onCompleted: info.frame.addControl(info) // a button, not part of the drag area

    function close() {
        if (tipLoader.item)
            (tipLoader.item as ShortcutTip).visible = false
    }

    onVisibleChanged: if (!visible) close()

    Hook { // the style's key (Theme.keySurface): flat until hovered, held while the tip is open
        id: surface
        anchors.fill: parent
        part: Theme.keySurface
        hovered: infoArea.containsMouse
        pressed: infoArea.pressed
        current: info.open
        flat: true
        role: "footer"
        dpr: info.dpr
    }

    Glyph {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: surface.out("contentOffsetY", 0)
        inkScale: surface.out("contentScale", 1)
        glyph: "" // Info
        size: 16
        color: surface.out("foreground", infoArea.containsMouse || info.open ? Theme.text : Theme.subtext)
    }

    MouseArea {
        id: infoArea
        anchors.fill: parent
        hoverEnabled: true
        onClicked: {
            if (info.open) {
                info.close()
                return
            }
            tipLoader.active = true
            const tip = tipLoader.item as ShortcutTip
            tip.rows = Shortcuts.entries(info.where, info.context, true)
            tip.popup(info)
        }
    }

    // While the tip is open, a press anywhere in the window closes it and
    // goes on to what it was on (the button itself toggles).
    MouseArea {
        id: outside
        parent: info.Window.contentItem
        anchors.fill: parent
        z: 1000
        enabled: info.open
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            if (!info.contains(info.mapFromItem(outside, mouse.x, mouse.y)))
                info.close()
            mouse.accepted = false
        }
    }

    Loader { // made when first wanted; gone with the window (see Main.qml's menu)
        id: tipLoader
        active: false
        sourceComponent: ShortcutTip {
            host: info.host
            onSettingsRequested: {
                info.close()
                info.host.openShortcutSettings()
            }
        }
    }

    Connections { // the window hidden: the tip goes too
        target: info.Window.window
        function onVisibleChanged() {
            if (!info.Window.window.visible)
                info.close()
        }
    }
}
