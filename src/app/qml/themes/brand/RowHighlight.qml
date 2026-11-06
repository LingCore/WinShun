import QtQuick
import QtQuick.Effects

// rowHighlight: the floor under a result, clipboard, panel or menu row.
// The selected row is a neutral raised face — near-white with a very faint
// 1 px edge and a hairline shadow (light), white 7 % (dark) — and a neutral
// ink bar on its left (3 × 16) that grows in. No red or pink here. current
// without selected (keyboard-current, not chosen): the edge alone. Under the
// pointer: a whisper of grey. Fills its host (the row's box, inset 10 from
// the list's sides). In a menu (role "menu": Hook gives inMenu, bar false)
// hovered or selected is the plain hover fill, corner menuItemRadius.
Item {
    id: row

    property QtObject look
    property bool selected: false
    property bool hovered: false
    property bool current: false
    property real position: 0
    property bool bar: true
    property bool inMenu: false
    property string role: ""
    property real corner: -1
    property real dpr: 1

    readonly property real radius: look ? (corner >= 0 ? corner : inMenu ? look.menuItemRadius : look.rowRadius) : 0
    readonly property real edgeWidth: look ? look.edge(dpr) : 0
    readonly property bool raisedFace: selected && !inMenu
    readonly property color hoverFill: look ? (role === "panel" ? look.panelHover : look.rowHover) : "transparent"
    // Hook.out names: the words stay ink on every state.
    readonly property color foreground: look ? look.ink : "transparent"
    // Hook.out: how far the row's content starts from the face's left edge — the result row's icon sits 14 past it, 6 clear of the 3 px bar.
    readonly property real contentInset: 14

    RectangularShadow {
        visible: row.look ? row.raisedFace && !row.look.dark : false
        anchors.fill: parent
        radius: row.radius
        offset.y: 1
        blur: 2.4
        color: row.look ? row.look.rowSelectedShadow : "transparent"
    }
    Rectangle {
        anchors.fill: parent
        radius: row.radius
        color: !row.look ? "transparent"
             : row.raisedFace ? row.look.rowSelected
             : row.inMenu && (row.selected || row.hovered) ? row.look.menuHover
             : row.hovered ? row.hoverFill
             : Qt.rgba(row.hoverFill.r, row.hoverFill.g, row.hoverFill.b, 0)
        border.width: row.raisedFace || (row.current && !row.inMenu) ? row.edgeWidth : 0
        border.color: row.look ? (row.selected ? row.look.rowSelectedEdge : row.look.capEdge) : "transparent"
        Behavior on color { ColorAnimation { duration: row.look ? row.look.animFast : 0 } }
    }
    Rectangle { // the ink bar, 4 px inside the face's edge
        visible: row.bar && !row.inMenu
        x: row.edgeWidth + 4
        width: row.look ? row.look.snap(3, row.dpr) : 0
        height: 16
        y: row.look ? row.look.snap((row.height - height) / 2, row.dpr) : 0
        radius: row.look ? (row.look.rounded ? width / 2 : 0) : 0
        color: row.look ? row.look.indicator : "transparent"
        scale: row.selected ? 1 : 0
        Behavior on scale { NumberAnimation { duration: row.look ? row.look.animMove : 0; easing.type: Easing.Bezier; easing.bezierCurve: row.look ? row.look.easeOut : [0, 0, 0, 1, 1, 1] } }
    }
}
