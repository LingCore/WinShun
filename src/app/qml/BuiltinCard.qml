import QtQuick
import WinShun

// A built-in feature on the 插件 page's 内置功能 (PluginsSection), drawn as
// a plugin's card is (PluginCard): icon, name, 内置, what it does and where
// it stands, its switch on the right. Its options, if any, fold
// under 设置 (`options`); a feature whose options live on another page
// takes the user there instead (`linkText`, linkActivated). The switch is
// the feature's own: the same setting as on its page.
Rectangle {
    id: card

    property string glyph
    property string title
    property string description
    property string status // where it stands now; empty: nothing said
    property bool checked: false
    property bool available: true // false: it cannot be turned on here now (the description says why)
    property string linkText // shown while on; empty: no link
    property bool expanded: false
    property bool marked: false // what the settings search found
    // The width kept left of the switch for 设置 or the link, the same on
    // every card of 内置功能: the switches stay in one column, and still
    // when a card's button comes, goes or changes its words.
    property real buttonSlot: 0
    default property alias options: optionsColumn.data
    // A tile of the 内置功能 grid (TileFlow), drawn as a plugin's tile is
    // (PluginTile): the name by the icon, the words under both, 设置 or the
    // link at the foot on the left, the switch on the right; no 内置, which
    // the grid's title says. Sized by the grid; unfolded it takes the whole
    // width (`wide`), its options under the same face. What is placed across
    // goes by `tile` alone, which never changes: a tile's words take its
    // width, make its height, and must not come round to its width again.
    property bool tile: false
    readonly property bool wide: tile && expanded
    readonly property bool tileFace: tile && !expanded
    readonly property bool narrow: tile || body.width < 520
    readonly property bool hasOptions: optionsColumn.children.length > 0

    signal toggled(bool on)
    signal linkActivated()

    implicitHeight: body.y + (tileFace ? texts.implicitHeight + 12 + headerControls.implicitHeight : body.implicitHeight) + 12
    readonly property real dpr: Screen.devicePixelRatio // one Screen attached object for all its items (GitHub #111)
    radius: Theme.controlRadius
    color: "transparent"

    Hook { // the style's card (Theme.cardPart), a plate inside the section's
        anchors.fill: parent
        part: Theme.cardPart
        corner: Theme.controlRadius
        extra: ({ inset: true })
        dpr: card.dpr
    }
    Rectangle { // found by the settings search: the card's face, marked
        visible: card.marked
        anchors.fill: parent
        anchors.margins: Theme.edge(card.dpr)
        radius: Math.max(0, Theme.controlRadius - Theme.edge(card.dpr))
        color: Theme.selection
    }

    Column {
        id: body
        x: 14
        y: card.tile ? 14 : 12 // a tile's name level with a plugin's (PluginTile)
        width: parent.width - 28
        spacing: 12

        Item { // the header; on a tile, all of it, the switch at its foot
            id: header
            width: parent.width
            height: card.tileFace ? card.height - body.y - 12
                  : card.narrow ? texts.implicitHeight + 12 + headerControls.implicitHeight
                  : Math.max(texts.implicitHeight, headerControls.implicitHeight)

            Glyph {
                id: icon
                y: card.tile ? 0 : 2
                width: card.tile ? 36 : 32 // a tile's name where a plugin tile's is
                height: width
                glyph: card.glyph
                size: 28
                color: Theme.text
            }

            Column {
                id: texts
                anchors.left: icon.right
                anchors.leftMargin: 12
                anchors.right: card.narrow ? parent.right : headerControls.left
                anchors.rightMargin: card.narrow ? 0 : 12
                spacing: card.tile ? 10 : 4

                // The name and 内置 after it; on a tile the name alone, beside
                // the icon's middle, and the words below run under the icon
                // too, the tile's width (a Column sets only y).
                Item {
                    width: parent.width
                    height: card.tile ? Math.max(icon.height, nameRow.height) : nameRow.height

                    Row {
                        id: nameRow
                        y: card.tile ? Theme.snap((parent.height - height) / 2, card.dpr) : 0
                        width: parent.width
                        spacing: 8

                        Text {
                            width: Math.min(implicitWidth, Math.max(0, parent.width - (card.tile ? 0 : builtinTag.width + parent.spacing)))
                            text: card.title
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            font.weight: Theme.fontStrong
                            wrapMode: Text.Wrap
                        }
                        Tag {
                            id: builtinTag
                            visible: !card.tile
                            text: qsTr("内置")
                        }
                    }
                }
                Text {
                    x: card.tile ? -texts.x : 0
                    width: card.tile ? header.width : parent.width
                    text: card.description
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                    lineHeight: card.tile ? 1.15 : 1 // as a plugin tile's
                }
                Text {
                    x: card.tile ? -texts.x : 0
                    width: card.tile ? header.width : parent.width
                    visible: text.length > 0
                    text: card.status
                    color: card.checked ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            Row {
                id: headerControls
                y: card.tileFace ? parent.height - implicitHeight : card.narrow ? texts.implicitHeight + 12 : 0
                anchors.right: parent.right
                spacing: 12

                // The button, right-aligned in buttonSlot; on a tile on the
                // left, its words under the words above (a flat button's
                // padding left out), as a plugin tile's 详情.
                Item {
                    anchors.verticalCenter: parent.verticalCenter
                    width: card.tile ? header.width - toggle.width - parent.spacing
                         : Math.max(card.buttonSlot, foldButton.visible ? foldButton.implicitWidth : 0,
                                    linkButton.visible ? linkButton.implicitWidth : 0)
                    height: card.tile ? 32 : 36
                    visible: width > 0

                    FlatButton {
                        dpr: card.dpr
                        id: foldButton
                        anchors.right: card.tile ? undefined : parent.right
                        anchors.left: card.tile ? parent.left : undefined
                        anchors.leftMargin: -12
                        anchors.verticalCenter: parent.verticalCenter
                        visible: card.checked && card.hasOptions
                        text: card.expanded ? qsTr("收起设置") : qsTr("设置")
                        trailingGlyph: String.fromCharCode(card.expanded ? 0xE70E : 0xE70D) // ChevronUp, ChevronDown
                        flat: true
                        Accessible.role: Accessible.Button
                        Accessible.name: card.expanded ? qsTr("收起%1设置").arg(card.title) : qsTr("展开%1设置").arg(card.title)
                        Accessible.onPressAction: clicked()
                        onClicked: {
                            forceActiveFocus(Qt.MouseFocusReason)
                            card.expanded = !card.expanded
                        }
                    }
                    FlatButton {
                        dpr: card.dpr
                        id: linkButton
                        anchors.right: card.tile ? undefined : parent.right
                        anchors.left: card.tile ? parent.left : undefined
                        anchors.leftMargin: -12
                        anchors.verticalCenter: parent.verticalCenter
                        visible: card.checked && card.linkText.length > 0
                        text: card.linkText
                        trailingGlyph: String.fromCharCode(0xE76C) // ChevronRight
                        flat: true
                        Accessible.role: Accessible.Link
                        Accessible.name: text
                        Accessible.onPressAction: clicked()
                        onClicked: card.linkActivated()
                    }
                }
                ToggleSwitch {
                    id: toggle
                    dpr: card.dpr
                    anchors.verticalCenter: parent.verticalCenter
                    word: !card.tile // as a plugin tile's: the track alone
                    enabled: card.available
                    opacity: enabled ? 1 : 0.5
                    checked: card.checked
                    Accessible.name: card.title
                    onToggled: (on) => {
                        card.toggled(on)
                        card.expanded = on && card.hasOptions // its ways in, in sight as it comes on
                    }
                }
            }
        }

        Column {
            visible: card.checked && card.expanded && card.hasOptions
            width: parent.width
            spacing: 12

            Hook { // between the header and the options (Theme.separator)
                width: parent.width
                height: Theme.edge(card.dpr)
                part: Theme.separator
                extra: ({ card: true })
                dpr: card.dpr
            }

            Column {
                id: optionsColumn
                width: parent.width
                spacing: 16
            }
        }
    }
}
