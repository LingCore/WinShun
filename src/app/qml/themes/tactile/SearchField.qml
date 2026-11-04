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
    readonly property color textColor: look.ink
    readonly property color placeholderColor: look.ink3
    readonly property color iconColor: look.ink2
    readonly property color caretColor: look.caret
    readonly property real leftInset: look.grooveTextInset
    readonly property real keyInset: look.grooveKeyInsetAt(dpr)
    readonly property real keyRadius: look.grooveKeyRadius

    radius: corner >= 0 ? corner : look.searchRadius
}
