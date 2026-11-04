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

    readonly property real radius: corner >= 0 ? corner : look.cardRadius

    Rectangle {
        anchors.fill: parent
        radius: card.radius
        color: card.hovered ? card.look.cardHover : card.look.cardFill
        border.width: card.look.edge(card.dpr)
        border.color: card.hovered ? card.look.cardBorderHover : card.look.cardBorder
        Behavior on color { ColorAnimation { duration: card.look.animFast } }
    }
}
