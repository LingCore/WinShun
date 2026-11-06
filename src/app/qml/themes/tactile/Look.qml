pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// 轻拟物质感 (tactile): one warm-white (graphite on dark) panel of material.
// The search box is a groove cut into it, everything that can be clicked is a
// key that goes down, the selected row is a card lifted out of a shallow tray,
// and everything that cannot be clicked stays flat. Design source:
// design/search-concepts/04-tactile.html (the CSS values, not its notes).
QtObject {
    id: root

    readonly property string name: "tactile"
    readonly property bool dark: SystemTheme.dark

    // ---- helpers ------------------------------------------------------------
    // Warm shadow ink (70, 48, 25) and black at an alpha: shading on the panel.
    function warm(alpha: real): color { return Qt.rgba(70 / 255, 48 / 255, 25 / 255, alpha) }
    function black(alpha: real): color { return Qt.rgba(0, 0, 0, alpha) }
    function white(alpha: real): color { return Qt.rgba(1, 1, 1, alpha) }
    // The fine detail of a key (rim, highlight, seam, card line): one device
    // pixel at every scale, as the prototype draws it (1 / dpr CSS px).
    function hair(dpr: real): real { return 1 / Math.max(1, dpr) }
    // The window's edge: as many device pixels as Theme.edge (1 at 100 %, 2 at 150 %).
    function edge(dpr: real): real { return Math.max(1, Math.round(dpr)) / dpr }
    function snap(v: real, dpr: real): real { return Math.round(v * dpr) / dpr }
    // The user's 圆角/直角 setting: every corner goes square (0), the window too.
    function r(v: real): real { return SystemTheme.roundedCorners ? v : 0 }
    function pill(size: real): real { return SystemTheme.roundedCorners ? size / 2 : 0 }
    // Never translucent: an opaque panel of material by design (04-tactile.html).
    readonly property bool backdrop: false
    readonly property bool backdropCapable: false // 透明效果 has nothing to do here

    // ---- geometry (logical px; even, so 150 % lands on whole pixels) ----------
    // The window is Windows' own (DWM: 8 on Windows 11), as fluent's. What
    // sits by its corners is concentric with it: inner = max(8 − inset, 4).
    readonly property real windowRadius: SystemTheme.roundedCorners && SystemTheme.nativeWindowBorder ? 8 : 0
    readonly property int searchInset: 10        // groove to the window edge (top, left, right)
    readonly property int searchRadius: r(Math.max(4, 8 - searchInset)) // by the window's corners: 4
    readonly property int grooveKeyInset: 7      // keys in the groove: 7 from its top, bottom, right
    readonly property int grooveKeyRadius: r(2)     // 4 - 7 < 0, so the minimum 2
    // 7 is odd: at 150 % it is 10.5 device px. Hosts place the keys this far
    // in (whole device pixels, the same on top, bottom and right) and make
    // them groove height - 2 x this tall, so all three insets stay equal.
    function grooveKeyInsetAt(dpr: real): real { return Math.round(grooveKeyInset * dpr) / dpr }
    readonly property int grooveTextInset: 11    // the magnifier's left inset in the groove
    readonly property int keyRadius: r(6)
    readonly property int keySide: 2             // the side under a key's face
    readonly property int keyTravel: 2           // how far a face goes down
    readonly property int keyGap: 4              // between keys in a group
    readonly property int ledSize: 6
    readonly property int ledInset: 7            // the lamp's left inset in a key
    readonly property int ledGap: 5              // lamp to label
    readonly property int switchPadding: 2       // knob to the switch groove, all four sides
    readonly property int switchRadius: r(8)
    readonly property int knobRadius: r(6)
    readonly property int liftRadius: r(10)         // the lifted row card
    readonly property int cardInset: 10          // card to the tray (top, left, right)
    readonly property int menuRadius: r(8)       // a menu window's corner is DWM's: 8
    readonly property int menuPadding: 6
    readonly property int menuItemRadius: r(Math.max(4, 8 - menuPadding)) // concentric: 4
    readonly property int menuItemHeight: 34
    readonly property real pageTabsPad: switchPadding // the knob's room from the groove's ends, as from its top and bottom
    readonly property real pageTabsInset: 0 // the page tabs have a track: it starts at the cards' edge
    readonly property int capRadius: r(5)           // key hints
    readonly property int seamThickness: 2
    readonly property int trayPadding: 10
    readonly property int panelLightHeight: 140  // the panel's top light fades out over this
    readonly property int toggleWidth: 40
    readonly property int toggleHeight: 20
    readonly property int checkSize: 20
    readonly property int animFast: 120          // key travel, colours
    readonly property int animSlide: 180         // knobs, the card sliding between rows
    readonly property int animMove: animSlide

    // ---- geometry the hosts lay out by (Theme reads these names) ---------------
    // The search groove: 10 from the search row's top, left and right, radius 8.
    readonly property real fieldInset: searchInset
    readonly property real fieldRadius: searchRadius
    // Keys inside the groove: the same whole-device-pixel inset on top, bottom
    // and right; the host makes them fieldHeight - 2 x this tall.
    function fieldKeyInsetAt(dpr: real): real { return grooveKeyInsetAt(dpr) }
    // Result rows (from the list's sides): the card 10 in, the 34 px icon 10
    // further, the name 12 after it, the row keys 14 from the card's right.
    readonly property real rowInset: cardInset
    readonly property real hintX: 17 // the number mid-way between the card's edge (10) and the icon (48)
    // No grip over the search box: the field's own edge already marks the header, and the grip sat on it.
    readonly property bool grip: false
    readonly property real iconInset: cardInset + 10
    readonly property real textStart: cardInset + 10 + 34 + 12
    readonly property real actionsInset: cardInset + 14
    readonly property int rowSlide: animSlide       // the lifted card slides between rows
    // Row keys stand on their own (26 x 28, 4 apart): no capsule round them.
    readonly property real capsuleRadius: 0
    readonly property real capsulePadding: 0
    readonly property real capsuleKeyRadius: keyRadius
    readonly property color capsuleFill: "transparent"
    readonly property real typeChipHeight: 30

    // ---- accent: one warm signal orange, fixed (not the Windows accent) -------
    // Only as a lamp, the caret, match ink, and one fill under white text.
    readonly property color led: dark ? "#FF7A30" : "#F2600F"
    readonly property color ledGlow: dark ? Qt.rgba(1, 122 / 255, 48 / 255, 0.45) : Qt.rgba(242 / 255, 96 / 255, 15 / 255, 0.40)
    readonly property color accentInk: dark ? "#FFA36B" : "#B13D08"   // 4.9 tray / 5.9 card (light), 7.8 / 5.7 (dark)
    readonly property color accentFillColor: "#C2410C"                // under white, 5.2:1
    readonly property color caret: led

    // ---- material -----------------------------------------------------------
    readonly property color panelTop: dark ? "#313134" : "#FAF8F4"
    readonly property color panelBottom: dark ? "#2A2A2D" : "#F3EFE9"
    readonly property real grainOpacity: dark ? 0.02 : 0.024 // the prototype's 0.03 / 0.045 of a softer noise
    readonly property url grainSource: Qt.resolvedUrl(dark ? "grain-light.png" : "grain-dark.png")
    readonly property color seamDark: dark ? black(0.26) : Qt.rgba(60 / 255, 40 / 255, 20 / 255, 0.085)
    readonly property color seamLight: dark ? white(0.045) : white(0.75)
    readonly property color edgeColor: dark ? "#6A6A6A" : "#B4B4B4"   // 1 px neutral grey, as Theme.windowEdge

    // recessed things: the search groove, switch grooves, the results tray
    readonly property color slot: dark ? "#202022" : "#ECE8E1"
    readonly property color slotShade: dark ? black(0.34) : warm(0.115)    // its top inner line
    readonly property color slotFade: dark ? black(0.10) : warm(0.025)     // the few px under it
    // The taskbar box stands for the search groove (C++, Theme.nativeLook):
    // the slot, its shade as a line all round (no inner shadow there).
    readonly property color searchBoxFill: slot
    readonly property color searchBoxEdge: Qt.tint(slot, slotShade)
    readonly property real searchBoxRadius: searchRadius
    readonly property color trayColor: dark ? "#252527" : "#EDE9E2"
    readonly property color trayShade: dark ? black(0.09) : warm(0.035)   // CSS inset 0 1 2 -1 .28 / .08, as it lands

    // keys
    readonly property color keyFaceTop: dark ? "#55555B" : "#FFFFFF"
    readonly property color keyFaceBottom: dark ? "#4A4A4F" : "#F6F3EE"
    readonly property color keyRim: dark ? black(0.40) : warm(0.14)
    readonly property color keyRimBottom: dark ? black(0.57) : warm(0.225)
    readonly property color keySideColor: dark ? "#2A2A2D" : "#DFD8CD"
    readonly property color keyHighlight: dark ? white(0.17) : white(1)
    readonly property color keyDrop: dark ? black(0.55) : warm(0.30)
    readonly property color keyDownTop: dark ? "#2F2F32" : "#ECE7E0"
    readonly property color keyDownBottom: dark ? "#343437" : "#F1EDE7"
    readonly property color keyWall: dark ? black(0.22) : warm(0.085)
    readonly property color keyDisabledFace: dark ? "#38383B" : "#F1EDE7"
    readonly property color capLine: dark ? white(0.16) : warm(0.20)        // key hints: outline only
    // a pressed coloured type key
    readonly property color chipDownRim: black(0.16)
    readonly property color chipDownWall: black(0.18)

    // the lifted card (selected row) and menus
    readonly property color cardColor: dark ? "#3B3B3F" : "#FFFEFC"
    readonly property color cardLine: dark ? black(0.28) : warm(0.07)
    readonly property color cardHighlight: dark ? white(0.075) : white(1)
    readonly property color cardShadowNear: dark ? black(0.18) : warm(0.05)
    readonly property color cardShadow: dark ? black(0.26) : warm(0.09)
    readonly property color menuShadowNear: dark ? black(0.20) : warm(0.05)
    readonly property color menuShadow: dark ? black(0.42) : warm(0.16)
    readonly property color rowHover: dark ? white(0.03) : warm(0.03)
    // a settings plate: flat (it cannot be clicked), a step lighter than the panel
    readonly property color plate: dark ? "#333336" : "#FCFAF7"
    readonly property color plateHover: dark ? "#37373A" : "#FFFEFB"

    // ink
    readonly property color ink: dark ? "#F3F1ED" : "#1F1B16"
    readonly property color ink2: dark ? "#C9C4BC" : "#554F46"
    readonly property color ink3: dark ? "#B0ABA3" : "#6B645A"
    // a resting key label: ink2, a step lighter on dark (ink2 on the dark key face is 4.3:1; this 5.0)
    readonly property color keyInk: dark ? "#D8D3CC" : ink2
    readonly property color neutralFill: dark ? "#64646B" : "#34312C"
    readonly property color dangerInk: dark ? "#FF8078" : "#C2362F"
    readonly property color dangerFillColor: "#B42D26"
    readonly property color favorite: "#D88A0B"

    // match highlight: the words themselves, colour + weight, no backing
    readonly property color matchColor: accentInk
    readonly property int matchWeight: Font.Bold
    readonly property QtObject matchStyle: QtObject {
        readonly property color color: root.matchColor
        readonly property int weight: root.matchWeight
    }

    // paper icons
    readonly property color paperTop: dark ? "#F2EFEA" : "#FFFFFF"
    readonly property color paperBottom: dark ? "#E3DED5" : "#F2EDE6"
    readonly property color paperSide: dark ? "#99928A" : "#D3CABB"
    readonly property color paperRim: dark ? black(0.32) : warm(0.22)
    readonly property color paperFold: dark ? "#D2CBC0" : "#E6E0D5"
    readonly property color iconDrop: dark ? black(0.32) : Qt.rgba(60 / 255, 40 / 255, 20 / 255, 0.16)

    // ---- type palette: icon hue (light / dark) and the deep fill under white --
    readonly property var types: ({
        folder:   { hue: "#D18A0E", hueDark: "#F2B640", deep: "#A35B05" },
        document: { hue: "#2F6FDE", hueDark: "#77A6FF", deep: "#2459C9" },
        image:    { hue: "#119A72", hueDark: "#45CFA4", deep: "#0C7A5C" },
        video:    { hue: "#7F54E0", hueDark: "#AE8DFF", deep: "#6C43CF" },
        audio:    { hue: "#E0562F", hueDark: "#FF8D6C", deep: "#BC3C1D" },
        archive:  { hue: "#A97A25", hueDark: "#D7A657", deep: "#83601C" },
        program:  { hue: "#5558E6", hueDark: "#8E90FF", deep: "#4649C8" },
        ebook:    { hue: "#0F9393", hueDark: "#3FCBC9", deep: "#0B7474" },
        code:     { hue: "#BC4CAB", hueDark: "#E585D6", deep: "#A03A8F" }
    })
    // `hue`: a palette key above, or any colour (its deep fill is derived).
    function typeEntry(hue: var): var {
        if (typeof hue === "string" && types[hue] !== undefined)
            return types[hue]
        const c = Qt.color(hue === undefined || hue === null || hue === "" ? "#888888" : hue)
        return { hue: c, hueDark: Qt.lighter(c, 1.35), deep: Qt.darker(c, 1.3) }
    }
    // ChipSurface.hueIndex -> palette key: -2 文件夹, -1 不限 (none),
    // 0.. in TypeStrip.dotColors order (文档 图片 视频 音频 压缩包 程序 电子书 代码), wrapping.
    readonly property var typeOrder: ["document", "image", "video", "audio", "archive", "program", "ebook", "code"]
    function hueKey(index: int): string {
        if (index === -2)
            return "folder"
        if (index < 0)
            return ""
        return typeOrder[index % typeOrder.length]
    }
    function typeHue(hue: var): color { const e = typeEntry(hue); return dark ? e.hueDark : e.hue }
    function typeDeep(hue: var): color { return typeEntry(hue).deep }

    // ---- the names Theme.qml has always had, in this material ----------------
    readonly property color background: panelBottom
    readonly property color windowEdge: edgeColor
    readonly property color divider: seamDark
    readonly property color text: ink
    readonly property color subtext: ink2
    readonly property color faint: ink3
    readonly property color hover: rowHover
    readonly property color selection: dark ? white(0.07) : warm(0.06)
    readonly property color textSelection: Qt.rgba(led.r, led.g, led.b, 0.30)
    readonly property color badge: dark ? Qt.rgba(1, 122 / 255, 48 / 255, 0.14) : Qt.rgba(242 / 255, 96 / 255, 15 / 255, 0.10)
    readonly property color track: slot
    readonly property color chipActive: keyFaceTop
    readonly property color chipHover: dark ? "#3A3A3D" : "#E6E1D9"
    readonly property color chipBorder: keyRim
    readonly property color pieHover: dark ? "#404043" : "#E6E1D9"
    readonly property color pieShadow: dark ? black(0.36) : warm(0.12)
    readonly property color keycap: "transparent"   // key hints are outlines (KeyHint)
    readonly property color checker: dark ? "#D0D0D0" : "#FFFFFF"
    readonly property color checkerAlt: dark ? "#A0A0A0" : "#D6D6D6"
    readonly property color page: panelBottom
    readonly property color cardFill: plate    // Theme.card (the name `card` is the hook)
    readonly property color cardBorder: cardLine
    readonly property color navHover: rowHover
    readonly property color navSelected: selection
    readonly property color control: keyFaceTop
    readonly property color controlHover: keyFaceTop
    readonly property color controlPressed: keyDownTop
    readonly property color controlBorder: keyRim
    readonly property color inputFocused: slot
    readonly property color accentText: "#FFFFFF"
    readonly property color menuBackground: cardColor
    readonly property color menuHover: keyFaceTop
    readonly property color danger: dangerInk
    readonly property color dangerFill: dangerFillColor
    readonly property color success: dark ? "#6CCB5F" : "#1E7A3C"
    readonly property color caution: dark ? "#F2B640" : "#9D5D00"
    readonly property color closeHover: "#C42B1C"
    readonly property color closePressed: Qt.rgba(0.769, 0.169, 0.110, 0.9)
    readonly property color accent: accentInk
    readonly property color accentFill: accentFillColor
    readonly property color previewAccent: accentFillColor // the theme pictures of the settings (ThemeCard): this style's colour, not the Windows one
    readonly property color accentLine: led                 // markers and the caret: the lamp orange
    readonly property color accentTint: selection           // a selected row is lifted, not tinted
    // The chrome (search row, type strip, footer) is the window's own panel:
    // no layer over it, so its top light and grain show. The results are the
    // tray (hook `tray`) cut into it, which draws its own top line.
    readonly property color layer: "transparent"
    readonly property color layerEdge: "transparent"
    readonly property color pressed: dark ? white(0.05) : warm(0.06)
    readonly property color tagFill: dark ? white(0.07) : warm(0.06)
    readonly property color chipRest: keyDownTop
    readonly property color keyEdge: capLine
    readonly property color menuEdge: cardLine
    readonly property int menuShadowSpread: 6
    readonly property string iconFont: SystemTheme.iconFont
    // radii (Theme gates them by SystemTheme.roundedCorners)
    readonly property int cardRadius: r(8)          // settings plates
    readonly property int largeCardRadius: r(10)
    readonly property int insetCardRadius: r(6)
    readonly property int controlRadius: keyRadius
    readonly property int largeControlRadius: r(8)
    readonly property int smallControlRadius: r(4)
    readonly property real smallRadius: r(2)
    readonly property int lineRadius: r(1)
    readonly property int progressRadius: r(2)
    readonly property int cardFocusRadius: r(13)
    readonly property int rowRadius: liftRadius

    // ---- parts (hooks) --------------------------------------------------------
    readonly property Component windowFrame: Component { WindowBody { look: root } }
    readonly property Component searchField: Component { SearchField { look: root } }
    readonly property Component inputField: Component { InputField { look: root } }
    readonly property Component keySurface: Component { KeySurface { look: root } }
    readonly property Component segmented: Component { Segmented { look: root } }
    readonly property Component chipSurface: Component { ChipSurface { look: root } }
    readonly property Component rowHighlight: Component { RowHighlight { look: root } }
    readonly property Component typeIcon: Component { TypeIcon { look: root } }
    readonly property Component menuFrame: Component { MenuFrame { look: root } }
    readonly property Component toggle: Component { Toggle { look: root } }
    readonly property Component keyHint: Component { HintCap { look: root } }
    readonly property Component separator: Component { Separator { look: root } }
    readonly property Component scrollBar: Component { ScrollThumb { look: root } }
    readonly property Component card: Component { CardSurface { look: root } }
    // The results tray, cut into the panel under the rows.
    readonly property Component tray: Component { Tray { look: root } }
    // Row keys are separate keys (the prototype's .acts): no capsule.
    readonly property Component actionCapsule: null
}
