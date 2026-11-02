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
    // For text, glyphs and lines: lightened on dark to read there.
    readonly property color accent: dark ? Qt.lighter(SystemTheme.accent, 1.55) : SystemTheme.accent
    // For fills under white words or marks (accentText): deep enough for
    // white to read, whatever the accent (SystemTheme.accentFill).
    readonly property color accentFill: SystemTheme.accentFill
    // FreeType synthesizes a missing heavy face only from Bold (700).
    // Keep the UI hierarchy when shipping the complete regular font alone.
    readonly property int fontStrong: Font.Bold

    readonly property color background: dark ? "#202020" : "#F9F9F9"
    // The edge of our windows (WindowEdge.qml), rounded as DWM rounds them.
    // A light grey on the dark ones: over a dark program their shadow does
    // not show. The light ones stand apart by their shadow.
    // The window itself: DWM rounds it on Windows 11 only (win::styleFramelessWindow).
    readonly property int windowRadius: SystemTheme.roundedCorners && SystemTheme.nativeWindowBorder ? 8 : 0
    readonly property int cardRadius: SystemTheme.roundedCorners ? 6 : 0
    readonly property int largeCardRadius: SystemTheme.roundedCorners ? 8 : 0
    readonly property int insetCardRadius: SystemTheme.roundedCorners ? 5 : 0
    readonly property int controlRadius: SystemTheme.roundedCorners ? 4 : 0
    readonly property int largeControlRadius: SystemTheme.roundedCorners ? 6 : 0
    readonly property int smallControlRadius: SystemTheme.roundedCorners ? 3 : 0
    readonly property real smallRadius: SystemTheme.roundedCorners ? 1.5 : 0
    readonly property int lineRadius: SystemTheme.roundedCorners ? 1 : 0
    readonly property int progressRadius: SystemTheme.roundedCorners ? 2 : 0
    readonly property int cardFocusRadius: SystemTheme.roundedCorners ? 11 : 0
    // Chips and scrollbars keep their existing proportions in rounded mode.
    function pillRadius(size: real): real { return SystemTheme.roundedCorners ? size / 2 : 0 }
    // Edges on whole device pixels (docs/pitfalls.md, "边框 1 px 在 150% 下一边粗一边细").
    // dpr is the window's: Screen.devicePixelRatio from the item that asks, so
    // a window moved to another screen asks again.
    // edge(): a 1 px line or border, as many device pixels as Qt draws a border
    // of 1 px with (1.5 at 150 % becomes 2). A line of plain 1 is 1 or 2 pixels
    // by where it lies, and a child inset by 1 covers half of a border.
    function edge(dpr: real): real { return Math.max(1, Math.round(dpr)) / dpr }
    // snap(): the nearest device pixel; snapUp(): the next one up (a size that must not shrink).
    function snap(v: real, dpr: real): real { return Math.round(v * dpr) / dpr }
    function snapUp(v: real, dpr: real): real { return Math.ceil(v * dpr - 1e-6) / dpr }
    readonly property color windowEdge: dark ? "#6A6A6A" : "#B4B4B4"
    readonly property color divider: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.084) : Qt.rgba(0, 0, 0, 0.058))
                                              : dark ? "#2E2E2E" : "#EAEAEA"
    readonly property color text: dark ? "#F3F3F3" : "#1B1B1B"
    readonly property color subtext: dark ? "#B8B8B8" : "#666666"
    // Hints and placeholders: Windows 11's tertiary text, legible on a card (≥ 3:1).
    readonly property color faint: "#8A8A8A"
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
    // The drop pie (DropPieWindow): its middle with the pointer in it (let go
    // there and nothing happens), and a chip under the pointer that cannot be
    // taken; a step off the menu background.
    readonly property color pieHover: dark ? "#404040" : "#E3E3E3"
    // The soft shadow round its chips, at their edge (SoftShadow).
    readonly property color pieShadow: dark ? Qt.rgba(0, 0, 0, 0.36) : Qt.rgba(0, 0, 0, 0.12)
    // A key cap is a plain light-grey fill with no border (a border would be a
    // second edge to keep on whole pixels); translucent so it reads on a card,
    // on Mica and on the plain footer alike. Subtext on it stays above 4.5:1.
    readonly property color keycap: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.06)
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
    // Words and marks on accentFill and dangerFill: white in both themes,
    // never Windows 11's black on a light accent (see docs/colors.md).
    // Not "onAccent": a name of on + capital letter reads as a signal handler,
    // and such a property was never re-evaluated when the theme changed.
    readonly property color accentText: "#FFFFFF"
    readonly property color menuBackground: dark ? "#2C2C2C" : "#F9F9F9"
    readonly property color menuHover: dark ? "#3A3A3A" : "#ECECEC"
    readonly property int menuItemHeight: 36
    readonly property color danger: dark ? "#FF99A4" : "#C42B1C" // words and glyphs
    readonly property color dangerFill: "#C42B1C" // under white words (an armed delete button)
    readonly property color success: dark ? "#6CCB5F" : "#0F7B0F" // Windows 11's status colours
    readonly property color caution: dark ? "#FCE100" : "#9D5D00"
    readonly property color closeHover: "#C42B1C" // title bar close button, as in Windows 11
    readonly property color closePressed: Qt.rgba(0.769, 0.169, 0.110, 0.9)

    // Type scale in pixels. Even sizes land on whole device pixels at 150 %.
    readonly property int fontDisplay: 28 // settings window title
    readonly property int fontSearch: 24 // search box
    readonly property int fontTitle: 18 // result names, section titles, empty state
    readonly property int fontBody: 16 // most text
    readonly property int fontCaption: 14 // descriptions, key caps, hints

    // 界面大小 (Settings uiSize): the launcher, the clipboard and the bar by
    // file dialogs in smaller or larger type, on lower or taller rows; the
    // settings window keeps the scale above. Whole pixels in every step, even
    // where they can be (whole device pixels at 150 %); 标准 is the scale
    // above. Widths, icons (32: one of Windows' own icon sizes at every
    // scaling) and 1-pixel lines stay. C++ has the heights it places windows
    // by in UiSize.h.
    readonly property int step: SystemTheme.uiSize === "compact" ? 0 : SystemTheme.uiSize === "roomy" ? 2 : 1
    readonly property int uiSearch: [20, 24, 28][step]
    readonly property int uiTitle: [16, 18, 20][step]
    readonly property int uiBody: [14, 16, 18][step] // compact: Windows 11's own body text
    readonly property int uiCaption: [12, 14, 16][step]
    readonly property int uiTab: [14, 15, 16][step] // the clipboard's category chips
    readonly property int uiSmall: [12, 12, 14][step] // the Ctrl+G key on the dialog bar
    readonly property int headerHeight: SystemTheme.headerHeight // the search field's row: 56, 64, 72
    readonly property int rowHeight: [52, 60, 68][step] // a result or clipboard entry
    readonly property int footerHeight: [36, 42, 48][step]
    readonly property int tabHeight: [28, 32, 36][step] // the launcher's scope tabs
    readonly property int chipHeight: [24, 28, 32][step] // type chips, footer switches, the clipboard's categories (+2)
    readonly property int stripHeight: [36, 40, 44][step] // the type strip
    readonly property int tabsHeight: [40, 44, 48][step] // the clipboard's row of categories
    readonly property int keycapHeight: [20, 22, 24][step]
    readonly property int badgeHeight: [22, 24, 26][step] // "应用", a group's name
    readonly property int labelRowHeight: [24, 28, 32][step] // headings between the rows
    readonly property int hintRowHeight: [32, 36, 40][step]
    readonly property int fileRowHeight: [36, 40, 44][step] // the clipboard preview's list of files
    readonly property int panelRowHeight: [36, 42, 48][step] // the search panel's rows, one line each
    readonly property int searchIconSize: [20, 22, 24][step]

    readonly property string iconFont: SystemTheme.iconFont
}
