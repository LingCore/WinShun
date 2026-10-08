pragma Singleton
import QtQuick
import WinShun

// The 拾穗计划 page's warm colours: a golden afternoon in light mode, a sunset
// in dark mode, neither on black (MacShun: WarmPalette).
QtObject {
    readonly property bool dark: Theme.dark

    // Sky, top to bottom. The dusk runs from brick red through orange to
    // amber: saturated, never greyish.
    readonly property color skyTop: dark ? Qt.rgba(0.58, 0.23, 0.17, 1) : Qt.rgba(1.00, 0.96, 0.87, 1)
    readonly property color skyMiddle: dark ? Qt.rgba(0.80, 0.36, 0.19, 1) : Qt.rgba(1.00, 0.86, 0.63, 1)
    readonly property color skyBottom: dark ? Qt.rgba(0.96, 0.58, 0.27, 1) : Qt.rgba(0.98, 0.70, 0.43, 1)

    readonly property color ink: dark ? Qt.rgba(1.00, 0.96, 0.90, 1) : Qt.rgba(0.36, 0.19, 0.06, 1)
    readonly property color inkSoft: dark ? Qt.rgba(1.00, 0.86, 0.72, 1) : Qt.rgba(0.55, 0.34, 0.17, 1)
    readonly property color chip: dark ? Qt.rgba(1, 1, 1, 0.2) : Qt.rgba(1, 1, 1, 0.55)
    readonly property color iconFill: dark ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(1, 1, 1, 0.6) // the works' icon tiles
    readonly property color iconStroke: dark ? Qt.rgba(1, 1, 1, 0.35) : Qt.rgba(1, 1, 1, 0.9)
    readonly property color accent: dark ? Qt.rgba(0.95, 0.38, 0.22, 1) : Qt.rgba(0.93, 0.42, 0.22, 1)
    readonly property color accentLight: Qt.lighter(accent, 1.15) // top of the accent gradients

    readonly property color wheatBack: dark ? Qt.rgba(1.00, 0.78, 0.46, 0.5) : Qt.rgba(0.92, 0.66, 0.32, 0.6)
    readonly property color wheatFront: dark ? Qt.rgba(1.00, 0.86, 0.58, 0.85) : Qt.rgba(0.80, 0.50, 0.18, 0.85)
    readonly property color mote: dark ? Qt.rgba(1.00, 0.95, 0.80, 1) : "white"
    readonly property color bird: Qt.rgba(ink.r, ink.g, ink.b, 0.4)
}
