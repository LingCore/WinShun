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
    readonly property Item item: built

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
    // a part that declares that name; the others ignore them.
    property var extra: ({})

    // Older names of `role`, as the brand and tactile parts were first written.
    readonly property bool raised: role === "rowAction"
    readonly property bool inField: role === "scope" || role === "fieldKey"
    readonly property bool bar: role !== "menu"
    readonly property bool inMenu: role === "menu"
    readonly property bool keyed: role === "scope"
    readonly property bool checkbox: kind === "check"

    readonly property var names: ["hovered", "pressed", "checked", "focused", "selected", "current", "active", "error", "isAll",
        "highlighted", "danger", "maximized", "mixed", "hueIndex", "count", "currentIndex", "hoveredIndex", "pressedIndex",
        "segments", "kind", "paint", "size", "orientation", "position", "extent", "corner", "fill", "dpr", "flat", "role",
        "raised", "inField", "bar", "inMenu", "keyed", "checkbox"]

    property Item built: null

    // A value the part works out (contentOffsetY, foreground, leadingInset,
    // textColor …), or the host's own for a part without it.
    function out(name: string, fallback: var): var {
        const it = built
        if (!it)
            return fallback
        const value = it[name]
        return value === undefined ? fallback : value
    }
    // The same for a function of the part (segmentX(i) …).
    function call(name: string, args: var, fallback: var): var {
        const it = built
        if (!it || typeof it[name] !== "function")
            return fallback
        return it[name].apply(it, args)
    }

    function applyExtra() {
        if (!built || !extra)
            return
        for (const name in extra) {
            if (name in built)
                built[name] = extra[name]
        }
    }

    function rebuild() {
        if (built) {
            built.destroy()
            built = null
        }
        if (!part)
            return
        const made = part.createObject(hook)
        if (!made) {
            console.warn("Hook: the part could not be made:", part.errorString())
            return
        }
        for (const name of names) {
            if (name in made)
                made[name] = Qt.binding(() => hook[name])
        }
        made.anchors.fill = hook
        built = made
        applyExtra()
    }

    onPartChanged: rebuild()
    onExtraChanged: applyExtra()
    Component.onCompleted: if (!built) rebuild()
}
