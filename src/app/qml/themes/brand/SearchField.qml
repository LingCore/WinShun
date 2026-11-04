import QtQuick
import QtQuick.Shapes
import QtQuick.Effects

// searchField: a search box. Unfocused: a white field with a 1 px neutral
// edge. Focused: one continuous 2 px ring in the Logo's colours, never broken
// or faded along the way (stops placed by perimeter length, Look.ringStops).
// flat: the launcher's search row — the host already sets the Hook `fieldInset`
// (10) inside the row on every side (themes/README.md 3.3), so the field fills
// it, radius 8, concentric with the window's 18; a faint glow of the ring's
// colours shows below it while focused. Not flat (settings, clipboard, plugins): the field
// fills the Hook, ring without glow. Radius: Look.fieldRadius (8).
Item {
    id: field

    property QtObject look
    property bool flat: false
    property bool focused: false
    property bool hovered: false
    property real dpr: 1

    // For the host: its corner and its colours.
    readonly property real radius: look ? look.fieldRadius : 0
    readonly property color caretColor: look ? look.caret : "transparent"
    readonly property color ink: look ? look.ink : "transparent"
    readonly property color placeholderInk: look ? look.ink3 : "transparent"
    readonly property color iconInk: look ? look.ink2 : "transparent"
    // Hook.out names. leadingInset: where the words start, from the field's
    // left edge (the 22 px magnifier at 13, then 25 to the words).
    readonly property color textColor: ink
    readonly property color placeholderColor: placeholderInk
    readonly property color iconColor: iconInk
    readonly property real leadingInset: 60
    readonly property real iconInset: 13 // the icon's left edge, from the field's
    readonly property real contentOffsetY: -2 // the 24 px query's optical centre

    property real lit: focused ? 1 : 0
    Behavior on lit { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: field.look ? field.look.easeOut : [0, 0, 0, 1, 1, 1] } }

    Item {
        id: box
        anchors.fill: parent

        // ---- glow (flat only): the ring's colours, blurred, peeking out below the field ----
        Shape {
            id: glowShape
            x: 8
            y: 0
            width: Math.max(0, box.width - 16)
            height: box.height + 4
            visible: false
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeWidth: -1
                fillGradient: RingGradient { look: field.look; boxWidth: glowShape.width; boxHeight: glowShape.height; count: 36 }
                PathRectangle { x: 0; y: 0; width: glowShape.width; height: glowShape.height; radius: field.look ? field.look.r(6) : 0 }
            }
        }
        MultiEffect {
            source: glowShape
            x: glowShape.x
            y: glowShape.y
            width: glowShape.width
            height: glowShape.height
            blurEnabled: true
            blurMax: 24
            blur: 1
            opacity: field.look ? field.look.glowOpacity * field.lit : 0
            visible: field.flat && field.lit > 0 // no offscreen layer while unfocused
        }

        // ---- the field ----
        Rectangle {
            anchors.fill: parent
            radius: field.radius
            color: field.look ? field.look.fieldFill : "transparent"
        }
        Rectangle { // 1 px neutral edge (fades as the ring comes)
            anchors.fill: parent
            radius: field.radius
            color: "transparent"
            border.width: field.look ? field.look.edge(field.dpr) : 0
            border.color: field.look ? (field.hovered && !field.focused ? field.look.controlBorder : field.look.fieldEdge) : "transparent"
            opacity: 1 - field.lit
            visible: opacity > 0
        }

        // ---- the ring: 2 px band, outer radius 8, inner 6 (concentric) ----
        Shape {
            id: ring
            anchors.fill: parent
            opacity: field.lit
            visible: field.lit > 0
            preferredRendererType: Shape.CurveRenderer
            readonly property real w: field.look ? field.look.snap(field.look.ringWidth, field.dpr) : 0
            ShapePath {
                strokeWidth: -1
                fillRule: ShapePath.OddEvenFill
                fillGradient: RingGradient { look: field.look; boxWidth: ring.width; boxHeight: ring.height }
                PathRectangle { x: 0; y: 0; width: ring.width; height: ring.height; radius: field.radius }
                PathRectangle { x: ring.w; y: ring.w; width: ring.width - 2 * ring.w; height: ring.height - 2 * ring.w; radius: Math.max(0, field.radius - ring.w) }
            }
        }
    }
}
