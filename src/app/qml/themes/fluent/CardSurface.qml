import QtQuick

// card: a settings card (SettingsSection, a plugin's, a share's), the card
// fill and a 1 px edge. `corner`: the host's radius (a card that joins
// another), or -1 for the style's. `inset`: a card inside a card (a
// plugin's in its section): the edge alone, no second fill over the first.
Rectangle {
    property QtObject look: null
    property bool hovered: false
    property real corner: -1
    property bool inset: false
    property real dpr: 1

    radius: corner >= 0 || !look ? Math.max(0, corner) : look.cardRadius
    color: look && !inset ? look.cardFill : "transparent"
    border.width: Math.max(1, Math.round(dpr)) / dpr
    border.color: look ? look.cardBorder : "transparent"
}
