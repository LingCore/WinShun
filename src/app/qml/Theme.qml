pragma Singleton
import QtQuick
import WinShun

// All colours and metrics of the UI. Follows the Windows light/dark setting
// and accent colour (see SystemTheme in C++).
QtObject {
    readonly property bool dark: SystemTheme.dark
    // Our windows can stand on Mica (see SystemTheme.backdrop): the layers on
    // them are translucent then, with the fill colours WinUI uses there; over
    // a plain background they keep their solid colours.
    readonly property bool backdrop: SystemTheme.backdrop
    readonly property color accent: dark ? Qt.lighter(SystemTheme.accent, 1.55) : SystemTheme.accent

    readonly property color background: dark ? "#202020" : "#F9F9F9"
    // The edge of our windows (WindowEdge.qml), rounded as DWM rounds them.
    // A light grey on the dark ones: over a dark program their shadow does
    // not show. The light ones stand apart by their shadow.
    readonly property int windowRadius: SystemTheme.roundedCorners ? 8 : 0
    readonly property color windowEdge: dark ? "#6A6A6A" : "#B4B4B4"
    readonly property color divider: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.084) : Qt.rgba(0, 0, 0, 0.058))
                                              : dark ? "#2E2E2E" : "#EAEAEA"
    readonly property color text: dark ? "#F3F3F3" : "#1B1B1B"
    readonly property color subtext: dark ? "#A7A7A7" : "#666666"
    readonly property color faint: dark ? "#727272" : "#9A9A9A"
    readonly property color hover: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.035))
                                            : dark ? "#292929" : "#F1F1F1"
    readonly property color selection: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06))
                                                : dark ? "#2F2F2F" : "#EAEAEA"
    readonly property color textSelection: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    readonly property color badge: Qt.rgba(accent.r, accent.g, accent.b, dark ? 0.16 : 0.10) // "应用" tag on app rows

    readonly property color track: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(0, 0, 0, 0.05))
                                            : dark ? "#2A2A2A" : "#EDEDED"
    readonly property color chipActive: dark ? "#3A3A3A" : "#FFFFFF"
    readonly property color chipHover: dark ? "#333333" : "#E4E4E4"
    readonly property color chipBorder: dark ? "#454545" : "#DCDCDC"
    readonly property color keycap: dark ? "#2A2A2A" : "#FFFFFF"
    readonly property color keycapBorder: dark ? "#3C3C3C" : "#DADADA"
    // The checkerboard under a translucent colour: light in both themes, as in
    // design tools, so that dark and light translucent colours both show.
    readonly property color checker: dark ? "#D0D0D0" : "#FFFFFF"
    readonly property color checkerAlt: dark ? "#A0A0A0" : "#D6D6D6"

    // Settings window (Windows 11 settings look)
    readonly property color page: dark ? "#202020" : "#F3F3F3"
    readonly property color card: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(1, 1, 1, 0.7))
                                           : dark ? "#2B2B2B" : "#FBFBFB"
    readonly property color cardBorder: backdrop ? (dark ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(0, 0, 0, 0.06))
                                                 : dark ? "#1D1D1D" : "#E5E5E5"
    readonly property color navHover: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.035))
                                               : dark ? "#292929" : "#EDEDED"
    readonly property color navSelected: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.055))
                                                  : dark ? "#2D2D2D" : "#EAEAEA"
    readonly property color control: dark ? "#373737" : "#FFFFFF"
    readonly property color controlHover: dark ? "#3C3C3C" : "#F6F6F6"
    readonly property color controlPressed: dark ? "#323232" : "#F0F0F0"
    readonly property color controlBorder: dark ? "#444444" : "#D5D5D5"
    readonly property color inputFocused: dark ? "#1F1F1F" : "#FFFFFF"
    // Not "onAccent": a name of on + capital letter reads as a signal handler,
    // and such a property was never re-evaluated when the theme changed.
    readonly property color accentText: dark ? "#000000" : "#FFFFFF"
    readonly property color menuBackground: dark ? "#2C2C2C" : "#F9F9F9"
    readonly property color menuHover: dark ? "#3A3A3A" : "#ECECEC"
    readonly property color danger: dark ? "#FF99A4" : "#C42B1C"
    readonly property color closeHover: "#C42B1C" // title bar close button, as in Windows 11
    readonly property color closePressed: Qt.rgba(0.769, 0.169, 0.110, 0.9)

    // Type scale in pixels. Even sizes land on whole device pixels at 150 %.
    readonly property int fontDisplay: 28 // settings window title
    readonly property int fontSearch: 24 // search box
    readonly property int fontTitle: 18 // result names, section titles, empty state
    readonly property int fontBody: 16 // most text
    readonly property int fontCaption: 14 // descriptions, key caps, hints

    readonly property string iconFont: SystemTheme.iconFont
}
