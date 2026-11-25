pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A column of the drop pie (DropPieWindow): the folders inside a chip's or a
// row's folder (GitHub #463), in a box as the line under the chips, its rows
// as the folder menu's (FolderMenuColumn). DropPie places it and says what
// the pointer is on; it only draws. A column too long for the screen has a
// strip at its top and its bottom that scroll it.
Item {
    id: column

    // DropPie.columns[i]: x, y, width, height, scrolls, strip, rows
    // [{text, disabled, branch, message}].
    required property var box
    // DropPie.columnStates[i]: first, hovered, target, opener, up, down, strip.
    required property var pointer
    required property real dpr
    required property int textSize // as the chips'
    readonly property int row: Theme.menuItemHeight
    readonly property real padding: Theme.menuPadding
    readonly property real strip: box.scrolls ? box.strip : 0

    x: Theme.snap(box.x, dpr)
    y: Theme.snap(box.y, dpr)
    width: Theme.snap(box.width, dpr)
    height: Theme.snap(box.height, dpr)

    SoftShadow {
        anchors.fill: frame
        radius: frame.radius
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Theme.largeControlRadius
        color: Theme.menuBackground
        border.width: Theme.edge(column.dpr)
        border.color: Theme.windowEdge
    }

    // A scroll strip: lit while the pointer is on it and it has further to go.
    component Strip: Item {
        id: strip
        required property bool more
        required property bool pointed
        required property string glyph
        required property real dpr

        MenuHighlight {
            anchors.fill: parent
            visible: strip.pointed && strip.more
            dpr: strip.dpr
        }
        Glyph {
            anchors.centerIn: parent
            glyph: strip.glyph
            size: 14
            color: strip.more ? Theme.text : Theme.faint
        }
    }

    Strip {
        visible: column.box.scrolls
        x: column.padding
        y: column.padding
        width: column.width - 2 * column.padding
        height: column.strip
        more: column.pointer.up === true
        pointed: column.pointer.strip === -1
        glyph: "" // ChevronUp
        dpr: column.dpr
    }

    ListView {
        x: column.padding
        y: column.padding + column.strip
        width: column.width - 2 * column.padding
        height: column.height - 2 * (column.padding + column.strip)
        clip: column.box.scrolls
        interactive: false
        reuseItems: true
        model: column.box.rows
        contentY: (column.pointer.first ?? 0) * column.row

        delegate: Item {
            id: entry

            required property var modelData
            required property int index
            readonly property bool target: index === column.pointer.hovered && column.pointer.target === true
            readonly property bool lit: !target && (index === column.pointer.hovered || index === column.pointer.opener)
            readonly property bool branch: modelData.branch === true
            readonly property color ink: target ? Theme.accentText : modelData.disabled ? Theme.faint : Theme.text

            width: ListView.view.width
            height: column.row

            Rectangle { // where letting go puts them: as the chip chosen
                anchors.fill: parent
                visible: entry.target
                radius: Theme.menuItemRadius
                color: Theme.accentFill
            }
            MenuHighlight {
                anchors.fill: parent
                visible: entry.lit
                dpr: column.dpr
            }
            Glyph {
                visible: entry.modelData.message !== true
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                glyph: "" // Folder
                size: 18
                color: entry.ink
            }
            Text {
                x: 42
                width: Math.max(0, entry.width - x - 26 - (entry.branch ? 20 : 0))
                anchors.verticalCenter: parent.verticalCenter
                text: entry.modelData.text
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: entry.ink
                font.pixelSize: column.textSize
            }
            Glyph {
                visible: entry.branch
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                glyph: "" // ChevronRight
                size: 14
                color: entry.ink
            }
        }
    }

    Strip {
        visible: column.box.scrolls
        x: column.padding
        y: column.height - column.padding - column.strip
        width: column.width - 2 * column.padding
        height: column.strip
        more: column.pointer.down === true
        pointed: column.pointer.strip === 1
        glyph: "" // ChevronDown
        dpr: column.dpr
    }
}
