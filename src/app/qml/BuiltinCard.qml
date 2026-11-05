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
    readonly property bool narrow: body.width < 520
    readonly property bool hasOptions: optionsColumn.children.length > 0

    signal toggled(bool on)
    signal linkActivated()

    implicitHeight: body.implicitHeight + 24
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
        y: 12
        width: parent.width - 28
        spacing: 12

        Item { // the header
            width: parent.width
            height: card.narrow ? texts.implicitHeight + 12 + headerControls.implicitHeight
                               : Math.max(texts.implicitHeight, headerControls.implicitHeight)

            Glyph {
                id: icon
                y: 2
                width: 32
                height: 32
                glyph: card.glyph
                size: 24
                color: Theme.text
            }

            Column {
                id: texts
                anchors.left: icon.right
                anchors.leftMargin: 12
                anchors.right: card.narrow ? parent.right : headerControls.left
                anchors.rightMargin: card.narrow ? 0 : 12
                spacing: 4

                Row {
                    width: parent.width
                    spacing: 8

                    Text {
                        width: Math.min(implicitWidth, Math.max(0, parent.width - builtinTag.width - parent.spacing))
                        text: card.title
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Theme.fontStrong
                        wrapMode: Text.Wrap
                    }
                    Tag {
                        id: builtinTag
                        text: qsTr("内置")
                    }
                }
                Text {
                    width: parent.width
                    text: card.description
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: card.status
                    color: card.checked ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            Row {
                id: headerControls
                y: card.narrow ? texts.implicitHeight + 12 : 0
                anchors.right: parent.right
                spacing: 12

                Item { // the button, right-aligned in buttonSlot
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(card.buttonSlot, foldButton.visible ? foldButton.implicitWidth : 0,
                                    linkButton.visible ? linkButton.implicitWidth : 0)
                    height: 36
                    visible: width > 0

                    FlatButton {
                        dpr: card.dpr
                        id: foldButton
                        anchors.right: parent.right
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
                        anchors.right: parent.right
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
                    dpr: card.dpr
                    anchors.verticalCenter: parent.verticalCenter
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
