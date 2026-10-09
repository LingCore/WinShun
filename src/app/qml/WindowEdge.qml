import QtQuick
import WinShun

// The edge of one of our windows: a thin line along its outline, drawn over
// everything in it. Drawn here, not by DWM, whose line is stepped along the
// rounded corners (see win::styleFramelessWindow). The window's own corners
// are rounded by DWM, as much as this one (Theme.windowRadius).
Rectangle {
    anchors.fill: parent
    z: 1000
    color: "transparent"
    radius: Theme.windowRadius
    antialiasing: true
    border.width: 1
    border.color: Theme.windowEdge
    // Maximized, the window fills the screen and has no edge.
    visible: Window.window !== null && Window.window.visibility !== Window.Maximized
             && Window.window.visibility !== Window.FullScreen
}
