import QtQuick

// The results tray: a shade deeper than the panel, with a one-pixel inner
// shadow along its top. Full width, square (it never touches a corner).
Rectangle {
    id: tray

    property real dpr: 1
    property QtObject look // injected by Look.qml
    readonly property real padding: look.trayPadding

    color: look.trayColor

    Rectangle { // the fade under the line
        y: tray.look.hair(tray.dpr)
        width: tray.width
        height: 3
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.rgba(tray.look.trayShade.r, tray.look.trayShade.g, tray.look.trayShade.b, tray.look.trayShade.a * 0.35) }
            GradientStop { position: 1; color: "transparent" }
        }
    }
    Rectangle {
        width: tray.width
        height: tray.look.hair(tray.dpr)
        color: tray.look.trayShade
    }
}
