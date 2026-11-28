import QtQuick

// One of the style's parts (themes/README.md), drawn here at this item's
// place and size: `part` is a hook of Theme.look (Theme.look.keySurface, …),
// the rest is the state the part draws. Only the state names the part
// declares are passed on, each as a binding; a part made for another style
// (Theme.style changed) takes this one's place at once.
//
// What the part works out for the host (the colour of the words on it, how
// far they go down when pressed, where a label starts) is read with out() and
// call(), with the host's own value for a part that has no such thing.
Item {
    id: hook

    property Component part
    // The part made (null: none, or not yet). Read by the host; set here only.
    property Item item: null
    // Made the first time it is shown, then kept (hidden with the host):
    // for a host that is mostly hidden, in every row of a list (a row's
    // buttons, its hover, its tip). Making a part costs far more than
    // drawing one, and a list makes and reuses rows while it scrolls.
    // Until then out() and call() give the host's own values.
    property bool lazy: false

    // The state, named as in the CONTRACT (design/themes/CONTRACT.md §3).
    property bool hovered: false
    property bool pressed: false
    property bool checked: false
    property bool focused: false
    property bool selected: false
    property bool current: false
    property bool active: true
    property bool error: false
    property bool isAll: false
    property bool highlighted: false // a primary button: accent fill, white words
    property bool danger: false // an armed delete: danger fill, white words
    property bool flat: false // no fill at rest (a toolbar's icon button)
    property bool iconOnly: false // segmented choices with glyphs, not text tabs
    property bool maximized: false // windowFrame: no corners, no edge
    property bool mixed: false // toggle: part on (the knob in the middle)
    property int hueIndex: -1 // chipSurface/typeIcon: -2 folder, -1 不限, 0.. the file types
    property int count: 0 // segmented
    property int currentIndex: -1
    property int hoveredIndex: -1
    property int pressedIndex: -1
    property var segments: [] // segmented: [{x, width}] of each segment, in this item
    property string kind // typeIcon (folder doc xls …), toggle ("check")
    property string paint: "all" // windowFrame: back | edge | all
    property real size: 0
    property int orientation: Qt.Horizontal // separator
    property real position: 0 // scrollBar: the thumb's y and height, in this item
    property real extent: 0
    property real corner: -1 // a corner radius the host needs (-1: the part's own)
    property color fill: "transparent" // windowFrame: the window's body colour
    property real dpr: 1
    // Where the part stands, for a style that draws it differently there:
    //   keySurface   "" | "rowAction" (a result row's buttons) | "fieldKey" (a key in the search field)
    //                | "toolbar" | "footer" | "dialog"
    //   segmented    "" | "scope" (全部/文件/内容 in the search field) | "sort" | "tabs" | "switch"
    //   rowHighlight "" | "list" | "menu" | "panel"
    //   chipSurface  "" | "type" (the type strip) | "group" (clipboard groups) | "filter"
    property string role
    // Inputs only one style knows ({ name: value }), passed on as they are to
    // a part that declares that name; the others ignore them. null: none.
    property var extra: null

    // A menu's highlight (`role` "menu"): read by the hosts that draw their own there.
    readonly property bool inMenu: role === "menu"

    // A window has hundreds of these (the settings window some 1800: every
    // button, switch, field and separator): nothing here is worked out or
    // kept per instance that only rebuild() needs. The names passed on are a
    // function's, not a property (an array in each), and the older names of
    // `role` are worked out when a part asks for them (state()), not bound
    // in every Hook for the few parts that declare one.
    function names() {
        return ["hovered", "pressed", "checked", "focused", "selected", "current", "active", "error", "isAll",
            "highlighted", "danger", "maximized", "mixed", "hueIndex", "count", "currentIndex", "hoveredIndex", "pressedIndex",
            "segments", "kind", "paint", "size", "orientation", "position", "extent", "corner", "fill", "dpr", "flat", "iconOnly", "role",
            "raised", "inField", "bar", "inMenu", "keyed", "checkbox"]
    }
    // The state by name; the older names of `role` and `kind`, as the brand
    // and tactile parts were first written, among them.
    function state(name: string): var {
        switch (name) {
        case "raised": return hook.role === "rowAction"
        case "inField": return hook.role === "scope" || hook.role === "fieldKey"
        case "bar": return hook.role !== "menu"
        case "keyed": return hook.role === "scope"
        case "checkbox": return hook.kind === "check"
        default: return hook[name]
        }
    }

    // A value the part works out (contentOffsetY, foreground, leadingInset,
    // textColor …), or the host's own for a part without it.
    function out(name: string, fallback: var): var {
        const it = item
        if (!it)
            return fallback
        const value = it[name]
        return value === undefined ? fallback : value
    }
    // The same for a function of the part (segmentX(i) …).
    function call(name: string, args: var, fallback: var): var {
        const it = item
        if (!it || typeof it[name] !== "function")
            return fallback
        return it[name].apply(it, args)
    }

    function applyExtra() {
        if (!item || !extra)
            return
        for (const name in extra) {
            if (name in item)
                item[name] = extra[name]
        }
    }

    function rebuild() {
        if (item) {
            item.destroy()
            item = null
        }
        if (!part || (lazy && !visible))
            return
        // The state as it is now, given while the part is being made: its
        // Behaviors animate only changes after it is complete. A part made
        // late (lazy, or a reused row's) shows at once as it should be,
        // instead of fading in from its defaults; later changes still
        // animate. Names the part does not declare are ignored.
        const all = names()
        const now = {}
        for (const name of all)
            now[name] = state(name)
        if (extra) {
            for (const name in extra)
                now[name] = extra[name]
        }
        const made = part.createObject(hook, now)
        if (!made) {
            console.warn("Hook: the part could not be made:", part.errorString())
            return
        }
        for (const name of all) {
            if (name in made)
                made[name] = Qt.binding(() => hook.state(name))
        }
        made.anchors.fill = hook
        item = made
        applyExtra()
    }

    onPartChanged: rebuild()
    onVisibleChanged: if (visible && !item && part) rebuild() // `visible` is the effective one: a hidden ancestor hides it too
    onExtraChanged: applyExtra()
    Component.onCompleted: if (!item) rebuild()
}
