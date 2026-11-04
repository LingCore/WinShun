import QtQuick

// rowHighlight: under a row that is chosen (`selected`: the keyboard's row,
// results, clipboard, panel, menus) or under the mouse (`hovered`). The host
// places it (inset from the list's sides). Fluent's: the accent's tint and a
// 3 × 16 accent pill at its left that grows in each time it lands (`position`
// is where the host's highlight is, so a move shows); hovered, a grey fill.
Rectangle {
    id: part

    property QtObject look: null
    property bool selected: false
    property bool hovered: false
    property bool current: false // keyboard-current without being chosen (a plain outline elsewhere)
    property real position: 0
    property real dpr: 1

    function snap(v) { return Math.round(v * dpr) / dpr }

    radius: look ? look.rowRadius : 0
    color: !look ? "transparent" : selected ? look.accentTint : hovered ? look.hover
           : Qt.rgba(look.hover.r, look.hover.g, look.hover.b, 0)

    Rectangle {
        id: pill
        visible: part.selected
        x: 0
        width: part.snap(3)
        height: Math.min(16, part.height - 8)
        y: part.snap((part.height - height) / 2)
        radius: part.look ? part.look.pill(width) : 0
        color: part.look ? part.look.accentLine : "transparent"
        transform: Scale { id: grow; origin.y: pill.height / 2 }
    }

    NumberAnimation {
        id: pop
        target: grow
        property: "yScale"
        from: 0.35
        to: 1
        duration: 200
        easing.type: Easing.OutCubic
    }
    onPositionChanged: if (selected) pop.restart()
    onSelectedChanged: if (selected) pop.restart()
}
