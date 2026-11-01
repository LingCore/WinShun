import QtQuick
import WinShun

// Windows 10 fallback. On Windows 11 the retained native frame provides both
// square and rounded outlines, including customized native corners.
Rectangle {
    anchors.fill: parent
    z: 1000
    color: "transparent"
    radius: Theme.windowRadius
    antialiasing: radius > 0
    border.width: 1
    border.color: Theme.windowEdge
    // Maximized, the window fills the screen and has no edge.
    visible: !SystemTheme.nativeWindowBorder && Window.window !== null && Window.window.visibility !== Window.Maximized
             && Window.window.visibility !== Window.FullScreen
}
