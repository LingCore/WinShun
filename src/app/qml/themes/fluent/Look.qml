import QtQuick
import WinShun

// 精修原生 (fluent): Windows 11's own language, done properly. Every token a
// style can set is declared here; brand and tactile start from this Look
// (import "../fluent" as Fluent; Fluent.Look { ... }) and override what
// differs. The README beside the style folders lists them all.
QtObject {
    id: root

    // ---- the style itself
    readonly property string name: "fluent"
    readonly property bool dark: SystemTheme.dark
    // Every radius goes through r(): the 圆角/直角 setting squares them all.
    function r(v: real): real { return SystemTheme.roundedCorners ? v : 0 }
    function pill(size: real): real { return SystemTheme.roundedCorners ? size / 2 : 0 }
    // Mica behind the window (the 透明效果 setting): the layers are translucent
    // then. A style that rounds its windows itself cannot have Mica (DWM's
    // backdrop is square without DWM's corners): false there, and every
    // surface opaque.
    property bool backdrop: SystemTheme.backdrop
    // Whether the style can stand on Mica at all (the 透明效果 setting has
    // nothing to do in a style without it: settings greys it out).
    readonly property bool backdropCapable: true

    // ---- window frame
    // "dwm": Windows rounds, edges and shadows the window (8 on Windows 11).
    // "own": the window is see-through, windowFrame draws its body and edge
    // with windowRadius, and WinShun puts windowShadow under it.
    property string frame: "dwm"
    // Windows draws the edge of a "dwm" frame (Windows 11); on Windows 10 the
    // window draws it (windowFrame, "edge").
    readonly property bool dwmEdge: SystemTheme.nativeWindowBorder
    property real windowRadius: SystemTheme.roundedCorners && SystemTheme.nativeWindowBorder ? 8 : 0
    // Layers of the shadow under an "own" frame, nearest first, in logical
    // px: {alpha, blur, dy}. Unused with "dwm".
    property var windowShadow: dark ? [{ alpha: 0.40, blur: 1, dy: 0 }, { alpha: 0.30, blur: 8, dy: 2 }]
                                    : [{ alpha: 0.10, blur: 1, dy: 0 }, { alpha: 0.10, blur: 8, dy: 2 }]

    // ---- radii
    property real cardRadius: r(6)
    property real largeCardRadius: r(8)
    property real insetCardRadius: r(5)
    property real controlRadius: r(4)
    property real largeControlRadius: r(6)
    property real smallControlRadius: r(3)
    property real smallRadius: r(1.5)
    property real lineRadius: r(1)
    property real progressRadius: r(2)
    property real cardFocusRadius: r(11)
    property real menuRadius: r(8)
    property real menuItemRadius: r(4)
    property real rowRadius: r(6) // a row's highlight (results, clipboard, panel)

    // ---- accent
    // For text, glyphs and lines: lightened on dark to read there.
    property color accent: dark ? Qt.lighter(SystemTheme.accent, 1.55) : SystemTheme.accent
    // For fills under white words or marks: deep enough for white to read.
    property color accentFill: SystemTheme.accentFill
    // Lines and markers (the scope indicator, the selection's pill, the caret).
    property color accentLine: dark ? accent : accentFill
    // The selected row's tint.
    property color accentTint: Qt.rgba(accentLine.r, accentLine.g, accentLine.b, dark ? 0.12 : 0.095)
    // Words and marks on accentFill and dangerFill: white in both themes.
    property color accentText: "#FFFFFF"

    // ---- matches (StyledText: colour and weight only)
    property color matchColor: accent
    property int matchWeight: Font.Bold

    // ---- surfaces
    property color background: dark ? "#202020" : "#F9F9F9"
    // The chrome over the window's base (search row and type strip, footer):
    // a lighter layer, the results showing the base through.
    property color layer: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.052) : Qt.rgba(1, 1, 1, 0.78))
                                   : dark ? "#272727" : "#FFFFFF"
    property color layerEdge: backdrop ? (dark ? Qt.rgba(0, 0, 0, 0.42) : Qt.rgba(0, 0, 0, 0.08))
                                       : dark ? "#1A1A1A" : "#EBEBEB"
    property color windowEdge: dark ? "#6A6A6A" : "#B4B4B4"
    property color divider: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.084) : Qt.rgba(0, 0, 0, 0.058))
                                     : dark ? "#2E2E2E" : "#EAEAEA"
    property color text: dark ? "#F3F3F3" : "#1B1B1B"
    property color subtext: dark ? "#B8B8B8" : "#666666"
    property color faint: "#8A8A8A"
    property color hover: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.035))
                                   : dark ? "#292929" : "#F1F1F1"
    property color pressed: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.09) : Qt.rgba(0, 0, 0, 0.07))
                                     : dark ? "#323232" : "#E8E8E8"
    property color selection: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06))
                                       : dark ? "#2F2F2F" : "#EAEAEA"
    property color textSelection: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    property color badge: Qt.rgba(accent.r, accent.g, accent.b, dark ? 0.16 : 0.10)
    property color tagFill: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.05)

    property color track: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(0, 0, 0, 0.05))
                                   : dark ? "#2A2A2A" : "#EDEDED"
    property color chipActive: dark ? "#3A3A3A" : "#FFFFFF"
    property color chipHover: dark ? "#333333" : "#E4E4E4"
    property color chipBorder: dark ? "#454545" : "#DCDCDC"
    // 不限 selected: the resting choice, neutral; only a real filter gets the accent.
    property color chipRest: dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.075)
    property color pieHover: dark ? "#404040" : "#E3E3E3"
    property color pieShadow: dark ? Qt.rgba(0, 0, 0, 0.36) : Qt.rgba(0, 0, 0, 0.12)
    property color keycap: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.06)
    // A key hint's outline (keyHint: a thin edge, no fill).
    property color keyEdge: dark ? Qt.rgba(1, 1, 1, 0.17) : Qt.rgba(0, 0, 0, 0.14)
    property color checker: dark ? "#D0D0D0" : "#FFFFFF"
    property color checkerAlt: dark ? "#A0A0A0" : "#D6D6D6"

    // settings window
    property color page: dark ? "#202020" : "#F3F3F3"
    property color cardFill: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(1, 1, 1, 0.7))
                                  : dark ? "#2B2B2B" : "#FBFBFB"
    property color cardBorder: backdrop ? (dark ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(0, 0, 0, 0.06))
                                        : dark ? "#1D1D1D" : "#E5E5E5"
    property color navHover: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.035))
                                      : dark ? "#292929" : "#EDEDED"
    property color navSelected: backdrop ? (dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.055))
                                         : dark ? "#2D2D2D" : "#EAEAEA"
    property color control: dark ? "#373737" : "#FFFFFF"
    property color controlHover: dark ? "#3C3C3C" : "#F6F6F6"
    property color controlPressed: dark ? "#323232" : "#F0F0F0"
    property color controlBorder: dark ? "#444444" : "#D5D5D5"
    property color inputFocused: dark ? "#1F1F1F" : "#FFFFFF"

    // menus
    property color menuBackground: dark ? "#2C2C2C" : "#F9F9F9"
    property color menuHover: dark ? "#3A3A3A" : "#ECECEC"
    property color menuEdge: dark ? Qt.rgba(1, 1, 1, 0.09) : Qt.rgba(0, 0, 0, 0.10)
    // The menu's shadow (SoftShadow-like rings): colour at its edge and reach.
    property color menuShadow: dark ? Qt.rgba(0, 0, 0, 0.42) : Qt.rgba(0, 0, 0, 0.14)
    property int menuShadowSpread: 6

    // states
    property color danger: dark ? "#FF99A4" : "#C42B1C"
    property color dangerFill: "#C42B1C"
    property color success: dark ? "#6CCB5F" : "#0F7B0F"
    property color caution: dark ? "#FCE100" : "#9D5D00"
    property color closeHover: "#C42B1C"
    property color closePressed: Qt.rgba(0.769, 0.169, 0.110, 0.9)

    // The type strip's colours by hueIndex (-2 folder, 0.. the file types),
    // for the styles that colour their chips and file icons.
    property var typeHues: ["#4F9DFF", "#3FBF7F", "#A57CFF", "#FF8A5C", "#C9953A", "#7B8CFF", "#2FBFB4", "#E07BD0"]
    property color folderHue: "#E8B84A"
    function hueFor(index: int): color { return index === -2 ? folderHue : index < 0 ? faint : typeHues[index % typeHues.length] }

    // For parts that draw icons.
    readonly property string iconFont: SystemTheme.iconFont

    // ---- lines and pixels
    // edge(): a 1 px border or line on whole device pixels (2 at 150 %): window
    // edges, window-level dividers. hair(): one device pixel, for a style's
    // lines inside its own parts (key edges, seams), placed on whole device pixels.
    function edge(dpr: real): real { return Math.max(1, Math.round(dpr)) / dpr }
    function hair(dpr: real): real { return 1 / Math.max(1, dpr) }

    // ---- geometry the hosts lay out by (launcher, menus); fluent's own values
    // The search field in the launcher's search row: inset from the row's edges
    // (0: flat, the whole row is the field) and its radius.
    property real fieldInset: 0
    property real fieldRadius: controlRadius
    // Keys inside the search field (pin, expand, the scope keys): inset from the
    // field's top, bottom and right; whole device pixels.
    function fieldKeyInsetAt(dpr: real): real { return 0 }
    // Result rows: the highlight's inset from the list's sides, the icon's left
    // inset, where the name starts, the row buttons' inset from the right.
    property real rowInset: 6
    property real iconInset: 16
    property real textStart: 60
    property real actionsInset: 14
    // The highlight slides from row to row (ms; 0: it jumps).
    property int rowSlide: 0
    // The row buttons' capsule (actionCapsule); none in fluent.
    property real capsuleRadius: 0
    property real capsulePadding: 0
    property real capsuleKeyRadius: controlRadius
    property color capsuleFill: "transparent"
    // The type strip's chips (0: UiScale's chipHeight).
    property real typeChipHeight: 0
    // Menus: the item height, the padding round the items.
    property int menuItemHeight: 36
    property real menuPadding: 4
    // Settings' page tabs: the first label's x from the tabs' left (fluent's
    // have no track, so the words line up with the title; a style that draws
    // a track gives 0).
    property real pageTabsInset: -12
    // A check box's side; the dimming under a dialog over a window.
    property real checkSize: 18
    property color scrim: Qt.rgba(0, 0, 0, dark ? 0.5 : 0.3)
    // An edge round a menu item under the mouse where its fill alone hardly
    // shows (C++ tray menu); none in fluent.
    property color menuHoverEdge: "transparent"
    // Motion: colours and presses; things that move.
    property int animFast: 100
    property int animMove: 200

    // ---- hooks: the parts this style draws (see themes/README.md)
    property Component windowFrame: Component { WindowBody { look: root } }
    property Component searchField: Component { SearchField { look: root } }
    property Component inputField: Component { InputField { look: root } }
    property Component keySurface: Component { KeySurface { look: root } }
    property Component segmented: Component { Segmented { look: root } }
    property Component chipSurface: Component { ChipSurface { look: root } }
    property Component rowHighlight: Component { RowHighlight { look: root } }
    property Component typeIcon: null // fluent: the system's own shell icons
    property Component menuFrame: Component { MenuFrame { look: root } }
    property Component toggle: Component { Toggle { look: root } }
    property Component keyHint: Component { HintCap { look: root } }
    property Component separator: Component { Separator { look: root } }
    property Component scrollBar: Component { ScrollThumb { look: root } }
    property Component card: Component { CardSurface { look: root } }
    // The result list's tray (under the rows) and the row buttons' capsule:
    // nothing in fluent.
    property Component tray: null
    property Component actionCapsule: null
}
