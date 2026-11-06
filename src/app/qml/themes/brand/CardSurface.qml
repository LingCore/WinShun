import QtQuick

// card: a settings card (SettingRow/SettingsSection), plugin card, share
// card. White on the window (dark: one step up), radius 10, a faint 1 px
// neutral edge, flat — a card is not a button. Under the pointer only its
// edge firms a little. Rows inside a card are divided with `separator`.
Item {
    id: card

    property QtObject look
    property bool hovered: false
    property real corner: -1 // the host's radius, -1: the style's
    property real dpr: 1

    readonly property real radius: look ? (corner >= 0 ? corner : look.cardRadius) : 0

    Rectangle {
        anchors.fill: parent
        radius: card.radius
        color: card.look ? (card.hovered ? card.look.cardHover : card.look.cardFill) : "transparent"
        border.width: card.look ? card.look.edge(card.dpr) : 0
        border.color: card.look ? (card.hovered ? card.look.cardBorderHover : card.look.cardBorder) : "transparent"
        Behavior on color { ColorAnimation { duration: card.look ? card.look.animFast : 0 } }
    }
}
