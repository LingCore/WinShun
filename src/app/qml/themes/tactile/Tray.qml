import QtQuick

// The results tray: a shade deeper than the panel, with a one-pixel inner
// shadow along its top. Full width, square (it never touches a corner).
Rectangle {
    id: tray

    property real dpr: 1
    property QtObject look // injected by Look.qml
    readonly property real padding: look ? look.trayPadding : 10
    readonly property real hair: look ? look.hair(dpr) : 1
    readonly property color shade: look ? look.trayShade : "transparent"

    color: look ? look.trayColor : "transparent"

    Rectangle { // the fade under the line
        y: tray.hair
        width: tray.width
        height: 3
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.rgba(tray.shade.r, tray.shade.g, tray.shade.b, tray.shade.a * 0.35) }
            GradientStop { position: 1; color: "transparent" }
        }
    }
    Rectangle {
        width: tray.width
        height: tray.hair
        color: tray.shade
    }
}
