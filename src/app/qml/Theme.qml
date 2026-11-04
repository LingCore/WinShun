pragma Singleton
import QtQuick
import WinShun

// All colours and metrics of the UI. Follows the Windows light/dark setting
// and accent colour (see SystemTheme in C++), and the 界面风格 (Theme.style):
// a style's colours, radii and parts live in its Look (themes/<style>/Look.qml,
// see themes/README.md); the names below stay the ones every component uses,
// each read from the Look in effect. Type sizes and row heights are the
// styles' common ones (UiScale).
QtObject {
    id: theme

    readonly property bool dark: SystemTheme.dark
    // fluent | brand | tactile
    readonly property string style: SystemTheme.uiStyle

    // Each style's Look, made the first time it is the one in effect. One
    // that cannot be made (a broken file) falls back to fluent, which must.
    property var looks: ({})
    function lookFor(name: string): QtObject {
        if (looks[name])
            return looks[name]
        const component = Qt.createComponent("themes/" + name + "/Look.qml")
        const made = component.status === Component.Ready ? component.createObject(theme) : null
        if (!made) {
            console.warn("Theme: style", name, "could not be loaded:", component.errorString())
            return name === "fluent" ? null : lookFor("fluent")
        }
        looks[name] = made
        return made
    }
    readonly property QtObject look: lookFor(style)
    // fluent's Look holds every token; a style's Look that leaves one out
    // gets fluent's.
    readonly property QtObject base: lookFor("fluent")
    function pick(name: string): var {
        const value = look[name]
        return value === undefined ? base[name] : value
    }
    // A token only some styles have, with the host's value for the others.
    function tok(name: string, fallback: var): var {
        const value = look[name]
        return value === undefined ? fallback : value
    }

    // Our windows can stand on Mica (see SystemTheme.backdrop): the layers on
    // them are translucent then, with the fill colours WinUI uses there; over
    // a plain background they keep their solid colours. A style that rounds
    // its windows itself has none.
    readonly property bool backdropCapable: pick("backdropCapable") // the style can have Mica at all (透明效果)
    readonly property bool backdrop: pick("backdrop") // For text, glyphs and lines: lightened on dark to read there.
    readonly property color accent: pick("accent") // For fills under white words or marks (accentText): deep enough for
    // white to read, whatever the accent (SystemTheme.accentFill).
    readonly property color accentFill: pick("accentFill")
    readonly property color accentLine: pick("accentLine")
    readonly property color accentTint: pick("accentTint") // FreeType synthesizes a missing heavy face only from Bold (700).
    // Keep the UI hierarchy when shipping the complete regular font alone.
    readonly property int fontStrong: Font.Bold
    // Matched characters (StyledText): their colour and weight, nothing under them.
    readonly property color matchColor: pick("matchColor")
    readonly property int matchWeight: pick("matchWeight")
    readonly property color background: pick("background")
    readonly property color layer: pick("layer")
    readonly property color layerEdge: pick("layerEdge") // The window itself: DWM rounds it on Windows 11 (win::styleFramelessWindow),
    // or the style does (look.frame "own", see WindowEdge).
    readonly property real windowRadius: pick("windowRadius")
    readonly property bool ownFrame: SystemTheme.ownFrameAvailable && pick("frame") === "own" && pick("windowRadius") > 0
    readonly property real cardRadius: pick("cardRadius")
    readonly property real largeCardRadius: pick("largeCardRadius")
    readonly property real insetCardRadius: pick("insetCardRadius")
    readonly property real controlRadius: pick("controlRadius")
    readonly property real largeControlRadius: pick("largeControlRadius")
    readonly property real smallControlRadius: pick("smallControlRadius")
    readonly property real smallRadius: pick("smallRadius")
    readonly property real lineRadius: pick("lineRadius")
    readonly property real progressRadius: pick("progressRadius")
    readonly property real cardFocusRadius: pick("cardFocusRadius")
    readonly property real menuRadius: pick("menuRadius")
    readonly property real menuItemRadius: pick("menuItemRadius")
    readonly property real rowRadius: pick("rowRadius") // Chips and scrollbars keep their existing proportions in rounded mode.
    function pillRadius(size: real): real { return look.pill ? look.pill(size) : base.pill(size) }
    // Edges on whole device pixels (docs/pitfalls.md, "边框 1 px 在 150% 下一边粗一边细").
    // dpr is the window's: Screen.devicePixelRatio, so a window moved to another
    // screen asks again. Read it once at a component's root (or the window's)
    // and pass it down: every item that reads Screen has an attached object of
    // its own, work on each move to another monitor (GitHub #111).
    // edge(): a 1 px line or border, as many device pixels as Qt draws a border
    // of 1 px with (1.5 at 150 % becomes 2). A line of plain 1 is 1 or 2 pixels
    // by where it lies, and a child inset by 1 covers half of a border.
    function edge(dpr: real): real { return Math.max(1, Math.round(dpr)) / dpr }
    // snap(): the nearest device pixel; snapUp(): the next one up (a size that must not shrink).
    function snap(v: real, dpr: real): real { return Math.round(v * dpr) / dpr }
    function snapUp(v: real, dpr: real): real { return Math.ceil(v * dpr - 1e-6) / dpr }
    readonly property color windowEdge: pick("windowEdge")
    readonly property color divider: pick("divider")
    readonly property color text: pick("text")
    readonly property color subtext: pick("subtext") // Hints and placeholders: Windows 11's tertiary text, legible on a card (≥ 3:1).
    readonly property color faint: pick("faint")
    readonly property color hover: pick("hover")
    readonly property color pressed: pick("pressed")
    readonly property color selection: pick("selection")
    readonly property color textSelection: pick("textSelection")
    readonly property color badge: pick("badge") // "应用" tag on app rows
    readonly property color tagFill: pick("tagFill") // 收藏 / 最近 beside a row's date

    readonly property color track: pick("track")
    readonly property color chipActive: pick("chipActive")
    readonly property color chipHover: pick("chipHover")
    readonly property color chipBorder: pick("chipBorder")
    readonly property color chipRest: pick("chipRest") // The drop pie (DropPieWindow): its middle with the pointer in it (let go
    // there and nothing happens), and a chip under the pointer that cannot be
    // taken; a step off the menu background.
    readonly property color pieHover: pick("pieHover") // The soft shadow round its chips, at their edge (SoftShadow).
    readonly property color pieShadow: pick("pieShadow") // A key cap's fill (KeyCap, a row's chord) and a key hint's outline (KeyHint).
    readonly property color keycap: pick("keycap")
    readonly property color keyEdge: pick("keyEdge") // The checkerboard under a translucent colour: light in both themes, as in
    // design tools, so that dark and light translucent colours both show.
    readonly property color checker: pick("checker")
    readonly property color checkerAlt: pick("checkerAlt") // Settings window (Windows 11 settings look)
    readonly property color page: pick("page")
    readonly property color card: pick("cardFill")
    readonly property color cardBorder: pick("cardBorder")
    readonly property color navHover: pick("navHover")
    readonly property color navSelected: pick("navSelected")
    readonly property color control: pick("control")
    readonly property color controlHover: pick("controlHover")
    readonly property color controlPressed: pick("controlPressed")
    readonly property color controlBorder: pick("controlBorder")
    readonly property color inputFocused: pick("inputFocused") // Words and marks on accentFill and dangerFill: white in both themes,
    // never Windows 11's black on a light accent (see docs/colors.md).
    // Not "onAccent": a name of on + capital letter reads as a signal handler,
    // and such a property was never re-evaluated when the theme changed.
    readonly property color accentText: pick("accentText")
    readonly property color menuBackground: pick("menuBackground")
    readonly property color menuHover: pick("menuHover")
    readonly property color menuEdge: pick("menuEdge")
    readonly property color menuShadow: pick("menuShadow")
    readonly property int menuShadowSpread: pick("menuShadowSpread")
    readonly property int menuItemHeight: pick("menuItemHeight")
    readonly property real menuPadding: pick("menuPadding")
    readonly property color danger: pick("danger") // words and glyphs
    readonly property color dangerFill: pick("dangerFill") // under white words (an armed delete button)
    readonly property color success: pick("success") // Windows 11's status colours
    readonly property color caution: pick("caution")
    readonly property color closeHover: pick("closeHover") // title bar close button, as in Windows 11
    readonly property color closePressed: pick("closePressed") // Type scale in pixels. Even sizes land on whole device pixels at 150 %.
    readonly property int fontDisplay: 28 // settings window title
    readonly property int fontSearch: 24 // search box
    readonly property int fontTitle: 18 // result names, section titles, empty state
    readonly property int fontBody: 16 // most text
    readonly property int fontCaption: 14 // descriptions, key caps, hints

    // 界面大小 (Settings uiSize): the launcher and the bar by file dialogs
    // in smaller or larger type, on lower or taller rows (the clipboard has
    // a step of its own, see UiScale); the settings window keeps the scale
    // above. 标准 is the scale above.
    readonly property UiScale metrics: UiScale { size: SystemTheme.uiSize }
    readonly property int step: SystemTheme.uiSize === "compact" ? 0 : SystemTheme.uiSize === "roomy" ? 2 : 1
    readonly property int uiSearch: metrics.uiSearch
    readonly property int uiTitle: metrics.uiTitle
    readonly property int uiBody: metrics.uiBody
    readonly property int uiCaption: metrics.uiCaption
    readonly property int uiTab: metrics.uiTab
    readonly property int uiSmall: metrics.uiSmall
    readonly property int headerHeight: SystemTheme.headerHeight // the search field's row: 56, 64, 72
    readonly property int rowHeight: metrics.rowHeight
    readonly property int footerHeight: metrics.footerHeight
    readonly property int tabHeight: metrics.tabHeight
    readonly property int chipHeight: metrics.chipHeight
    readonly property int stripHeight: metrics.stripHeight
    readonly property int tabsHeight: metrics.tabsHeight
    readonly property int keycapHeight: metrics.keycapHeight
    readonly property int badgeHeight: metrics.badgeHeight
    readonly property int labelRowHeight: metrics.labelRowHeight
    readonly property int hintRowHeight: metrics.hintRowHeight
    readonly property int fileRowHeight: metrics.fileRowHeight
    readonly property int panelRowHeight: metrics.panelRowHeight
    readonly property int searchIconSize: metrics.searchIconSize

    readonly property string iconFont: SystemTheme.iconFont

    // The window frame for C++ (OwnFrame in WindowEdge): radius and shadow of
    // an "own" frame; 0 is DWM's.
    readonly property real frameRadius: ownFrame ? pick("windowRadius") : 0
    readonly property var frameShadow: pick("windowShadow")

    // Geometry the hosts lay out by (themes/README.md, "宿主读的几何").
    readonly property real fieldInset: pick("fieldInset")
    readonly property real fieldRadius: pick("fieldRadius")
    function fieldKeyInsetAt(dpr: real): real { return (look.fieldKeyInsetAt ?? base.fieldKeyInsetAt)(dpr) }
    readonly property real rowInset: pick("rowInset")
    readonly property real iconInset: pick("iconInset")
    readonly property real textStart: pick("textStart")
    readonly property real actionsInset: pick("actionsInset")
    readonly property int rowSlide: pick("rowSlide")
    readonly property real capsuleRadius: pick("capsuleRadius")
    readonly property real capsulePadding: pick("capsulePadding")
    readonly property real capsuleKeyRadius: pick("capsuleKeyRadius")
    readonly property color capsuleFill: pick("capsuleFill")
    readonly property real typeChipHeight: pick("typeChipHeight") > 0 ? pick("typeChipHeight") : chipHeight
    readonly property int animFast: pick("animFast")
    readonly property int animMove: pick("animMove")
    // hair(): one device pixel, for lines inside a style's parts.
    function hair(dpr: real): real { return 1 / Math.max(1, dpr) }

    // The parts (hooks) of the style in effect, for Hook.part; one a Look
    // leaves out is fluent's. typeIcon, tray and actionCapsule may be null
    // (nothing drawn: system icons, no tray, no capsule).
    readonly property Component windowFrame: pick("windowFrame")
    readonly property Component searchField: pick("searchField")
    readonly property Component inputField: pick("inputField")
    readonly property Component keySurface: pick("keySurface")
    readonly property Component segmented: pick("segmented")
    readonly property Component chipSurface: pick("chipSurface")
    readonly property Component rowHighlight: pick("rowHighlight")
    readonly property Component typeIcon: look.typeIcon ?? null
    readonly property Component menuFrame: pick("menuFrame")
    readonly property Component toggle: pick("toggle")
    readonly property Component keyHint: pick("keyHint")
    readonly property Component separator: pick("separator")
    readonly property Component scrollBar: pick("scrollBar")
    readonly property Component cardPart: pick("card")
    readonly property Component tray: look.tray ?? null
    readonly property Component actionCapsule: look.actionCapsule ?? null

    // ---- for what C++ draws itself (SystemTheme.nativeLook): the tray menu,
    // the taskbar box, pins, the countdown, and plugins without QML (sent
    // with the plugins' appearance). Only tokens from above: one source.
    // Radii already squared by the 直角 setting; `rounded` for the pills
    // C++ works out itself (h / 2). `caret`: transparent where the style
    // has no caret colour of its own (the host keeps its own then).
    readonly property var nativeLook: ({
        style: theme.style,
        dark: theme.dark,
        rounded: SystemTheme.roundedCorners,
        windowRadius: theme.frameRadius,
        cardRadius: theme.cardRadius,
        controlRadius: theme.controlRadius,
        largeControlRadius: theme.largeControlRadius,
        smallControlRadius: theme.smallControlRadius,
        menuRadius: theme.menuRadius,
        menuItemRadius: theme.menuItemRadius,
        menuPadding: theme.menuPadding,
        background: theme.background,
        layer: theme.layer,
        page: theme.page,
        windowEdge: theme.windowEdge,
        divider: theme.divider,
        text: theme.text,
        subtext: theme.subtext,
        faint: theme.faint,
        hover: theme.hover,
        pressed: theme.pressed,
        tagFill: theme.tagFill,
        control: theme.control,
        controlHover: theme.controlHover,
        controlBorder: theme.controlBorder,
        inputFocused: theme.inputFocused,
        menuBackground: theme.menuBackground,
        menuHover: theme.menuHover,
        menuEdge: theme.menuEdge,
        accent: theme.accent,
        accentFill: theme.accentFill,
        accentLine: theme.accentLine,
        accentText: theme.accentText,
        textSelection: theme.textSelection,
        caret: theme.tok("caret", Qt.rgba(0, 0, 0, 0)),
        // The taskbar box as the style's search field (transparent, -1: the
        // box's own translucent pill, as Windows' search box).
        searchBoxFill: theme.tok("searchBoxFill", Qt.rgba(0, 0, 0, 0)),
        searchBoxEdge: theme.tok("searchBoxEdge", Qt.rgba(0, 0, 0, 0)),
        searchBoxRadius: theme.tok("searchBoxRadius", -1),
        danger: theme.danger,
        dangerFill: theme.dangerFill
    })
    onNativeLookChanged: SystemTheme.setNativeLook(nativeLook)
    Component.onCompleted: SystemTheme.setNativeLook(nativeLook)
}
