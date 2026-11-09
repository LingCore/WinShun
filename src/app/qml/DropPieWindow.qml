pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The drop pie (DropPie): the favorite folders as chips round the pointer,
// brought up by the key under Esc while files are held or dragged. The
// direction the pointer goes from the middle chooses a folder; the line
// under the chips says what letting go would do. The window is see-through
// and never takes the focus; DropPie places it, is its drop target, and
// hides it the moment the pointer leaves the chips' reach.
Window {
    id: pie

    required property DropPie dropPie

    readonly property real dpr: Screen.devicePixelRatio
    readonly property real k: dropPie.sizeScale
    // The centre on a whole device pixel, so the middle's edge is crisp.
    readonly property real cx: Theme.snap(dropPie.centre.x, dpr)
    readonly property real cy: Theme.snap(dropPie.centre.y, dpr)
    readonly property int labelSize: dropPie.labelPixelSize
    readonly property int smallSize: Math.max(12, labelSize - 1)
    readonly property real chipHeight: Theme.snap(dropPie.chipHeight, dpr)
    readonly property real chipRadius: Theme.pillRadius(chipHeight)
    readonly property int count: Math.max(1, dropPie.sectors.length)
    property real reveal: 0 // 0 → 1 as it appears; 0 at once when it goes

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"

    Connections {
        target: pie.dropPie
        function onRevealedChanged() {
            if (pie.dropPie.revealed) {
                revealAnimation.restart()
            } else {
                revealAnimation.stop()
                pie.reveal = 0
            }
        }
    }

    NumberAnimation {
        id: revealAnimation
        target: pie
        property: "reveal"
        from: 0
        to: 1 // exactly: the chips end on their whole pixels
        duration: SystemTheme.animations ? 140 : 0
        easing.type: Easing.OutCubic
    }

    // Where a chip is while it appears: from a little nearer the middle out
    // to its place, on whole pixels.
    function chipX(sector: var): real {
        const centre = sector.x + sector.width / 2
        return Theme.snap(cx + centre * (0.8 + 0.2 * reveal) - sector.width / 2, dpr)
    }
    function chipY(sector: var): real {
        const centre = sector.y + chipHeight / 2
        return Theme.snap(cy + centre * (0.8 + 0.2 * reveal) - chipHeight / 2, dpr)
    }

    // Drawn by the software renderer, the window is one whose fully
    // transparent pixels Windows passes over: a drag is not told it is
    // there. Until a drag comes over it nothing of it is drawn (reveal 0),
    // so no drag ever came over it and it never appeared (GitHub #150;
    // measured: none of the points over it were its own, half of them once
    // revealed, all of them either way with Direct3D). The least there can
    // be of a colour everywhere, outside what fades in: not to be seen
    // (a pixel under it darkens by 1 in 255 at most), but there.
    Rectangle {
        objectName: "pieHitFill"
        anchors.fill: parent
        visible: GraphicsInfo.api === GraphicsInfo.Software
        color: Qt.rgba(0, 0, 0, 1 / 255)
    }

    Item {
        anchors.fill: parent
        opacity: pie.reveal

        // All the shadows first, so none falls on a neighbouring chip.
        Repeater {
            model: pie.dropPie.sectors

            delegate: SoftShadow {
                required property var modelData

                x: pie.chipX(modelData)
                y: pie.chipY(modelData)
                width: Theme.snap(modelData.width, pie.dpr)
                height: pie.chipHeight
                radius: pie.chipRadius
            }
        }

        SoftShadow {
            x: middle.x
            y: middle.y
            width: middle.width
            height: middle.height
            radius: middle.radius
            spread: 4
        }

        // The middle: let go here and nothing happens. A dot on its rim
        // points at the folder chosen.
        Rectangle {
            id: middle

            readonly property real size: 2 * Theme.snap(pie.dropPie.hubRadius, pie.dpr)

            x: pie.cx - size / 2
            y: pie.cy - size / 2
            width: size
            height: size
            radius: size / 2
            color: pie.dropPie.hovered < 0 ? Theme.pieHover : Theme.menuBackground
            border.width: Theme.edge(pie.dpr)
            border.color: Theme.windowEdge

            Glyph {
                anchors.centerIn: parent
                glyph: "\uE711" // Cancel
                size: 12
                color: Theme.subtext
            }
        }

        Rectangle {
            readonly property real angle: pie.dropPie.hovered * 2 * Math.PI / pie.count
            readonly property real distance: middle.size / 2 + 6 * pie.k

            visible: pie.dropPie.hovered >= 0
            width: Theme.snap(7 * pie.k, pie.dpr)
            height: width
            radius: width / 2
            x: Theme.snap(pie.cx + Math.sin(angle) * distance - width / 2, pie.dpr)
            y: Theme.snap(pie.cy - Math.cos(angle) * distance - height / 2, pie.dpr)
            color: pie.dropPie.picked >= 0 ? Theme.accentFill : Theme.faint
        }

        // The folders
        Repeater {
            model: pie.dropPie.sectors

            delegate: Rectangle {
                id: chip

                required property var modelData
                required property int index
                readonly property bool picked: index === pie.dropPie.picked
                readonly property bool hovered: index === pie.dropPie.hovered
                readonly property color ink: picked ? Theme.accentText : modelData.disabled ? Theme.faint : Theme.text

                x: pie.chipX(modelData)
                y: pie.chipY(modelData)
                width: Theme.snap(modelData.width, pie.dpr)
                height: pie.chipHeight
                radius: pie.chipRadius
                color: picked ? Theme.accentFill : hovered ? Theme.pieHover : Theme.menuBackground
                border.width: picked ? 0 : Theme.edge(pie.dpr)
                border.color: Theme.windowEdge

                Row {
                    x: 12 * pie.k
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6 * pie.k

                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: chip.modelData.disabled && pie.dropPie.noFolders ? "\uE734" : "\uE8B7" // FavoriteStar, Folder
                        size: 16 * pie.k
                        color: chip.ink
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: chip.modelData.label
                        textFormat: Text.PlainText
                        color: chip.ink
                        font.pixelSize: pie.labelSize
                    }
                    Text {
                        visible: text.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        text: chip.modelData.hint
                        textFormat: Text.PlainText
                        color: chip.picked ? Theme.accentText : Theme.subtext
                        font.pixelSize: pie.smallSize
                    }
                }
            }
        }

        // What letting go would do, under the chips (above them by the
        // screen's bottom).
        SoftShadow {
            x: status.x
            y: status.y
            width: status.width
            height: status.height
            radius: status.radius
        }

        Rectangle {
            id: status

            readonly property real padding: 10 * pie.k

            x: Theme.snap(pie.dropPie.statusRect.x, pie.dpr)
            y: Theme.snap(pie.dropPie.statusRect.y, pie.dpr)
            width: Theme.snap(pie.dropPie.statusRect.width, pie.dpr)
            height: Theme.snapUp(pie.dropPie.statusRect.height, pie.dpr)
            radius: Theme.largeControlRadius
            color: Theme.menuBackground
            border.width: Theme.edge(pie.dpr)
            border.color: Theme.windowEdge

            Column {
                x: status.padding
                y: Theme.snap((status.height - height) / 2, pie.dpr)
                width: status.width - 2 * status.padding
                spacing: 3

                Row {
                    width: parent.width
                    spacing: 6

                    Text {
                        id: statusVerb
                        visible: text.length > 0
                        text: pie.dropPie.statusVerb
                        color: Theme.subtext
                        font.pixelSize: pie.labelSize + 1
                    }
                    Text {
                        readonly property bool bold: pie.dropPie.hovered >= 0
                        text: pie.dropPie.elide(pie.dropPie.statusName, pie.labelSize + 1, bold,
                                                parent.width - (statusVerb.visible ? statusVerb.width + parent.spacing : 0))
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: pie.labelSize + 1
                        font.weight: bold ? Theme.fontStrong : Font.Normal
                    }
                }
                Item {
                    width: parent.width
                    height: detail.implicitHeight

                    Text {
                        id: detail
                        text: pie.dropPie.statusDetail
                        textFormat: Text.PlainText
                        color: Theme.subtext
                        font.pixelSize: pie.smallSize
                    }
                    Text {
                        anchors.right: parent.right
                        text: pie.dropPie.statusHint
                        textFormat: Text.PlainText
                        color: Theme.subtext
                        font.pixelSize: pie.smallSize
                    }
                }
            }
        }
    }
}
