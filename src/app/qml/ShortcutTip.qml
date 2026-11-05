pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The shortcuts a footer has no room for, grouped, shown when its info button
// (ShortcutsButton) is clicked: the keys, and beside them what they do, and
// a last row that opens the settings' list of all of them. A window of its
// own (as ContextMenu), so it is not cut off by a launcher that is short
// (nothing typed: just the search box and the footer). It never takes the
// focus; the mouse works on its last row. `host` is the launcher or the
// clipboard: the screen area and the native styling of the window (rounded,
// shadowed).
Window {
    id: tip

    objectName: "shortcutTip"
    required property QtObject host
    // [{group, keys, text}, …] in their groups' order (Shortcuts.entries)
    property var rows: []

    readonly property int padding: 12

    signal settingsRequested()

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.menuBackground
    width: Math.ceil(Math.max(grid.implicitWidth, link.implicitWidth)) + 2 * padding
    height: Math.ceil(content.implicitHeight) + 2 * padding

    property Item button: null

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    // Over `button` (under it when there is no room above), its right
    // edge at the button's, kept on the screen.
    function popup(button) {
        tip.button = button
        grid.forceLayout() // its size now, not after the next frame: placed by it
        content.forceLayout()
        place()
        host.prepareMenuWindow(tip)
        show()
    }

    function place() {
        if (!button)
            return
        const gap = 6
        const pos = button.mapToGlobal(0, 0)
        const area = host.screenArea(pos)
        let x = pos.x + button.width - width
        let y = pos.y - height - gap
        if (area.width > 0) {
            if (y < area.y)
                y = pos.y + button.height + gap
            x = Math.max(area.x, Math.min(x, area.x + area.width - width))
            y = Math.max(area.y, Math.min(y, area.y + area.height - height))
        }
        tip.x = x
        tip.y = y
    }
    // The rows changed while shown (another number key setting): still over the button.
    onWidthChanged: if (visible) place()
    onHeightChanged: if (visible) place()

    Column {
        id: content
        x: tip.padding
        y: tip.padding
        spacing: 8

        Grid {
            id: grid
            columns: 2
            columnSpacing: 14
            rowSpacing: 6
            verticalItemAlignment: Grid.AlignVCenter

            Repeater {
                // Per row: its keys and what they do; a heading before the
                // first of each group (its cell beside it has no width, so
                // the heading runs over it).
                model: {
                    const cells = []
                    let group = ""
                    for (const row of tip.rows) {
                        if (row.group !== group) {
                            group = row.group
                            cells.push({ kind: "heading", text: group }, { kind: "none", text: "" })
                        }
                        cells.push({ kind: "keys", text: row.keys }, { kind: "label", text: row.text })
                    }
                    return cells
                }

                delegate: Loader {
                    id: cell

                    required property var modelData

                    sourceComponent: cell.modelData.kind === "keys" ? keysCell
                                   : cell.modelData.kind === "label" ? labelCell
                                   : cell.modelData.kind === "heading" ? headingCell : spacerCell

                    Component {
                        id: keysCell

                        Item { // a key cap, as the footer's (KeyHint)
                            id: cap
                            width: keyText.implicitWidth + 10
                            height: Theme.keycapHeight

                            Hook { // the style's key cap (Theme.keyHint): it only tells, never pressed
                                id: capPart
                                anchors.fill: parent
                                part: Theme.keyHint
                                dpr: Screen.devicePixelRatio
                            }

                            Text {
                                id: keyText
                                x: tip.onPixel((cap.width - width) / 2)
                                y: tip.onPixel((cap.height - height) / 2 + capPart.out("contentOffsetY", 0))
                                text: cell.modelData.text
                                color: capPart.out("foreground", Theme.subtext)
                                font.pixelSize: Theme.uiCaption
                            }
                        }
                    }

                    Component {
                        id: labelCell

                        Text {
                            text: cell.modelData.text
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                        }
                    }

                    Component {
                        id: spacerCell

                        Item { width: 1; height: 1 } // the cell beside a heading
                    }

                    Component {
                        id: headingCell

                        Item {
                            width: 1 // (a cell of no width is not placed at all)
                            height: heading.implicitHeight + 6

                            Text {
                                id: heading
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 2
                                text: cell.modelData.text
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                font.weight: Theme.fontStrong
                            }
                        }
                    }
                }
            }
        }

        Hook { // the style's seam
            width: grid.width
            height: Theme.edge(Screen.devicePixelRatio)
            part: Theme.separator
            dpr: Screen.devicePixelRatio
        }

        Item { // all of them, in the settings
            id: link
            width: grid.width
            implicitWidth: linkText.implicitWidth
            height: linkText.implicitHeight + 8

            Text {
                id: linkText
                objectName: "shortcutLink"
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("全部快捷键…") + "  ›"
                color: linkArea.pressed ? Theme.subtext : Theme.accent
                font.pixelSize: Theme.fontCaption
                font.underline: linkArea.containsMouse
            }

            MouseArea {
                id: linkArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: tip.settingsRequested()
            }
        }
    }
}
