pragma ComponentBehavior: Bound
import QtQuick

// The one physical part: a key. The box IS the visible key: a 1 px rim
// (darker along the bottom), a face with a vertical gradient and a 1 px top
// highlight, and a 2 px side drawn inside the bottom; a thin contact shadow
// tucked under it. Down (pressed or checked) the face sinks 2 px, a 2 px wall
// shows above it and the side goes; the outline never moves.
// The host centres its label on the box and adds contentOffsetY.
Item {
    id: key

    // state (Hook)
    property bool hovered: false
    property bool pressed: false
    property bool checked: false
    property bool highlighted: false // a primary button: an orange key, white words
    property bool danger: false      // an armed delete: a red key, white words
    property bool flat: false        // a toolbar icon button: nothing at rest, a key under the pointer
    property real corner: -1         // the host's radius (-1: by role)
    property string role             // "" | rowAction | fieldKey (in the search groove: radius 2) | toolbar | footer | dialog
    property real dpr: 1
    // tactile inputs (Hook.extra): a lamp at the left (scope keys, 不限) lit
    // while checked, the contact shadow (off for menu items), and a deep
    // colour the face takes when down (a pressed coloured type key).
    property bool indicator: false
    property bool drop: true
    property color downFill: "transparent"
    readonly property real radius: corner >= 0 ? corner : role === "fieldKey" ? look.grooveKeyRadius : look.keyRadius

    // for the host
    readonly property bool down: pressed || checked
    readonly property bool bare: flat && enabled && !hovered && !down
    readonly property real contentOffsetY: bare ? 0 : down ? look.keyTravel / 2 : -look.keyTravel / 2
    // A solid colour key: primary, armed delete, or a coloured key held down.
    readonly property color solid: danger ? look.dangerFillColor : highlighted ? look.accentFillColor
                                   : downFill.a > 0 && down ? downFill : "transparent"
    readonly property bool tinted: solid.a > 0 && enabled
    readonly property color foreground: !enabled ? look.ink3 : tinted ? "#FFFFFF" : (down || hovered) ? look.ink : look.keyInk
    readonly property color iconColor: foreground
    readonly property int fontWeight: checked && !tinted ? Font.Bold : Font.Normal
    readonly property real leadingInset: indicator ? look.ledInset + look.ledSize + look.ledGap : 0
    // Disabled keys draw themselves flat with faint words: no fading by the host.
    readonly property real disabledOpacity: 1

    property QtObject look // injected by Look.qml
    readonly property real hair: look.hair(dpr)
    readonly property real innerRadius: Math.max(0, radius - hair)
    readonly property real bodyHeight: height - 2 * hair

    // Face, rim and side colours. A solid key at rest keeps the material's
    // build (top light, side, darker bottom rim) in its own colour.
    readonly property color faceTop: !enabled ? look.keyDisabledFace : tinted ? (down ? solid : Qt.lighter(solid, 1.05))
                                     : down ? look.keyDownTop : look.keyFaceTop
    readonly property color faceBottom: !enabled ? look.keyDisabledFace : tinted ? solid : down ? look.keyDownBottom : look.keyFaceBottom
    readonly property color rimColor: tinted ? look.chipDownRim : look.keyRim
    readonly property color rimBottom: tinted ? look.chipDownWall : look.keyRimBottom
    readonly property color sideColor: !enabled ? look.keyDisabledFace : tinted ? Qt.darker(solid, 1.45) : look.keySideColor
    readonly property color capColor: tinted ? (down ? Qt.tint(solid, look.chipDownWall) : Qt.tint(faceTop, look.white(0.22)))
                                      : down ? Qt.tint(look.keyDownTop, look.keyWall)
                                      : Qt.tint(look.keyFaceTop, look.keyHighlight)

    Item {
        anchors.fill: parent
        visible: !key.bare

        // contact shadow: one soft device-pixel line under the bottom edge
        Rectangle {
            visible: key.drop && key.enabled && !key.down
            x: key.radius * 0.6
            y: key.height
            width: key.width - 2 * x
            height: key.hair
            color: key.look.keyDrop
            opacity: 0.1 // CSS 0 1px 1px -1px: barely there, it only seats the key
        }

        // rim, darker along the bottom; laid over the face colour, as a CSS
        // border lies over the element's own background
        Rectangle {
            anchors.fill: parent
            radius: key.radius
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.tint(key.faceTop, key.rimColor) }
                GradientStop { position: key.height > 0 ? Math.max(0, (key.height - key.radius - 1) / key.height) : 0.5; color: Qt.tint(key.faceBottom, key.rimColor) }
                GradientStop { position: 1; color: Qt.tint(key.faceBottom, key.down || !key.enabled ? key.rimColor : key.rimBottom) }
            }
        }

        // padding box: the side at rest
        Rectangle {
            x: key.hair
            y: key.hair
            width: key.width - 2 * key.hair
            height: key.bodyHeight
            radius: key.innerRadius
            color: key.sideColor
        }

        // top light at rest, the wall when down: the face below leaves a crescent of it
        Rectangle {
            visible: key.enabled
            x: key.hair
            y: key.hair
            width: key.width - 2 * key.hair
            height: key.down ? key.bodyHeight : key.bodyHeight - key.look.keyTravel
            radius: key.innerRadius
            color: key.capColor
        }

        // face
        Rectangle {
            id: face
            x: key.hair
            width: key.width - 2 * key.hair
            y: !key.enabled ? key.hair : key.down ? key.hair + key.look.keyTravel : 2 * key.hair
            height: !key.enabled ? key.bodyHeight : key.bodyHeight - key.look.keyTravel - (key.down ? 0 : key.hair)
            radius: key.innerRadius
            gradient: Gradient {
                GradientStop { position: 0; color: key.faceTop }
                GradientStop { position: 1; color: key.faceBottom }
            }
            Behavior on y { NumberAnimation { duration: key.look.animFast; easing.type: Easing.OutCubic } }
            Behavior on height { NumberAnimation { duration: key.look.animFast; easing.type: Easing.OutCubic } }
        }

        // indicator lamp, on the face (it travels with it)
        Loader {
            active: key.indicator
            x: key.look.snap(key.look.ledInset, key.dpr)
            y: key.look.snap(face.y + (face.height - key.look.ledSize) / 2, key.dpr)
            sourceComponent: Lamp {
                lit: key.checked
                dpr: key.dpr
                look: key.look
            }
        }
    }
}
