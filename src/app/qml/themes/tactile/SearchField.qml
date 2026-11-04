import QtQuick

// The launcher's search row (and every other search box): one groove cut
// into the panel, radius 8 (concentric with the 18 px window, 10 px away).
// Focus shows only as the orange caret; the keys inside sit 7 px from its
// top, bottom and right (grooveKeyInset), radius 2.
Groove {
    id: field

    property bool focused: false
    property bool hovered: false
    property bool flat: false      // the launcher's row: still a groove, 10 in (Look.fieldInset)
    property real corner: -1

    // for the host
    readonly property color textColor: look ? look.ink : "transparent"
    readonly property color placeholderColor: look ? look.ink3 : "transparent"
    readonly property color iconColor: look ? look.ink2 : "transparent"
    readonly property color caretColor: look ? look.caret : "transparent"
    readonly property real leftInset: look ? look.grooveTextInset : 0
    // Where the words start, from the groove's left edge: the 22 px magnifier
    // at grooveTextInset, then 12 (04-tactile.html .q).
    readonly property real leadingInset: look ? look.grooveTextInset + 22 + 12 : 0
    readonly property real iconInset: look ? look.grooveTextInset : 0 // the icon's left edge, from the groove's
    readonly property real keyInset: look ? look.grooveKeyInsetAt(dpr) : 0
    readonly property real keyRadius: look ? look.grooveKeyRadius : 0

    radius: corner >= 0 ? corner : look ? look.searchRadius : 0
}
