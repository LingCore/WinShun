import QtQuick

Item {
    id: part

    property QtObject look: null
    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    property bool mixed: false
    property bool active: true
    property bool animated: true
    property bool still: false
    property string kind
    property real dpr: 1
    readonly property bool lit: checked && !mixed && active

    function settle() {
        still = true
        knob.x = Qt.binding(() => knob.goalX)
        still = false
    }

    GlassSurface {
        anchors.fill: parent
        look: part.look
        dpr: part.dpr
        radius: !look ? 0 : part.kind === "check" ? look.controlRadius : look.pill(height)
        baseColor: !look ? "transparent" : part.lit ? look.accentFill : part.hovered ? look.controlHover : look.controlPressed
        edgeColor: !look ? "transparent" : part.lit ? look.accentFill : look.controlBorder
    }
    Rectangle {
        id: knob
        readonly property real goalX: part.mixed ? (part.width - width) / 2 : part.checked ? part.width - width - 3 : 3
        visible: part.kind !== "check" && part.active
        width: part.pressed ? 17 : 14
        height: 14
        x: goalX
        y: Math.round((part.height - height) * part.dpr / 2) / part.dpr
        radius: part.look ? part.look.pill(height) : 0
        color: !part.look ? "transparent" : part.lit ? part.look.accentText : part.look.subtext
        Behavior on x {
            enabled: part.animated && !part.still && part.look !== null && part.look.animations
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }
    }
    Text {
        visible: part.kind === "check" && part.lit
        anchors.centerIn: parent
        text: "\uE73E"
        font.family: part.look ? part.look.iconFont : ""
        font.pixelSize: Math.round(part.height * 0.6)
        color: part.look ? part.look.accentText : "transparent"
    }
}
