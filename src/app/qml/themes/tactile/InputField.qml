import QtQuick

// A form input: a smaller groove (radius 6) with a light cut line under its
// bottom edge so it reads on plates too. Focused: a 1 px neutral rim (no
// blue); error: the rim in the danger colour; disabled: flat, no shade.
Item {
    id: input

    property bool focused: false
    property bool hovered: false
    property bool error: false
    property real dpr: 1
    property real corner: -1
    readonly property real radius: corner >= 0 ? corner : look.keyRadius

    // for the host
    readonly property color textColor: enabled ? look.ink : look.ink3
    readonly property color placeholderColor: look.ink3
    readonly property color caretColor: look.caret

    property QtObject look // injected by Look.qml
    readonly property real hair: look.hair(dpr)

    // the light lip under the cut
    Rectangle {
        y: input.hair
        width: input.width
        height: input.height
        radius: input.radius
        color: input.look.seamLight
    }
    Groove {
        anchors.fill: parent
        radius: input.radius
        dpr: input.dpr
        look: input.look
        tone: input.enabled ? (input.focused || input.hovered ? Qt.tint(input.look.slot, input.look.dark ? Qt.rgba(1, 1, 1, 0.02) : Qt.rgba(1, 1, 1, 0.35)) : input.look.slot)
                            : input.look.panelBottom
        shade: input.enabled ? input.look.slotShade : "transparent"
        fade: input.enabled ? input.look.slotFade : "transparent"
    }
    Rectangle {
        visible: input.enabled && (input.focused || input.error)
        anchors.fill: parent
        radius: input.radius
        color: "transparent"
        border.width: input.hair
        border.color: input.error ? input.look.dangerInk : Qt.rgba(input.look.ink3.r, input.look.ink3.g, input.look.ink3.b, 0.6)
    }
}
