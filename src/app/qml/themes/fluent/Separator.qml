import QtQuick

// separator: a 1 px line between parts (a seam), centred in the host's box
// across it and as long as the box along it. `card` (Hook.extra): between
// the rows of a settings card, in the card's edge colour.
Item {
    id: part

    property QtObject look: null
    property int orientation: Qt.Horizontal
    property bool card: false
    property real dpr: 1

    readonly property real line: Math.max(1, Math.round(dpr)) / dpr

    Rectangle {
        readonly property bool across: part.orientation === Qt.Horizontal
        x: across ? 0 : Math.round((part.width - part.line) / 2 * part.dpr) / part.dpr
        y: across ? Math.round((part.height - part.line) / 2 * part.dpr) / part.dpr : 0
        width: across ? part.width : part.line
        height: across ? part.line : part.height
        color: !part.look ? "transparent" : part.card ? part.look.cardBorder : part.look.divider
    }
}
