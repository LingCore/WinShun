import QtQuick
import WinShun

// 品牌色彩 (brand): the Logo's colours as one memorable mark — a continuous
// ring round the focused search field, the caret and the matched words in
// terracotta — and quiet neutrals everywhere else. Design source:
// design/search-concepts/03-brand.html. Light and dark are both here, keyed
// on SystemTheme.dark; the parts in this folder take every colour from here
// (their `look`), and only this file reads SystemTheme.
QtObject {
    id: brandLook

    readonly property string name: "brand"
    readonly property bool dark: SystemTheme.dark
    readonly property bool backdrop: false // opaque: an 18 px window has no Mica
    readonly property bool backdropCapable: false // 透明效果 has nothing to do here
    readonly property bool rounded: SystemTheme.roundedCorners

    // ---- Pixel helpers (same rules as Theme: docs/pitfalls.md) ----
    function edge(dpr: real): real { return Math.max(1, Math.round(dpr)) / dpr }
    function snap(v: real, dpr: real): real { return Math.round(v * dpr) / dpr }
    function snapUp(v: real, dpr: real): real { return Math.ceil(v * dpr - 1e-6) / dpr }
    function pillRadius(size: real): real { return rounded ? size / 2 : 0 }
    function r(v: real): real { return rounded ? v : 0 }

    // ---- Logo (resources/logo.svg corners) ----
    readonly property color logoRed: dark ? "#FF7A68" : "#FF5A4A"
    readonly property color logoGreen: dark ? "#4ED67F" : "#2FBF5E"
    readonly property color logoBlue: dark ? "#5AA4FF" : "#2D86FF"
    readonly property color logoAmber: dark ? "#FFC542" : "#FFB01F"

    // ---- Neutrals ----
    readonly property color ink: dark ? "#F2F2F5" : "#1B1B20"
    readonly property color ink2: dark ? "#B6B6BF" : "#5C5C66"
    readonly property color ink3: dark ? "#8C8C96" : "#8A8A93"
    // The window: always opaque (an 18 px corner rules out Mica, which DWM
    // shapes square); the prototype's Mica tone, settled.
    readonly property color windowFill: dark ? "#202024" : "#FAF9FA"
    // The results area: one step lighter (light) / darker (dark) than the window.
    readonly property color listFill: dark ? "#1D1D20" : "#FDFCFD" // the prototype's white 55 % / black 10 % over the window, opaque
    readonly property color fieldFill: dark ? "#2B2B31" : "#FFFFFF"
    readonly property color fieldEdge: dark ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(0, 0, 0, 0.12)
    readonly property color line: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.075)
    readonly property color hoverFill: dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.04)
    readonly property color pressedFill: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.07)
    readonly property color segTrack: dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.05)
    readonly property color segPill: dark ? "#45454D" : "#FFFFFF"
    readonly property color segShadow: dark ? Qt.rgba(0, 0, 0, 0.25) : Qt.rgba(0, 0, 0, 0.10)
    readonly property color segRing: dark ? Qt.rgba(1, 1, 1, 0.00) : Qt.rgba(0, 0, 0, 0.05)
    // Ink fill: "不限" selected and a switch that is on. Not a colour, so the
    // words on it follow it (white on the near-black, near-black on the near-white).
    readonly property color neutralFill: dark ? "#ECECF0" : "#2A2A30"
    readonly property color neutralFillInk: dark ? "#1B1B20" : "#FFFFFF"
    readonly property color capEdge: dark ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(0, 0, 0, 0.14)
    readonly property color capsuleFill: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.045)
    readonly property color rowSelected: dark ? Qt.rgba(1, 1, 1, 0.07) : "#FFFFFF"
    readonly property color rowSelectedEdge: dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.08)
    readonly property color rowSelectedShadow: dark ? "transparent" : Qt.rgba(0, 0, 0, 0.04)
    readonly property color rowHover: dark ? Qt.rgba(1, 1, 1, 0.035) : Qt.rgba(0, 0, 0, 0.025)
    // Under the pointer on a settings card (rows, the panel's choices): the same lightness step in dark as in light (CIE L* 2.2) —
    // white 3.5 % over the card's #28282D steps 3.5, the launcher list's darker floor takes it better.
    readonly property color panelHover: dark ? Qt.rgba(1, 1, 1, 0.022) : Qt.rgba(0, 0, 0, 0.025)
    readonly property color indicator: dark ? "#D4D4DA" : "#3A3A42"
    // The thin bar down the left of a settings sub-row: the selected row's ink, quieter (neutral, never the brand colour).
    readonly property color subRowBar: Qt.alpha(indicator, dark ? 0.38 : 0.32)

    // ---- Brand marks ----
    readonly property color brandInk: dark ? "#EDA591" : "#A8402C" // matched words, bold: 6.1:1 / 8.0:1
    readonly property color caret: dark ? "#FF7A68" : "#F2553F"
    readonly property real ringWidth: 2
    readonly property real glowOpacity: dark ? 0.30 : 0.16
    readonly property color favorite: "#E39A12" // the 收藏 star

    // ---- matchStyle: the words themselves only (no underline, no marker) ----
    readonly property color matchColor: brandInk
    readonly property int matchWeight: Font.Bold
    readonly property QtObject matchStyle: QtObject {
        readonly property color color: brandLook.matchColor
        readonly property int weight: brandLook.matchWeight
    }

    // ---- Geometry ----
    readonly property int windowRadius: r(18) // drawn by us: DWM gives 8 at most
    readonly property int fieldInset: 10 // search field from the window edge (all sides of the row)
    readonly property int fieldRadius: r(Math.max(2, 18 - fieldInset)) // concentric with the window: 8
    // The taskbar box stands for this field (C++, Theme.nativeLook).
    readonly property color searchBoxFill: fieldFill
    readonly property color searchBoxEdge: fieldEdge
    readonly property real searchBoxRadius: fieldRadius
    readonly property int rowRadius: r(10)
    readonly property int rowInset: 10
    readonly property int menuRadius: r(10)
    readonly property int menuPadding: 4
    readonly property int menuItemRadius: Math.max(rounded ? 2 : 0, menuRadius - menuPadding) // concentric: 6
    readonly property int capsuleRadius: r(9) // the row-action capsule (capsuleFill), 2 round its 30 px keys of corner 7
    readonly property int capsulePadding: 2
    readonly property int capsuleKeyRadius: Math.max(rounded ? 2 : 0, capsuleRadius - capsulePadding)
    readonly property int chipRadius: r(8)
    readonly property int keyRadius: r(7)
    readonly property int keyHintRadius: r(5)
    readonly property int segRadius: r(8)
    readonly property int segInFieldRadius: r(2) // field 8 − inset 6, held at the minimum 2
    readonly property int segPad: 2
    readonly property int toggleWidth: 40
    readonly property int toggleHeight: 20
    readonly property int animFast: 120
    readonly property int animMove: 180
    readonly property var easeOut: [0, 0, 0, 1, 1, 1] // cubic-bezier(0,0,0,1)

    // ---- Geometry the hosts lay out by (themes/README.md) ----
    // The field's keys (pin, expand; 28 high in the 44 px field) sit 8 from its
    // top, bottom and right; the scope keys (32 high) centre in the same row.
    function fieldKeyInsetAt(dpr: real): real { return snap(8, dpr) }
    // Result rows, from the list's own edges (as fluent's): the highlight 10 in,
    // the 32 px icon 14 inside it, the name 60 inside it, the action capsule
    // 13 from the highlight's right (13 above and below it in a 60 px row too).
    readonly property int iconInset: 24
    readonly property int textStart: 70
    readonly property int actionsInset: 23
    readonly property int rowSlide: 0 // the highlight jumps; its bar scales in (animMove)
    readonly property int typeChipHeight: 0 // UiScale's chipHeight (28 at the default size)

    // ---- Shadows ----
    // Menus (RectangularShadow; blur = 1.2 × the CSS blur).
    readonly property color menuShadowNear: dark ? Qt.rgba(0, 0, 0, 0.30) : Qt.rgba(0, 0, 0, 0.06)
    readonly property color menuShadowFar: dark ? Qt.rgba(0, 0, 0, 0.30) : Qt.rgba(0, 0, 0, 0.10)
    readonly property color menuEdge: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.08)
    readonly property color menuFill: dark ? "#2F2F35" : "#FFFFFF"

    // ---- Type palette: one OKLCH family, same lightness for every hue ----
    // ink = mark (L .53 / .79), tint = tile under it (L .955 / .33),
    // deep = fill under white words (L .51, white ≥ 5.3:1; same in both themes).
    readonly property var kinds: ({
        slides:  { ink: ["#B04613", "#F5A578"], tint: ["#FFEBDE", "#4A2E1E"] },
        sheet:   { ink: ["#2C7F44", "#8DCC9A"], tint: ["#E4F6E7", "#253C2A"] },
        doc:     { ink: ["#336ABB", "#8DBCFF"], tint: ["#E4F1FF", "#25364E"] },
        pdf:     { ink: ["#B23F3F", "#FE9B95"], tint: ["#FFE8E5", "#4D2A28"] },
        image:   { ink: ["#007A90", "#7CC8DB"], tint: ["#E0F4F9", "#1F3A41"] },
        md:      { ink: ["#6B668D", "#B9B6D8"], tint: ["#F0EFF9", "#353440"] },
        folder:  { ink: ["#935F00", "#DFB273"], tint: ["#FBEEDD", "#42321C"] },
        video:   { ink: ["#7358B3", "#C0ACFB"], tint: ["#F1EDFF", "#373149"] },
        audio:   { ink: ["#AC4923", "#F8A183"], tint: ["#FFEBE4", "#482E25"] },
        archive: { ink: ["#90601F", "#DCB282"], tint: ["#FBEEDF", "#43321D"] },
        app:     { ink: ["#5560C1", "#A4B4FF"], tint: ["#EBEFFF", "#2F344B"] },
        ebook:   { ink: ["#007E79", "#7BCBC6"], tint: ["#DFF6F4", "#183C3A"] },
        code:    { ink: ["#984B89", "#E4A1D5"], tint: ["#FBEAF7", "#432D3E"] },
        text:    { ink: ["#696B78", "#B8B9C8"], tint: ["#EFF0F4", "#34353B"] },
        file:    { ink: ["#696B78", "#B8B9C8"], tint: ["#EFF0F4", "#34353B"] }
    })
    // Names and extensions the app may pass as a kind.
    readonly property var kindAliases: ({
        folder: "folder", dir: "folder", directory: "folder",
        doc: "doc", docx: "doc", word: "doc", rtf: "doc", wps: "doc", odt: "doc", document: "doc",
        xls: "sheet", xlsx: "sheet", xlsm: "sheet", csv: "sheet", et: "sheet", ods: "sheet", excel: "sheet", sheet: "sheet", spreadsheet: "sheet",
        ppt: "slides", pptx: "slides", dps: "slides", odp: "slides", key: "slides", powerpoint: "slides", slides: "slides", presentation: "slides",
        pdf: "pdf",
        image: "image", png: "image", jpg: "image", jpeg: "image", gif: "image", bmp: "image", webp: "image", svg: "image", ico: "image", tif: "image", tiff: "image", heic: "image", jxr: "image", picture: "image",
        md: "md", markdown: "md",
        archive: "archive", zip: "archive", rar: "archive", "7z": "archive", tar: "archive", gz: "archive", xz: "archive", cab: "archive", iso: "archive", compressed: "archive",
        code: "code", js: "code", ts: "code", py: "code", c: "code", cpp: "code", h: "code", hpp: "code", cs: "code", java: "code", go: "code", rs: "code", qml: "code", json: "code", xml: "code", html: "code", css: "code", ps1: "code", bat: "code", sh: "code", lua: "code", yaml: "code", yml: "code", toml: "code", ini: "code",
        audio: "audio", mp3: "audio", wav: "audio", flac: "audio", aac: "audio", ogg: "audio", m4a: "audio", wma: "audio", music: "audio",
        video: "video", mp4: "video", mkv: "video", avi: "video", mov: "video", wmv: "video", webm: "video", flv: "video", movie: "video",
        ebook: "ebook", epub: "ebook", mobi: "ebook", azw3: "ebook", djvu: "ebook", book: "ebook",
        app: "app", exe: "app", msi: "app", program: "app", lnk: "app",
        text: "text", txt: "text", log: "text",
        file: "file", other: "file"
    })
    function kindOf(kind: string): string {
        const k = (kind || "").toLowerCase().replace(/^\./, "")
        return kindAliases[k] !== undefined ? kindAliases[k] : "file"
    }
    function kindInk(kind: string): color { return kinds[kindOf(kind)].ink[dark ? 1 : 0] }
    function kindTint(kind: string): color { return kinds[kindOf(kind)].tint[dark ? 1 : 0] }
    // The clipboard's text and link tiles (ClipRow), in the same 淡彩 as the file icons: text as documents, a link as e-books' teal.
    readonly property color clipTextTint: kindTint("doc")
    readonly property color clipTextInk: kindInk("doc")
    readonly property color clipLinkTint: kindTint("ebook")
    readonly property color clipLinkInk: kindInk("ebook")

    // The type strip by place (TypeStrip: -2 文件夹, -1 不限, 0.. the kinds in
    // the order of its dotColors, which is the prototype's order).
    readonly property var placeKinds: ["doc", "image", "video", "audio", "archive", "app", "ebook", "code"]
    readonly property var placeDeep: ({
        folder: "#8D5A00", doc: "#2D64B5", image: "#00748A", video: "#6D52AD", audio: "#A6431C",
        archive: "#8A5A18", app: "#4F5ABA", ebook: "#007873", code: "#924583"
    })
    // A chip's colours by its place → { ink: the icon, deep: the fill when chosen }.
    function chipHue(place: int): var {
        const kind = place === -2 ? "folder" : place >= 0 ? placeKinds[place % placeKinds.length] : ""
        if (kind === "")
            return { ink: ink2, deep: neutralFill }
        return { ink: kinds[kind].ink[dark ? 1 : 0], deep: placeDeep[kind] }
    }

    // ---- The ring's gradient ----
    // A plain conic crowds its colours into the middle of the long edges, so
    // the stops are placed by perimeter length: walk the outline, read each
    // point's angle from the centre, give it the colour for its share of the
    // way round (OKLCH keys, as the prototype's ringBackground()). Returns
    // [{ position, color }] for a Qt ConicalGradient with angle 90 (it runs
    // counter-clockwise from 3 o'clock; CSS conic runs clockwise from 12).
    function oklch(L: real, C: real, h: real): color {
        const a = C * Math.cos(h * Math.PI / 180), b = C * Math.sin(h * Math.PI / 180)
        const l_ = L + 0.3963377774 * a + 0.2158037573 * b
        const m_ = L - 0.1055613458 * a - 0.0638541728 * b
        const s_ = L - 0.0894841775 * a - 1.2914855480 * b
        const l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_
        const lin = [4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                     -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                     -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s]
        const g = lin.map(c => {
            c = c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055
            return Math.round(Math.max(0, Math.min(1, c)) * 255) / 255
        })
        return Qt.rgba(g[0], g[1], g[2], 1)
    }
    function ringStops(W: real, H: real, isDark: bool, count: int): var {
        if (W <= 0 || H <= 0)
            return []
        const P = 2 * (W + H), dL = isDark ? 0.08 : 0
        const keys = [ // [position along the outline, L, C, hue], clockwise from the top-left corner
            [0,             0.64, 0.19, 25],   // red      top-left
            [W * 0.5,       0.78, 0.16, 75],   // amber    top middle
            [W,             0.70, 0.17, 148],  // green    top-right
            [W + H,         0.70, 0.12, 195],  // teal     bottom-right
            [W + H + W / 2, 0.62, 0.18, 258],  // blue     bottom middle
            [2 * W + H,     0.60, 0.17, 305],  // violet   bottom-left
            [P,             0.64, 0.19, 385]   // red again
        ]
        const colourAt = d => {
            let i = 0
            while (i < keys.length - 2 && d > keys[i + 1][0]) i++
            const k0 = keys[i], k1 = keys[i + 1], t = (d - k0[0]) / (k1[0] - k0[0])
            return oklch(k0[1] + (k1[1] - k0[1]) * t + dL, (k0[2] + (k1[2] - k0[2]) * t) * (isDark ? 0.9 : 1), k0[3] + (k1[3] - k0[3]) * t)
        }
        const point = d => {
            if (d <= W) return [d - W / 2, -H / 2]
            if (d <= W + H) return [W / 2, d - W - H / 2]
            if (d <= 2 * W + H) return [W / 2 - (d - W - H), H / 2]
            return [-W / 2, H / 2 - (d - 2 * W - H)]
        }
        const out = []
        for (let k = 0; k < count; k++) {
            const d = k / count * P, p = point(d)
            const cssDeg = (Math.atan2(p[0], -p[1]) * 180 / Math.PI + 360) % 360 // clockwise from 12
            out.push({ position: ((360 - cssDeg) % 360) / 360, color: colourAt(d) })
        }
        out.sort((a, b) => a.position - b.position)
        // The same colour at 0 and 1: no seam where the gradient wraps.
        const first = out[0], last = out[out.length - 1]
        const span = first.position + 1 - last.position
        const t = span > 0 ? (1 - last.position) / span : 0
        const seam = Qt.tint(last.color, Qt.rgba(first.color.r, first.color.g, first.color.b, t))
        out.unshift({ position: 0, color: seam })
        out.push({ position: 1, color: seam })
        return out
    }

    // ---- Theme's existing tokens, in brand values (same names) ----
    readonly property color accent: brandInk // text, glyphs, lines
    readonly property color accentFill: "#A8402C" // under white words: 6.1:1, both themes
    readonly property color accentText: "#FFFFFF"
    readonly property int fontStrong: Font.Bold
    readonly property color background: listFill // the window's base (results, page body); chrome lies on it as `layer`
    readonly property color windowEdge: dark ? "#6A6A6A" : "#B4B4B4"
    readonly property color divider: line
    readonly property color text: ink
    readonly property color subtext: ink2
    readonly property color faint: ink3
    readonly property color hover: hoverFill
    readonly property color selection: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06)
    readonly property color textSelection: Qt.rgba(brandInk.r, brandInk.g, brandInk.b, 0.30)
    readonly property color badge: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.05) // neutral: brand stays on the ring and the matches
    readonly property color track: segTrack
    readonly property color chipActive: segPill
    readonly property color chipHover: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.06)
    readonly property color chipBorder: capEdge
    readonly property color pieHover: dark ? "#3E3E46" : "#E6E5E9"
    readonly property color pieShadow: dark ? Qt.rgba(0, 0, 0, 0.36) : Qt.rgba(0, 0, 0, 0.12)
    readonly property color keycap: "transparent" // a key hint is a fine outline only (keyHint)
    readonly property color checker: dark ? "#D0D0D0" : "#FFFFFF"
    readonly property color checkerAlt: dark ? "#A0A0A0" : "#D6D6D6"
    readonly property color page: windowFill
    readonly property color cardFill: dark ? "#28282D" : "#FFFFFF"
    readonly property color cardBorder: dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.07)
    readonly property color cardHover: dark ? "#2C2C32" : "#FFFFFF"
    readonly property color cardBorderHover: dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.12)
    readonly property color navHover: hoverFill
    readonly property color navSelected: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.055)
    readonly property color control: fieldFill
    readonly property color controlHover: dark ? "#31313A" : "#F7F7F9"
    readonly property color controlPressed: dark ? "#29292F" : "#F1F1F4"
    readonly property color controlBorder: dark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.14)
    readonly property color inputFocused: dark ? "#232328" : "#FFFFFF"
    readonly property color menuBackground: menuFill
    readonly property color menuHover: hoverFill
    readonly property int menuItemHeight: 34
    readonly property real pageTabsInset: 0 // the page tabs have a track: it starts at the cards' edge
    readonly property color danger: dark ? "#FF7A80" : "#D2363E"
    readonly property color dangerFill: "#C42F38" // white 5.5:1
    readonly property color success: dark ? "#6CCB5F" : "#0F7B0F"
    readonly property color caution: dark ? "#FCE100" : "#9D5D00"
    readonly property color closeHover: "#C42B1C"
    readonly property color closePressed: Qt.rgba(0.769, 0.169, 0.110, 0.9)

    readonly property int cardRadius: r(10)
    readonly property int largeCardRadius: r(12)
    readonly property int insetCardRadius: r(8)
    readonly property int controlRadius: r(6)
    readonly property int largeControlRadius: r(8)
    readonly property int smallControlRadius: r(4)
    readonly property real smallRadius: r(2)
    readonly property int lineRadius: r(1)
    readonly property int progressRadius: r(2)
    readonly property int cardFocusRadius: r(15) // largeCardRadius + the 3 px focus gap

    // ---- The rest of fluent/Look.qml's token set (A's skeleton reads these) ----
    function pill(size: real): real { return pillRadius(size) }
    readonly property color accentLine: dark ? brandInk : accentFill // lines and markers (caret has its own)
    readonly property color accentTint: rowSelected // the selected row stays neutral in this style
    // The chrome (search row, type strip, footer) is the window's tone; the
    // results show the base, one step lighter (light) / darker (dark).
    readonly property color layer: windowFill
    readonly property color layerEdge: line
    readonly property color pressed: pressedFill
    readonly property color tagFill: capsuleFill
    readonly property color chipRest: neutralFill // 不限 chosen (words on it: neutralFillInk)
    readonly property color keyEdge: capEdge
    readonly property color menuShadow: dark ? Qt.rgba(0, 0, 0, 0.30) : Qt.rgba(0, 0, 0, 0.10)
    readonly property int menuShadowSpread: 6
    readonly property var typeHues: placeKinds.map(k => kinds[k].ink[dark ? 1 : 0])
    readonly property color folderHue: kinds.folder.ink[dark ? 1 : 0]
    function hueFor(index: int): color { return index === -2 ? folderHue : index < 0 ? faint : typeHues[index % typeHues.length] }
    readonly property string iconFont: SystemTheme.iconFont

    // ---- Parts (hooks): Hook { part: Theme.look.keySurface; ... } ----
    readonly property Component searchField: Component { SearchField { look: brandLook } }
    readonly property Component inputField: Component { InputField { look: brandLook } }
    readonly property Component keySurface: Component { KeySurface { look: brandLook } }
    readonly property Component segmented: Component { Segmented { look: brandLook } }
    readonly property Component chipSurface: Component { ChipSurface { look: brandLook } }
    readonly property Component rowHighlight: Component { RowHighlight { look: brandLook } }
    readonly property Component typeIcon: Component { TypeIcon { look: brandLook } }
    readonly property Component menuFrame: Component { MenuFrame { look: brandLook } }
    readonly property Component toggle: Component { Toggle { look: brandLook } }
    readonly property Component keyHint: Component { HintCap { look: brandLook } }
    readonly property Component separator: Component { Separator { look: brandLook } }
    readonly property Component scrollBar: Component { ScrollThumb { look: brandLook } }
    readonly property Component card: Component { CardSurface { look: brandLook } }
    readonly property Component actionCapsule: Component { ActionCapsule { look: brandLook } }
    readonly property Component tray: null // the list lies on listFill itself
}
