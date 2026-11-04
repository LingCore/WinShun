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
    property real corner: -1
    property real dpr: 1

    readonly property real radius: corner >= 0 ? corner : inMenu ? look.menuItemRadius : look.rowRadius
    readonly property real edgeWidth: look.edge(dpr)
    readonly property bool raisedFace: selected && !inMenu
    // Hook.out names: the words stay ink on every state.
    readonly property color foreground: look.ink

    RectangularShadow {
        visible: row.raisedFace && !row.look.dark
        anchors.fill: parent
        radius: row.radius
        offset.y: 1
        blur: 2.4
        color: row.look.rowSelectedShadow
    }
    Rectangle {
        anchors.fill: parent
        radius: row.radius
        color: row.raisedFace ? row.look.rowSelected
             : row.inMenu && (row.selected || row.hovered) ? row.look.menuHover
             : row.hovered ? row.look.rowHover
             : Qt.rgba(row.look.rowHover.r, row.look.rowHover.g, row.look.rowHover.b, 0)
        border.width: row.raisedFace || (row.current && !row.inMenu) ? row.edgeWidth : 0
        border.color: row.selected ? row.look.rowSelectedEdge : row.look.capEdge
        Behavior on color { ColorAnimation { duration: row.look.animFast } }
    }
    Rectangle { // the ink bar, 4 px inside the face's edge
        visible: row.bar && !row.inMenu
        x: row.edgeWidth + 4
        width: row.look.snap(3, row.dpr)
        height: 16
        y: row.look.snap((row.height - height) / 2, row.dpr)
        radius: row.look.rounded ? width / 2 : 0
        color: row.look.indicator
        scale: row.selected ? 1 : 0
        Behavior on scale { NumberAnimation { duration: row.look.animMove; easing.type: Easing.Bezier; easing.bezierCurve: row.look.easeOut } }
    }
}
